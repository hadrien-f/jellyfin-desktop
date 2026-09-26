#include <QtTest/QtTest>
#include <cstring>

#include <wayland-client-protocol.h>

#include "../src/player/wayland/WaylandProtocolRules.h"

using namespace WaylandProtocolRules;

static const wl_message* findRequest(const wl_interface& interface, const char* name)
{
  for (int i = 0; i < interface.method_count; ++i)
  {
    if (strcmp(interface.methods[i].name, name) == 0)
      return &interface.methods[i];
  }
  return nullptr;
}

static const wl_message* findEvent(const wl_interface& interface, const char* name)
{
  for (int i = 0; i < interface.event_count; ++i)
  {
    if (strcmp(interface.events[i].name, name) == 0)
      return &interface.events[i];
  }
  return nullptr;
}

// Checked against libwayland's own interface tables, so a wrong rule here
// would also be wrong on the wire.
class TestWaylandProtocolRules : public QObject
{
  Q_OBJECT

private slots:
  void sinceVersion()
  {
    QCOMPARE(WaylandProtocolRules::sinceVersion(findRequest(wl_surface_interface, "attach")), 1);
    QCOMPARE(WaylandProtocolRules::sinceVersion(findRequest(wl_surface_interface, "damage_buffer")), 4);
    QCOMPARE(WaylandProtocolRules::sinceVersion(findRequest(wl_shm_interface, "release")), 2);
    QCOMPARE(WaylandProtocolRules::sinceVersion(findRequest(wl_output_interface, "release")), 3);
  }

  // Sending wl_shm.release on a version 1 object is a protocol error, which
  // would kill the application's Wayland connection.
  void destructorRespectsVersion()
  {
    QCOMPARE(destructorOpcode(&wl_shm_interface, 1), -1);
    QVERIFY(destructorOpcode(&wl_shm_interface, 2) >= 0);
    QCOMPARE(destructorOpcode(&wl_output_interface, 2), -1);
    QVERIFY(destructorOpcode(&wl_output_interface, 4) >= 0);

    // wl_callback has no destructor request at all.
    QCOMPARE(destructorOpcode(&wl_callback_interface, 1), -1);

    int op = destructorOpcode(&wl_surface_interface, 6);
    QVERIFY(op >= 0);
    QCOMPARE(wl_surface_interface.methods[op].name, "destroy");
  }

  void destructorRequests()
  {
    QVERIFY(isDestructorRequest(findRequest(wl_surface_interface, "destroy")));
    QVERIFY(isDestructorRequest(findRequest(wl_output_interface, "release")));
    QVERIFY(!isDestructorRequest(findRequest(wl_surface_interface, "commit")));
  }

  void destructorEvents()
  {
    QVERIFY(isDestructorEvent("wl_callback", findEvent(wl_callback_interface, "done")));
    QVERIFY(!isDestructorEvent("wl_buffer", findEvent(wl_buffer_interface, "release")));
    QVERIFY(!isDestructorEvent("wl_surface", findEvent(wl_surface_interface, "enter")));
  }

  void perFrameRequests()
  {
    QVERIFY(isPerFrameRequest("wl_surface", findRequest(wl_surface_interface, "frame")));
    QVERIFY(!isPerFrameRequest("wl_compositor", findRequest(wl_compositor_interface, "create_surface")));
  }

  void hiddenGlobals()
  {
    QVERIFY(isForwardedGlobal("wl_compositor"));
    QVERIFY(isForwardedGlobal("wp_color_manager_v1"));
    // Input stays with the web view; the translator implements the shell itself.
    QVERIFY(!isForwardedGlobal("wl_seat"));
    QVERIFY(!isForwardedGlobal("xdg_wm_base"));
    QVERIFY(!isForwardedGlobal("zxdg_decoration_manager_v1"));
    QVERIFY(!isForwardedGlobal("wl_data_device_manager"));
  }
};

QTEST_GUILESS_MAIN(TestWaylandProtocolRules)
#include "test_waylandprotocolrules.moc"
