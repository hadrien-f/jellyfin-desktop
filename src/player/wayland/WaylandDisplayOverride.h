#ifndef WAYLANDDISPLAYOVERRIDE_H
#define WAYLANDDISPLAYOVERRIDE_H

#include <QByteArray>

// Temporarily points WAYLAND_DISPLAY at another socket.
//
// mpv's video output connects with wl_display_connect(NULL) and offers no
// option to pick the socket, so the only way to send it to the translator is
// the environment. The environment is process-wide (Qt reconnects and child
// processes read it too), so the override is kept as short as possible: armed
// right before mpv creates its VO and restored as soon as the VO connected.
//
// Remove once mpv has a --wayland-display option.
class WaylandDisplayOverride
{
public:
  ~WaylandDisplayOverride() { restore(); }

  // Sets WAYLAND_DISPLAY to `socket`. Re-arming while armed keeps the value
  // saved by the first arm.
  void arm(const QByteArray& socket);

  // Puts back the value (or absence) seen by the first arm(). No-op when not armed.
  void restore();

  bool isArmed() const { return m_armed; }

private:
  bool m_armed = false;
  bool m_hadValue = false;
  QByteArray m_saved;
};

#endif // WAYLANDDISPLAYOVERRIDE_H
