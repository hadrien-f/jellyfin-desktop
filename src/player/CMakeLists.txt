add_sources(PlayerComponent.cpp PlayerComponent.h)
add_sources(AlbumArtProvider.cpp AlbumArtProvider.h)
add_sources(MpvVideoItem.cpp MpvVideoItem.h)
add_sources(OpenGLDetect.cpp OpenGLDetect.h)
add_sources(QtHelper.h)
add_sources(VideoBackend.cpp VideoBackend.h)

if(UNIX AND NOT APPLE)
  add_sources(wayland/WaylandDisplayOverride.cpp wayland/WaylandDisplayOverride.h)
endif()

if(WAYLANDHDR_FOUND)
  add_sources(
    wayland/WaylandProtocolRules.h
    wayland/WaylandTranslator.cpp wayland/WaylandTranslator.h
    wayland/WaylandVideoOutput.cpp wayland/WaylandVideoOutput.h
  )
endif()
