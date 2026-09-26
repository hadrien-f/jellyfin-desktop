#include <QtTest/QtTest>
#include "../src/player/wayland/WaylandDisplayOverride.h"

class TestWaylandDisplayOverride : public QObject
{
  Q_OBJECT

private slots:
  void init() { qputenv("WAYLAND_DISPLAY", "wayland-0"); }

  void armAndRestore()
  {
    WaylandDisplayOverride override;
    override.arm("translator-1");
    QVERIFY(override.isArmed());
    QCOMPARE(qgetenv("WAYLAND_DISPLAY"), QByteArray("translator-1"));

    override.restore();
    QVERIFY(!override.isArmed());
    QCOMPARE(qgetenv("WAYLAND_DISPLAY"), QByteArray("wayland-0"));
  }

  void rearmKeepsTheFirstSavedValue()
  {
    WaylandDisplayOverride override;
    override.arm("translator-1");
    override.arm("translator-2");
    QCOMPARE(qgetenv("WAYLAND_DISPLAY"), QByteArray("translator-2"));

    override.restore();
    QCOMPARE(qgetenv("WAYLAND_DISPLAY"), QByteArray("wayland-0"));
  }

  void restoresAnUnsetVariable()
  {
    qunsetenv("WAYLAND_DISPLAY");
    WaylandDisplayOverride override;
    override.arm("translator-1");
    override.restore();
    QVERIFY(!qEnvironmentVariableIsSet("WAYLAND_DISPLAY"));
  }

  void restoreWithoutArmDoesNothing()
  {
    WaylandDisplayOverride override;
    override.restore();
    QCOMPARE(qgetenv("WAYLAND_DISPLAY"), QByteArray("wayland-0"));
  }

  void destructorRestores()
  {
    {
      WaylandDisplayOverride override;
      override.arm("translator-1");
    }
    QCOMPARE(qgetenv("WAYLAND_DISPLAY"), QByteArray("wayland-0"));
  }
};

QTEST_GUILESS_MAIN(TestWaylandDisplayOverride)
#include "test_waylanddisplayoverride.moc"
