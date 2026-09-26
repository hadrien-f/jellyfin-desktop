#include <QtTest/QtTest>
#include "../src/player/VideoBackend.h"

class TestVideoBackend : public QObject
{
  Q_OBJECT

private:
  static VideoBackendEnvironment hdrCapable()
  {
    VideoBackendEnvironment env;
    env.waylandSubsurfaceBuilt = true;
    env.waylandSession = true;
    env.compositorColorManagement = true;
    return env;
  }

private slots:
  void subsurfaceWhenEverythingIsThere()
  {
    QCOMPARE(selectVideoBackend(hdrCapable()), VideoBackend::WaylandSubsurface);
  }

  void renderApiWhenAnyConditionIsMissing()
  {
    auto env = hdrCapable();
    env.waylandSubsurfaceBuilt = false;
    QCOMPARE(selectVideoBackend(env), VideoBackend::LibmpvRender);

    env = hdrCapable();
    env.waylandSession = false; // X11
    QCOMPARE(selectVideoBackend(env), VideoBackend::LibmpvRender);

    env = hdrCapable();
    env.compositorColorManagement = false; // e.g. an older compositor
    QCOMPARE(selectVideoBackend(env), VideoBackend::LibmpvRender);
  }

  void forcedVoWins()
  {
    auto env = hdrCapable();
    env.forcedVo = "libmpv";
    QCOMPARE(selectVideoBackend(env), VideoBackend::LibmpvRender);

    env.forcedVo = "gpu-next";
    QCOMPARE(selectVideoBackend(env), VideoBackend::LibmpvRender);
  }
};

QTEST_GUILESS_MAIN(TestVideoBackend)
#include "test_videobackend.moc"
