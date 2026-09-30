
#include "DisplayComponent.h"
#include "DisplayManager.h"
#include "settings/SettingsComponent.h"
#include <QGuiApplication>
#include <QWindow>
#include <QDebug>
#include <math.h>

#ifdef Q_OS_MAC
#include "osx/DisplayManagerOSX.h"
#elif defined(TARGET_RPI)
#include "rpi/DisplayManagerRPI.h"
#elif defined(USE_X11XRANDR)
#include "x11/DisplayManagerX11.h"
#elif defined(Q_OS_WIN)
#include "win/DisplayManagerWin.h"
#endif

#ifdef USE_KDE_WAYLAND
#include "kde/DisplayManagerKDE.h"
#endif

#include "dummy/DisplayManagerDummy.h"
#include "input/InputComponent.h"

///////////////////////////////////////////////////////////////////////////////////////////////////
DisplayComponent::DisplayComponent(QObject* parent) : ComponentBase(parent), m_initTimer(this)
{
  m_displayManager = nullptr;
  m_lastVideoMode = -1;
  m_lastDisplay = -1;
  m_applicationWindow = nullptr;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
DisplayComponent::~DisplayComponent()
{
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool DisplayComponent::initializeDisplayManager()
{
  m_initTimer.setSingleShot(false);

  bool res = false;
  if (m_displayManager)
    res = m_displayManager->initialize();

  emit refreshRateChanged();

  return res;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool DisplayComponent::componentInitialize()
{
#ifdef USE_KDE_WAYLAND
  // XRandR under Xwayland only sees emulated outputs, so use KWin's protocols when available.
  if (QGuiApplication::platformName() == "wayland")
  {
    m_displayManager = new DisplayManagerKDE(this);
    if (!m_displayManager->initialize())
    {
      delete m_displayManager;
      m_displayManager = nullptr;
    }
  }
#endif

  if (!m_displayManager)
  {
#if 0
    m_displayManager = new DisplayManagerDummy(this);
#elif defined(Q_OS_MAC)
    m_displayManager = new DisplayManagerOSX(this);
#elif defined(TARGET_RPI)
    m_displayManager = new DisplayManagerRPI(this);
#elif defined(USE_X11XRANDR)
    m_displayManager = new DisplayManagerX11(this);
#elif defined(Q_OS_WIN)
    m_displayManager = new DisplayManagerWin(this);
#endif
  }

  if (initializeDisplayManager())
  {
    auto* app = qobject_cast<QGuiApplication*>(QGuiApplication::instance());

    connect(app, SIGNAL(screenAdded(QScreen*)), this, SLOT(monitorChange()));
    connect(app, SIGNAL(screenRemoved(QScreen*)), this,  SLOT(monitorChange()));

    for(QScreen *screen : app->screens())
    {
      connect(screen, SIGNAL(refreshRateChanged(qreal)), this, SLOT(monitorChange()));
      connect(screen, SIGNAL(geometryChanged(QRect)), this, SLOT(monitorChange()));
    }

    // Quitting during playback would leave the display in the video's mode.
    connect(app, &QGuiApplication::aboutToQuit, this, &DisplayComponent::restorePreviousVideoMode);
    restoreAfterCrash();

#ifdef TARGET_RPI
    // The firmware doesn't always make the best decision. Hope we do better.
    qInfo() << "Trying to switch to best display mode.";
    switchToBestOverallVideoMode(0);
#endif

    return true;
  }

  return false;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
void DisplayComponent::monitorChange()
{
  qInfo() << "Monitor change detected.";

  if (!m_initTimer.isSingleShot())
  {
    m_initTimer.setSingleShot(true);
    m_initTimer.singleShot(1000, this, SLOT(initializeDisplayManager()));
  }
}

//////////////////////////////////////////////////////////////////////////////////////////////////
bool DisplayComponent::switchToBestVideoMode(float frameRate)
{
  initializeDisplayManager();

  if (!m_displayManager)
    return false;

  int currentDisplay = getApplicationDisplay();
  if (currentDisplay < 0)
  {
    qInfo() << "Not switching rate - current display not found.";
    return false;
  }

  int currentMode = m_displayManager->getCurrentDisplayMode(currentDisplay);
  if (m_lastVideoMode < 0)
  {
    m_lastVideoMode = currentMode;
    m_lastDisplay = currentDisplay;
  }

  qDebug() << "Current display:" << currentDisplay << "mode:" << currentMode;

  DMMatchMediaInfo matchInfo(frameRate, false);
  int bestmode = m_displayManager->findBestMatch(currentDisplay, matchInfo);
  if (bestmode >= 0)
  {
    if (bestmode != currentMode)
    {
      qDebug()
      << "Best video matching mode is "
      << m_displayManager->m_displays[currentDisplay]->m_videoModes[bestmode]->getPrettyName()
      << "on display" << currentDisplay;

      if (!m_displayManager->setDisplayMode(currentDisplay, bestmode))
      {
        qInfo() << "Mode switching failed.";
        return false;
      }
      saveRestoreState(currentDisplay, false);
      return true;
    }
    qInfo() << "No better video mode than the currently active one found.";
  }
  else
  {
    qDebug() << "No video mode found as better match.";
  }

  return false;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
bool DisplayComponent::switchToBestOverallVideoMode(int display)
{
  initializeDisplayManager();

  if (!m_displayManager || !m_displayManager->isValidDisplay(display))
    return false;

  if (!SettingsComponent::Get().value(SETTINGS_SECTION_MAIN, "hdmi_poweron").toBool())
  {
    qInfo() << "Switching to best mode disabled.";
    return false;
  }

  int bestmode = m_displayManager->findBestMode(display);
  if (bestmode < 0)
    return false;

  qInfo() << "We think mode" << bestmode << "is the best mode.";

  if (bestmode == m_displayManager->getCurrentDisplayMode(display))
  {
    qInfo() << "This mode is the currently active one. Not switching.";
    return false;
  }

  if (!m_displayManager->setDisplayMode(display, bestmode))
  {
    qInfo() << "Switching mode failed.";
    return false;
  }

  qInfo() << "Switching mode successful.";
  return true;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
double DisplayComponent::currentRefreshRate()
{
  if (!m_displayManager)
    return 0;

  int currentDisplay = getApplicationDisplay();
  if (currentDisplay < 0)
    return 0;
  int mode = m_displayManager->getCurrentDisplayMode(currentDisplay);
  if (mode < 0)
    return 0;
  return m_displayManager->m_displays[currentDisplay]->m_videoModes[mode]->m_refreshRate;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
bool DisplayComponent::switchToHdrForMedia(bool hdrMedia)
{
  initializeDisplayManager();

  if (!m_displayManager)
    return false;

  int currentDisplay = getApplicationDisplay();
  if (currentDisplay < 0)
  {
    qInfo() << "Not switching HDR - current display not found.";
    return false;
  }
  if (!m_displayManager->switchHdrForMedia(currentDisplay, hdrMedia))
    return false;
  saveRestoreState(currentDisplay, true);
  return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
qreal DisplayComponent::sdrWhiteLevel()
{
  // No initializeDisplayManager() here: it emits refreshRateChanged, which makes the
  // player reconfigure video and ask again.
  int display = m_displayManager ? getApplicationDisplay(true) : -1;
  return display >= 0 ? m_displayManager->sdrWhiteLevel(display) : 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
qreal DisplayComponent::peakLuminance()
{
  int display = m_displayManager ? getApplicationDisplay(true) : -1;
  return display >= 0 ? m_displayManager->peakLuminance(display) : 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool DisplayComponent::restorePreviousVideoMode()
{
  initializeDisplayManager();

  if (!m_displayManager)
    return false;

  // Independent of the mode: HDR may have been switched without a mode change.
  m_displayManager->restoreHdr();
  SettingsComponent::Get().setValue(SETTINGS_SECTION_STATE, "displayRestore", QVariant());

  if (!m_displayManager->isValidDisplayMode(m_lastDisplay, m_lastVideoMode))
    return false;

  bool ret = true;

  if (m_displayManager->getCurrentDisplayMode(m_lastDisplay) != m_lastVideoMode)
  {
    qDebug()
    << "Restoring VideoMode to"
    << m_displayManager->m_displays[m_lastDisplay]->m_videoModes[m_lastVideoMode]->getPrettyName()
    << "on display" << m_lastDisplay;

    ret =  m_displayManager->setDisplayMode(m_lastDisplay, m_lastVideoMode);
  }

  m_lastVideoMode = -1;
  m_lastDisplay = -1;

  return ret;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// What restorePreviousVideoMode() would undo, saved in storage.json so the next start can undo
// it if this session ends without restoring (crash, kill).
void DisplayComponent::saveRestoreState(int display, bool hdrSwitched)
{
  QVariantMap state = SettingsComponent::Get().value(SETTINGS_SECTION_STATE, "displayRestore").toMap();
  state["display"] = m_displayManager->m_displays[display]->m_name;
  if (hdrSwitched)
    state["hdrOff"] = true;
  if (m_lastDisplay == display && m_displayManager->isValidDisplayMode(display, m_lastVideoMode))
  {
    const DMVideoModePtr mode = m_displayManager->m_displays[display]->m_videoModes[m_lastVideoMode];
    state["width"] = mode->m_width;
    state["height"] = mode->m_height;
    state["refreshRate"] = mode->m_refreshRate;
  }
  SettingsComponent::Get().setValue(SETTINGS_SECTION_STATE, "displayRestore", state);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// ponytail: restores even if the user changed the mode by hand since the crash; store the mode
// switched to and compare if that turns out to annoy anyone.
void DisplayComponent::restoreAfterCrash()
{
  QVariantMap state = SettingsComponent::Get().value(SETTINGS_SECTION_STATE, "displayRestore").toMap();
  if (state.isEmpty() || !m_displayManager)
    return;

  qInfo() << "The last session didn't restore the display, restoring" << state;
  for (auto it = m_displayManager->m_displays.begin(); it != m_displayManager->m_displays.end(); ++it)
  {
    if (it.value()->m_name != state["display"].toString())
      continue;

    if (state["hdrOff"].toBool() && m_displayManager->isHdrEnabled(it.key()))
      m_displayManager->setHdrEnabled(it.key(), false);

    for (auto m = it.value()->m_videoModes.begin(); m != it.value()->m_videoModes.end(); ++m)
    {
      const DMVideoModePtr& mode = m.value();
      if (state.contains("width") && mode->m_width == state["width"].toInt() &&
          mode->m_height == state["height"].toInt() &&
          qFuzzyCompare(mode->m_refreshRate, state["refreshRate"].toFloat()))
      {
        if (m.key() != m_displayManager->getCurrentDisplayMode(it.key()))
          m_displayManager->setDisplayMode(it.key(), m.key());
        break;
      }
    }
  }
  SettingsComponent::Get().setValue(SETTINGS_SECTION_STATE, "displayRestore", QVariant());
}


//////////////////////////////////////////////////////////////////////////////////////////////////
int DisplayComponent::getApplicationDisplay(bool silent)
{
  QWindow* activeWindow = m_applicationWindow;

  int display = -1;
  if (activeWindow && m_displayManager)
  {
    if (!silent)
    {
      qInfo() << "Looking for a display at:" << activeWindow->geometry()
                   << "(center:" << activeWindow->geometry().center() << ")";
    }
    display = m_displayManager->getDisplayFromWindow(activeWindow);
  }

  if (!silent)
  {
    qInfo() << "Display index:" << display;
  }
  return display;
}

/////////////////////////////////////////////////////////////////////////////////////////
QString DisplayComponent::displayName(int display)
{
  if (display < 0)
    return "(not found)";
  QString id = QString("#%0 ").arg(display);
  if (m_displayManager->isValidDisplay(display))
    return id + m_displayManager->m_displays[display]->m_name;
  else
    return id + "(not valid)";
}

/////////////////////////////////////////////////////////////////////////////////////////
QString DisplayComponent::modePretty(int display, int mode)
{
  if (mode < 0)
    return "(not found)";
  QString id = QString("#%0 ").arg(mode);
  if (m_displayManager->isValidDisplayMode(display, mode))
    return id + m_displayManager->m_displays[display]->m_videoModes[mode]->getPrettyName();
  else
    return id + "(not valid)";
}

/////////////////////////////////////////////////////////////////////////////////////////
QString DisplayComponent::debugInformation()
{
  QString debugInfo;
  QTextStream stream(&debugInfo);
  stream << "Display\n";

  if (!m_displayManager)
  {
    stream << "  (no DisplayManager initialized)\n";
  }
  else
  {
    int display = getApplicationDisplay(true);
    int mode = display < 0 ? -1 : m_displayManager->getCurrentDisplayMode(display);

    stream << "  Current screen: " << displayName(display) << "\n";
    if (display >= 0)
      stream << "  Current mode: " << modePretty(display, mode) << "\n";
    if (m_displayManager->isValidDisplayMode(m_lastDisplay, m_lastVideoMode))
    {
      stream << "  Switch back on screen: " << displayName(m_lastDisplay) << "\n";
      stream << "  Switch back to mode: " << modePretty(m_lastDisplay, m_lastVideoMode) << "\n";
    }
  }

  stream << "\n";
  stream.flush();
  return debugInfo;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
static float modeDistance(const DMVideoMode& m1, const DMVideoMode& m2)
{
  if (m1.m_height == m2.m_height &&
      m1.m_width == m2.m_width &&
      m1.m_bitsPerPixel == m2.m_bitsPerPixel &&
      m1.m_interlaced == m2.m_interlaced)
    return fabs(m1.m_refreshRate - m2.m_refreshRate);
  return -1;
}

/////////////////////////////////////////////////////////////////////////////////////////
void DisplayComponent::switchCommand(QString command)
{
  if (!m_displayManager)
  {
    qCritical() << "Display manager not set";
    return;
  }

  if (!initializeDisplayManager())
  {
    qCritical() << "Could not reinitialize display manager";
    return;
  }

  int currentDisplay = getApplicationDisplay();
  if (currentDisplay < 0)
  {
    qCritical() << "Current display not found";
    return;
  }
  int id = m_displayManager->getCurrentDisplayMode(currentDisplay);
  if (id < 0)
  {
    qCritical() << "Current mode not found";
    return;
  }
  DMVideoMode currentMode = *m_displayManager->m_displays[currentDisplay]->m_videoModes[id];
  DMVideoMode mode = currentMode;
  int bestMode = -1; // if -1, select it by using the mode variable above

  for(QString a : command.split(" "))
  {
    a = a.trimmed();
    if (a == "p")
    {
      mode.m_interlaced = false;
    }
    else if (a == "i")
    {
      mode.m_interlaced = true;
    }
    else if (a.endsWith("hz"))
    {
      a = a.mid(0, a.size() - 2);
      bool ok;
      float rate = a.toFloat(&ok);
      if (ok)
        mode.m_refreshRate = rate;
    }
    else if (a.startsWith("mode="))
    {
      a = a.mid(5);
      bool ok;
      int newId = a.toInt(&ok);
      if (ok && m_displayManager->isValidDisplayMode(currentDisplay, newId))
        bestMode = newId;
    }
    else if (a.indexOf("x") >= 0)
    {
      QStringList sub = a.split("x");
      if (sub.size() != 2)
        continue;
      bool ok;
      int w = sub[0].toInt(&ok);
      if (!ok)
        continue;
      int h = sub[1].toInt(&ok);
      if (!ok)
        continue;
      mode.m_width = w;
      mode.m_height = h;
    }
  }

  qInfo() << "Current mode:" << currentMode.getPrettyName();

  if (bestMode < 0)
  {
    qInfo() << "Mode requested by command:" << mode.getPrettyName();

    for(auto cur : m_displayManager->m_displays[currentDisplay]->m_videoModes)
    {
      // "Score" according to what was requested
      float dCur = modeDistance(*cur, mode);
      if (dCur < 0)
        continue;
      if (bestMode < 0)
      {
        bestMode = cur->m_id;
      }
      else
      {
        // "Score" according to best mode
        float dBest = modeDistance(*m_displayManager->m_displays[currentDisplay]->m_videoModes[bestMode],
                                    mode);
        if (dCur < dBest)
          bestMode = cur->m_id;
      }
    }
  }

  if (bestMode >= 0)
  {
    qInfo() << "Found mode to switch to:" << m_displayManager->m_displays[currentDisplay]->m_videoModes[bestMode]->getPrettyName();
    if (m_displayManager->setDisplayMode(currentDisplay, bestMode))
    {
      m_lastDisplay = m_lastVideoMode = -1;
    }
    else
    {
      qInfo() << "Switching failed.";
    }
    return;
  }

  qInfo() << "Requested mode not found.";
}

/////////////////////////////////////////////////////////////////////////////////////////
void DisplayComponent::componentPostInitialize()
{
  InputComponent::Get().registerHostCommand("switch", this, "switchCommand");

#ifdef TARGET_RPI
  if (m_displayManager)
    InputComponent::Get().registerHostCommand("recreateRpiUI", m_displayManager, "resetRendering");
#endif
}
