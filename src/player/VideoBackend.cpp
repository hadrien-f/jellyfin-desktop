#include "VideoBackend.h"

///////////////////////////////////////////////////////////////////////////////////////////////////
VideoBackend selectVideoBackend(const VideoBackendEnvironment& env)
{
  if (!env.forcedVo.isEmpty())
    return VideoBackend::LibmpvRender;

  if (env.waylandSubsurfaceBuilt && env.waylandSession && env.compositorColorManagement)
    return VideoBackend::WaylandSubsurface;

  return VideoBackend::LibmpvRender;
}
