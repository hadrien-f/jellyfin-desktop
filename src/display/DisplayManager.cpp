//
//  DisplayManager.cpp
//  konvergo
//
//  Created by Lionel CHAZALLON on 28/09/2014.
//
//

#include <QDebug>
#include <algorithm>
#include <cmath>
#include <QWindow>
#include "DisplayManager.h"
#include "math.h"
#include "settings/SettingsComponent.h"

///////////////////////////////////////////////////////////////////////////////////////////////////
DisplayManager::DisplayManager(QObject* parent) : QObject(parent) {}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool DisplayManager::initialize()
{
  qInfo() << QString("DisplayManager found %1 Display(s).").arg(m_displays.size());

  // list video modes
  for(int displayid : m_displays.keys())
  {
    DMDisplayPtr display = m_displays[displayid];
    qInfo() << QString("Available modes for Display #%1 (%2)").arg(displayid).arg(display->m_name);
    for (int modeid = 0; modeid < display->m_videoModes.size(); modeid++)
    {
      DMVideoModePtr mode = display->m_videoModes[modeid];
      qInfo() << QString("Mode %1: %2").arg(modeid, 2).arg(mode->getPrettyName());
    }
  }

  // Log current display mode
  int mainDisplay = getMainDisplay();
  if (mainDisplay >= 0)
  {
    int currentMode = getCurrentDisplayMode(mainDisplay);
    if (currentMode >= 0)
      qInfo() << QString("DisplayManager : Current Display Mode on Display #%1 is %2")
                     .arg(mainDisplay)
                     .arg(m_displays[mainDisplay]->m_videoModes[currentMode]->getPrettyName());
    else
      qCritical() << "DisplayManager : unable to retrieve current video mode";
  }
  else
    qCritical() << "DisplayManager : unable to retrieve main display";

  return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
DMVideoModePtr DisplayManager::getCurrentVideoMode(int display)
{
  int currentMode = getCurrentDisplayMode(display);
  DMVideoModePtr currentVideoMode;

  if (currentMode >= 0)
    currentVideoMode = m_displays[display]->m_videoModes[currentMode];

  return currentVideoMode;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool DisplayManager::isValidDisplay(int display) { return m_displays.contains(display); }

///////////////////////////////////////////////////////////////////////////////////////////////////
bool DisplayManager::isValidDisplayMode(int display, int mode)
{
  if (isValidDisplay(display))
    if (mode >= 0 && mode < m_displays[display]->m_videoModes.size())
      return true;

  return false;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
qreal DisplayManager::dolbyVisionPeakLuminance(const QByteArray& edid)
{
  // Layout as decoded by edid-decode (cta_dolby_video in parse-cta-block.cpp).
  auto pqToNits = [](double pq) {
    const double m1 = 2610.0 / 16384, m2 = 2523.0 / 4096 * 128;
    const double c1 = 3424.0 / 4096, c2 = 2413.0 / 4096 * 32, c3 = 2392.0 / 4096 * 32;
    double e = std::pow(pq, 1 / m2);
    return 10000 * std::pow(std::max(e - c1, 0.0) / (c2 - c3 * e), 1 / m1);
  };
  const auto* d = reinterpret_cast<const unsigned char*>(edid.constData());
  for (qsizetype b = 128; b + 128 <= edid.size(); b += 128)
  {
    if (d[b] != 0x02) // CTA-861 extension
      continue;
    const qsizetype end = b + d[b + 2];
    for (qsizetype i = b + 4; i < end && i < b + 127; i += 1 + (d[i] & 31))
    {
      const int len = d[i] & 31;
      // Extended tag 0x01 (vendor-specific video), OUI 00-D0-46 (Dolby), stored LSB first.
      if (d[i] >> 5 != 7 || len < 5 || i + 1 + len > b + 127 || d[i + 1] != 0x01 ||
          d[i + 2] != 0x46 || d[i + 3] != 0xd0 || d[i + 4] != 0x00)
        continue;
      const unsigned char* x = d + i + 5;
      const int n = len - 4;
      switch ((x[0] >> 5) & 7)
      {
        case 0:
          if (n > 15)
            return pqToNits(((x[15] << 4) | (x[13] & 0xf)) / 4095.0);
          break;
        case 1:
          if (n > 1)
            return 100 + (x[1] >> 1) * 50;
          break;
        case 2:
          if (n > 2)
            return pqToNits((2055 + 65 * (x[2] >> 3)) / 4095.0);
          break;
      }
    }
  }
  return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool DisplayManager::switchHdrForMedia(int display, bool hdrMedia)
{
  if (!hdrMedia || !isValidDisplay(display))
    return false;

  if (!isHdrCapable(display))
  {
    qInfo() << "Not switching to HDR - display" << display << "is not HDR capable.";
    return false;
  }
  if (isHdrEnabled(display))
    return false;

  qInfo() << "Switching display" << display << "to HDR.";
  if (!setHdrEnabled(display, true))
  {
    qInfo() << "Switching to HDR failed.";
    return false;
  }
  m_hdrSwitchedDisplay = display;
  return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool DisplayManager::restoreHdr()
{
  if (m_hdrSwitchedDisplay < 0)
    return false;

  int display = m_hdrSwitchedDisplay;
  m_hdrSwitchedDisplay = -1;
  if (!isValidDisplay(display) || !isHdrEnabled(display))
    return false;

  qInfo() << "Switching display" << display << "back to SDR.";
  return setHdrEnabled(display, false);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// refresh: video FPS
// multiple: display FPS
bool DisplayManager::isRateMultipleOf(float refresh, float multiple, bool exact)
{
  long roundedRefresh = lrint(refresh);
  long roundedMultiple = lrint(multiple);

  if (roundedRefresh == 0)
      return false;

  long factor = roundedMultiple / roundedRefresh;
  float newRate = factor * refresh;
  if (newRate < 1)
    return false;

  float tolerance = exact ? 0.01 * factor : 1;

  return fabs(newRate - multiple) < tolerance;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
int DisplayManager::findBestMatch(int display, DMMatchMediaInfo& matchInfo)
{
  bool avoid_25_30 = SettingsComponent::Get().value(SETTINGS_SECTION_VIDEO, "refreshrate.avoid_25hz_30hz").toBool();

  // Grab current videomode information
  DMVideoModePtr currentVideoMode = getCurrentVideoMode(display);
  if (!currentVideoMode)
    return -1;

  // now we try to find the exact match in current resolution
  // then fill a list
  DMVideoModeWeightMap weights;

  DMVideoModeMap::const_iterator modeit = m_displays[display]->m_videoModes.constBegin();

  while (modeit != m_displays[display]->m_videoModes.constEnd())
  {
    DMVideoModePtr candidate = modeit.value();

    // avoid switching to 30 fps (prefer a multiple - 60Hz is ideal)
    // the intention is also to match 30/1.001
    if ((fabs(candidate->m_refreshRate - 30.0) < 0.5) ||
        (fabs(candidate->m_refreshRate - 25.0) < 0.5))
    {
      if (avoid_25_30)
      {
        qInfo() << "DisplayManager RefreshMatch : skipping rate " << candidate->m_refreshRate << "as requested";
        modeit++;
        continue;
      }
    }

    weights[candidate->m_id] = DMVideoModeWeightPtr(new DMVideoModeWeight);
    weights[candidate->m_id]->m_mode = candidate;
    weights[candidate->m_id]->m_weight = 0;

    // Weight Resolution match
    if ((candidate->m_width == currentVideoMode->m_width) &&
        (candidate->m_height == currentVideoMode->m_height) &&
        (candidate->m_bitsPerPixel == currentVideoMode->m_bitsPerPixel))
    {
      weights[candidate->m_id]->m_weight += MATCH_WEIGHT_RES;
    }

    // weight refresh rate
    // exact Match
    if (fabs(candidate->m_refreshRate - matchInfo.m_refreshRate) <= 0.01)
      weights[candidate->m_id]->m_weight += MATCH_WEIGHT_REFRESH_RATE_EXACT;

    // exact multiple refresh rate
    if (isRateMultipleOf(matchInfo.m_refreshRate, candidate->m_refreshRate, true))
      weights[candidate->m_id]->m_weight += MATCH_WEIGHT_REFRESH_RATE_MULTIPLE;

    // close refresh match (less than 1 hz diff to match all 23.xxx modes to 24p)
    if (fabs(candidate->m_refreshRate - matchInfo.m_refreshRate) <= 0.5)
    {
      weights[candidate->m_id]->m_weight += MATCH_WEIGHT_REFRESH_RATE_CLOSE;
    }

    // approx multiple refresh rate
    if (isRateMultipleOf(matchInfo.m_refreshRate, candidate->m_refreshRate, false))
      weights[candidate->m_id]->m_weight += MATCH_WEIGHT_REFRESH_RATE_MULTIPLE_CLOSE;

    // weight interlacing
    if (candidate->m_interlaced == matchInfo.m_interlaced)
      weights[candidate->m_id]->m_weight += MATCH_WEIGHT_INTERLACE;

    if (candidate->m_id == currentVideoMode->m_id)
      weights[candidate->m_id]->m_weight += MATCH_WEIGHT_CURRENT;

    modeit++;
  }

  // now grab the mode with the highest weight
  DMVideoModeWeightPtr chosen;
  float maxWeight = 0;

  DMVideoModeWeightMap::const_iterator weightit = weights.constBegin();
  while (weightit != weights.constEnd())
  {
    qDebug() << "Mode " << weightit.value()->m_mode->m_id << "("
                 << weightit.value()->m_mode->getPrettyName() << ") has weight "
                 << weightit.value()->m_weight;
    if (weightit.value()->m_weight > maxWeight)
    {
      chosen = weightit.value();
      maxWeight = chosen->m_weight;
    }

    weightit++;
  }

  if ((chosen) && (chosen->m_weight > MATCH_WEIGHT_RES))
  {
    qInfo() << "DisplayManager RefreshMatch : found a suitable mode : "
                << chosen->m_mode->getPrettyName();
    return chosen->m_mode->m_id;
  }

  qInfo() << "DisplayManager RefreshMatch : found no suitable videomode";
  return -1;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
int DisplayManager::findBestMode(int display)
{
  int bestMode = -1;

  for(auto mode : m_displays[display]->m_videoModes)
  {
    if (bestMode < 0)
    {
      bestMode = mode->m_id;
    }
    else
    {
      DMVideoModePtr best = m_displays[display]->m_videoModes[bestMode];
      DMVideoModePtr candidate = mode;

      // Highest priority: prefer non-interlaced modes.
      if (!best->m_interlaced && candidate->m_interlaced)
        continue;

      if (best->m_bitsPerPixel > candidate->m_bitsPerPixel)
        continue;

      if (best->m_width > candidate->m_width)
        continue;

      if (best->m_height > candidate->m_height)
        continue;

      if (best->m_refreshRate > candidate->m_refreshRate)
        continue;

      bestMode = candidate->m_id;
    }
  }

  return bestMode;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
int DisplayManager::getDisplayFromPoint(const QPoint& pt)
{
  return getDisplayFromPoint(pt.x(), pt.y());
}

///////////////////////////////////////////////////////////////////////////////////////////////////
int DisplayManager::getDisplayFromWindow(QWindow* window)
{
  return window ? getDisplayFromPoint(window->geometry().center()) : -1;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
int DisplayManager::findDisplayByName(const QString& name)
{
  for (const DMDisplayPtr& display : m_displays)
  {
    if (display->m_name == name)
      return display->m_id;
  }
  return -1;
}
