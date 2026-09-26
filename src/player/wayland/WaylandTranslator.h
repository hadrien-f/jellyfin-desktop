#ifndef WAYLANDTRANSLATOR_H
#define WAYLANDTRANSLATOR_H

#include <functional>
#include <memory>

#include <QByteArray>

struct wl_display;
struct wl_surface;

// Shows mpv's own Wayland video output as a subsurface of a Qt window.
//
// Wayland cannot parent one client's surface under another client's, and mpv
// has no way to render into a surface it is given. So mpv connects to a
// Wayland server inside this process instead of the compositor, and every
// request it (and Mesa's Vulkan WSI inside it) sends is replayed onto Qt's own
// wl_display, on a private event queue; events travel back the same way. The
// compositor therefore sees mpv's surfaces as Qt's, and the translator can turn
// mpv's xdg_toplevel into a wl_subsurface placed below the window's surface.
//
// Forwarding is generic: the translator walks each message's signature and
// maps object arguments between the two sides, so any interface listed in
// WaylandProtocolRules::s_forwardedGlobals works without per-request code.
// Only xdg_wm_base is implemented here; unlisted globals (wl_seat, shells,
// decorations, ...) are hidden so mpv never takes input or focus.
//
// A protocol error on the upstream side would kill Qt's connection, so every
// message must respect object versions; see WaylandProtocolRules.
//
// Threading: the translator runs its own thread, which serves mpv and
// dispatches the private upstream queue. Callbacks are invoked on that thread.
// The public methods may be called from any thread.
class WaylandTranslator
{
public:
  struct Callbacks
  {
    // A client bound xdg_wm_base, i.e. mpv's VO is connected.
    std::function<void()> shellBound;
    // mpv's toplevel was attached below the parent (true) or went away (false).
    std::function<void(bool attached)> videoSurfaceChanged;
    // The parent must commit for a subsurface change to take effect.
    std::function<void()> parentCommitNeeded;
  };

  WaylandTranslator(wl_display* upstream, Callbacks callbacks);
  ~WaylandTranslator();

  // False when the socket could not be created or a required global is missing.
  bool isValid() const;

  // Name of the listening socket in $XDG_RUNTIME_DIR, for WAYLAND_DISPLAY.
  // Unique to this process.
  QByteArray socketName() const;

  // Surface mpv's toplevel is placed below. Call with nullptr *before* the
  // surface is destroyed; blocks until the translator stopped using it.
  void setParentSurface(wl_surface* parent);

  // Logical size sent to mpv as an xdg_toplevel configure.
  void configure(int width, int height);

  struct Private;

private:
  std::unique_ptr<Private> d;
};

#endif // WAYLANDTRANSLATOR_H
