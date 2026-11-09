#include <QtTest/QtTest>
#include "../src/display/DisplayManager.h"
#include "../src/settings/SettingsComponent.h"
#include "../src/settings/SettingsSection.h"
#include "../src/settings/SettingsValue.h"

// Implements the pure virtuals (and some helpers) to test DisplayManager logic.
class TestableDisplayManager : public DisplayManager
{
public:
  explicit TestableDisplayManager() : DisplayManager(nullptr), m_currentMode(0) {}

  bool setDisplayMode(int, int) override { return true; }
  int getCurrentDisplayMode(int) override { return m_currentMode; }
  int getMainDisplay() override { return 0; }
  int getDisplayFromPoint(int, int) override { return 0; }

  void setCurrentMode(int mode) { m_currentMode = mode; }

  bool isHdrCapable(int) override { return m_hdrCapable; }
  bool isHdrEnabled(int) override { return m_hdrEnabled; }
  bool setHdrEnabled(int, bool enable) override
  {
    m_hdrSwitches++;
    m_hdrEnabled = enable;
    return true;
  }

  bool m_hdrCapable = true;
  bool m_hdrEnabled = false;
  int m_hdrSwitches = 0;

  void addMode(int id, int w, int h, int bpp, float hz, bool interlaced)
  {
    auto mode = DMVideoModePtr::create();
    mode->m_id = id;
    mode->m_width = w;
    mode->m_height = h;
    mode->m_bitsPerPixel = bpp;
    mode->m_refreshRate = hz;
    mode->m_interlaced = interlaced;
    mode->m_privId = id;

    if (!m_displays.contains(0))
    {
      auto disp = DMDisplayPtr::create();
      disp->m_id = 0;
      disp->m_name = "TestDisplay";
      disp->m_privId = 0;
      m_displays[0] = disp;
    }
    m_displays[0]->m_videoModes[id] = mode;
  }

private:
  int m_currentMode;
};


class TestDisplayManager : public QObject
{
  Q_OBJECT

private:
  void setAvoid25_30(bool on)
  {
    auto* section = SettingsComponent::Get().getSection(SETTINGS_SECTION_VIDEO);
    section->setValue("refreshrate.avoid_25hz_30hz", on);
  }

private slots:
  void initTestCase();
  void testExactRefreshRateMatch();
  void testPrefers60HzOver30HzForFilm();
  void testCloseRefreshRateMatch();
  void testNoMatchWithDifferentResolution();
  void testInterlacedPreference();
  void testCurrentModeBonusBreaksTie();
  void testAvoid25And30Hz();
  void testNoSuitableModeReturnsNegative();
  void testFindBestMode();
  void testMultipleRefreshRate();
  void testFindDisplayByName();
  void testHdrSwitchedOnForHdrMediaAndRestored();
  void testHdrLeftAloneForSdrMedia();
  void testHdrAlreadyOnIsNotTurnedOff();
  void testHdrNotSwitchedWhenIncapable();
  void testDolbyVisionPeakLuminance();
};

// Helper function: Populates a DisplayManager with some typical display modes.
static void setupStandardModes(TestableDisplayManager& mgr)
{
  // id, w, h, bpp, hz, interlaced
  mgr.addMode(0, 1920, 1080, 32, 24.0f, false);
  mgr.addMode(1, 1920, 1080, 32, 30.0f, false);
  mgr.addMode(2, 1920, 1080, 32, 50.0f, false);
  mgr.addMode(3, 1920, 1080, 32, 60.0f, false); // This will be set as currentMode (see below)
  mgr.addMode(4, 1920, 1080, 32, 23.976f, false);
  mgr.addMode(5, 1920, 1080, 32, 59.94f, false);
  mgr.setCurrentMode(3);
}

void TestDisplayManager::initTestCase()
{
  auto* section = new SettingsSection(SETTINGS_SECTION_VIDEO, PLATFORM_ANY, -1,
                                      &SettingsComponent::Get());
  auto* avoidSetting = new SettingsValue("refreshrate.avoid_25hz_30hz",
                                          false, PLATFORM_ANY, section);
  section->registerSetting(avoidSetting);
  SettingsComponent::Get().registerSection(section);
}

void TestDisplayManager::testExactRefreshRateMatch()
{
  TestableDisplayManager mgr;
  setupStandardModes(mgr);
  DMMatchMediaInfo match(24.0f, false);

  int best = mgr.findBestMatch(0, match);
  QCOMPARE(best, 0); // 24 hz mode
}

void TestDisplayManager::testPrefers60HzOver30HzForFilm()
{
  // Prefer 60 hz for 30 fps content.
  TestableDisplayManager mgr;
  setupStandardModes(mgr);
  DMMatchMediaInfo match(30.0f, false);

  setAvoid25_30(true);
  int best = mgr.findBestMatch(0, match);
  QCOMPARE(best, 3);
  setAvoid25_30(false);
}

void TestDisplayManager::testCloseRefreshRateMatch()
{
  // Prefer 24hz for 23.976 fps content.
  TestableDisplayManager mgr;
  setupStandardModes(mgr);
  DMMatchMediaInfo match(23.976f, false);

  int best = mgr.findBestMatch(0, match);
  QCOMPARE(best, 4);
}

void TestDisplayManager::testNoMatchWithDifferentResolution()
{
  // Prefer 1080p over 720p, regardless of framerate.
  TestableDisplayManager mgr;
  mgr.addMode(0, 1920, 1080, 32, 60.0f, false);
  mgr.addMode(1, 1280, 720,  32, 24.0f, false);
  mgr.setCurrentMode(0);

  DMMatchMediaInfo match(24.0f, false);
  int best = mgr.findBestMatch(0, match);
  QCOMPARE(best, 0);
}

void TestDisplayManager::testInterlacedPreference()
{
  // Prefer interlaced mode for interlaced content.
  TestableDisplayManager mgr;
  mgr.addMode(0, 1920, 1080, 32, 50.0f, false);
  mgr.addMode(1, 1920, 1080, 32, 50.0f, true);
  mgr.setCurrentMode(0); // non-interlaced

  DMMatchMediaInfo match(50.0f, true);
  int best = mgr.findBestMatch(0, match);
  QCOMPARE(best, 1);
}

void TestDisplayManager::testCurrentModeBonusBreaksTie()
{
  // Prefer current mode when multiple identical modes available.
  TestableDisplayManager mgr;
  mgr.addMode(0, 1920, 1080, 32, 60.0f, false);
  mgr.addMode(1, 1920, 1080, 32, 60.0f, false);
  mgr.setCurrentMode(1);

  DMMatchMediaInfo match(60.0f, false);
  int best = mgr.findBestMatch(0, match);
  QCOMPARE(best, 1);
}

void TestDisplayManager::testAvoid25And30Hz()
{
  // Prefer 50 hz for 25 fps content when avoid25_30 is enabled.
  TestableDisplayManager mgr;
  mgr.addMode(0, 1920, 1080, 32, 25.0f, false);
  mgr.addMode(1, 1920, 1080, 32, 50.0f, false);
  mgr.setCurrentMode(1);

  DMMatchMediaInfo match(25.0f, false);
  setAvoid25_30(true);
  int best = mgr.findBestMatch(0, match);
  QCOMPARE(best, 1);

  // Disabling avoid25_30 should produce the opposite result.
  setAvoid25_30(false);
  best = mgr.findBestMatch(0, match);
  QCOMPARE(best, 0);
}

void TestDisplayManager::testNoSuitableModeReturnsNegative()
{
  // No preference when resolution doesn't match content.
  TestableDisplayManager mgr;
  mgr.addMode(0, 1920, 1080, 32, 60.0f, false);
  mgr.addMode(1, 1280, 720,  32, 24.0f, false);
  mgr.setCurrentMode(0);
  mgr.m_displays[0]->m_videoModes.remove(0);

  DMMatchMediaInfo match(24.0f, false);
  int best = mgr.findBestMatch(0, match);

  QCOMPARE(best, -1);
}

void TestDisplayManager::testFindBestMode()
{
  // Prefer greatest refresh rate, greatest resolution, and non-interlaced.
  TestableDisplayManager mgr;
  mgr.addMode(0, 1280, 720,  32, 60.0f, false);
  mgr.addMode(1, 1920, 1080, 32, 60.0f, false);
  mgr.addMode(2, 1920, 1080, 32, 30.0f, true);
  mgr.addMode(3, 1920, 1080, 32, 60.0f, true);

  int best = mgr.findBestMode(0);
  QCOMPARE(best, 1);
}

void TestDisplayManager::testMultipleRefreshRate()
{
  // Prefer exact multiple of fps rate.
  TestableDisplayManager mgr;
  mgr.addMode(0, 1920, 1080, 32, 47.952f, false);
  mgr.addMode(1, 1920, 1080, 32, 60.0f,   false);
  mgr.setCurrentMode(1);

  DMMatchMediaInfo match(23.976f, false);
  int best = mgr.findBestMatch(0, match);
  QCOMPARE(best, 0);
}

void TestDisplayManager::testFindDisplayByName()
{
  TestableDisplayManager mgr;
  setupStandardModes(mgr);

  QCOMPARE(mgr.findDisplayByName("TestDisplay"), 0);
  QCOMPARE(mgr.findDisplayByName("HDMI-A-1"), -1);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void TestDisplayManager::testHdrSwitchedOnForHdrMediaAndRestored()
{
  TestableDisplayManager mgr;
  setupStandardModes(mgr);

  QVERIFY(mgr.switchHdrForMedia(0, true));
  QVERIFY(mgr.m_hdrEnabled);

  QVERIFY(mgr.restoreHdr());
  QVERIFY(!mgr.m_hdrEnabled);
  // Nothing left to restore.
  QVERIFY(!mgr.restoreHdr());
  QCOMPARE(mgr.m_hdrSwitches, 2);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void TestDisplayManager::testHdrLeftAloneForSdrMedia()
{
  TestableDisplayManager mgr;
  setupStandardModes(mgr);

  QVERIFY(!mgr.switchHdrForMedia(0, false));
  QVERIFY(!mgr.restoreHdr());
  QCOMPARE(mgr.m_hdrSwitches, 0);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void TestDisplayManager::testHdrAlreadyOnIsNotTurnedOff()
{
  // The user runs the desktop in HDR: playback must not change that.
  TestableDisplayManager mgr;
  setupStandardModes(mgr);
  mgr.m_hdrEnabled = true;

  QVERIFY(!mgr.switchHdrForMedia(0, true));
  QVERIFY(!mgr.restoreHdr());
  QVERIFY(mgr.m_hdrEnabled);
  QCOMPARE(mgr.m_hdrSwitches, 0);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void TestDisplayManager::testHdrNotSwitchedWhenIncapable()
{
  TestableDisplayManager mgr;
  setupStandardModes(mgr);
  mgr.m_hdrCapable = false;

  QVERIFY(!mgr.switchHdrForMedia(0, true));
  QVERIFY(!mgr.switchHdrForMedia(7, true)); // no such display
  QCOMPARE(mgr.m_hdrSwitches, 0);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void TestDisplayManager::testDolbyVisionPeakLuminance()
{
  // XGIMI Titan Noir Max (movie mode): no luminance in its HDR static metadata block, a
  // Dolby Vision v2 block with "Target Max PQ v2: 2705" = 431 cd/m^2 (PQ; an older
  // edid-decode printed 447).
  QByteArray edid(
    "\x00\xff\xff\xff\xff\xff\xff\x00\x60\xed\x30\x00\x01\x00\x00\x00"
    "\x01\x23\x01\x03\x80\x73\x41\x78\x0a\xcf\x74\xa3\x57\x4c\xb0\x23"
    "\x09\x48\x4c\x00\x00\x00\x01\x01\x01\xff\x01\xff\xff\x01\x01\x01"
    "\x01\x01\x01\x01\x01\x20\x08\xe8\x00\x30\xf2\x70\x5a\x80\xb0\x58"
    "\x8a\x00\xc4\x8e\x21\x00\x00\x1e\x02\x3a\x80\x18\x71\x38\x2d\x40"
    "\x58\x2c\x45\x00\xc4\x8e\x21\x00\x00\x1e\x00\x00\x00\xfc\x00\x58"
    "\x47\x49\x4d\x49\x20\x54\x56\x0a\x20\x20\x20\x20\x00\x00\x00\xfd"
    "\x00\x3b\x46\x1f\x8c\x3c\x00\x0a\x20\x20\x20\x20\x20\x20\x01\xc7"
    "\x02\x03\x71\xf0\x57\x05\x84\x03\x01\x12\x13\x14\x90\x1f\x20\x22"
    "\x5d\x5f\x60\x61\x62\x64\x65\x66\x5e\x63\x02\x21\x32\x09\x7f\x07"
    "\x3d\x07\xc0\x5f\x7e\x07\x15\x07\x50\x57\x06\x00\x67\x7e\x00\x83"
    "\x01\x00\x00\xe2\x00\xff\x72\x03\x0c\x00\x30\x00\xf8\x44\x20\xc0"
    "\x84\x01\x02\x03\x04\x01\x41\x00\x00\x68\xd8\x5d\xc4\x01\x78\x80"
    "\x6f\x02\xe3\x05\xff\x81\xe4\x0f\x00\x60\x06\xe3\x06\x07\x01\xe5"
    "\x01\x8b\x84\x90\x01\xeb\x01\x46\xd0\x00\x4d\x63\x52\x3e\xa4\xcc"
    "\x39\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x53",
    256);
  QCOMPARE(qRound(DisplayManager::dolbyVisionPeakLuminance(edid)), 431);

  // Same block as version 1: target max = 100 + 50 * (byte 1 >> 1).
  QByteArray v1 = edid;
  const qsizetype dv = v1.indexOf(QByteArray("\x01\x46\xd0\x00", 4)) + 4;
  v1[dv] = char(0x20);     // version 1
  v1[dv + 1] = char(7 << 1); // 100 + 7 * 50
  QCOMPARE(qRound(DisplayManager::dolbyVisionPeakLuminance(v1)), 450);

  // No Dolby block (other OUI), and no EDID at all.
  QByteArray none = edid;
  none[dv - 1] = char(0x01);
  QCOMPARE(DisplayManager::dolbyVisionPeakLuminance(none), 0.0);
  QCOMPARE(DisplayManager::dolbyVisionPeakLuminance(QByteArray()), 0.0);
}

QTEST_APPLESS_MAIN(TestDisplayManager)
#include "test_displaymanager.moc"
