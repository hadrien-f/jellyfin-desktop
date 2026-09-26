#ifndef VIDEOBACKEND_H
#define VIDEOBACKEND_H

#include <QString>

// How decoded video reaches the screen.
enum class VideoBackend
{
  // mpv renders through the libmpv OpenGL render API into a Qt Quick item
  // (MpvVideoItem). Works everywhere, but always outputs SDR.
  LibmpvRender,
  // mpv uses its own gpu-next Vulkan video output, shown as a Wayland
  // subsurface below the web view (see player/wayland/). Can output HDR.
  WaylandSubsurface,
};

struct VideoBackendEnvironment
{
  bool waylandSubsurfaceBuilt = false; // compiled with USE_WAYLAND_HDR
  bool waylandSession = false;
  bool compositorColorManagement = false; // wp_color_manager_v1 is advertised
  QString forcedVo;                       // video/debug.force_vo setting
};

// Picks the backend. The subsurface backend is only worth it (and only tested)
// where the compositor can take HDR surfaces; any forced VO keeps the
// historical render-API path.
VideoBackend selectVideoBackend(const VideoBackendEnvironment& env);

#endif // VIDEOBACKEND_H
