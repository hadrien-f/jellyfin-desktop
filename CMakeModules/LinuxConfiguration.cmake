include(GNUInstallDirs)

find_package(X11)
if(X11_FOUND AND X11_Xrandr_FOUND)
  include_directories(${X11_X11_INCLUDE_PATH} ${X11_Xrandr_INCLUDE_PATH})
  set(X11XRANDR_FOUND 1)
  add_definitions(-DUSE_X11XRANDR)
else()
  set(X11_LIBRARIES "")
  set(X11_Xrandr_LIB "")
endif()

# Display mode switching on KDE Plasma Wayland sessions
find_package(Qt6 COMPONENTS WaylandClient QUIET)
find_path(PLASMA_WAYLAND_PROTOCOLS_DIR kde-output-device-v2.xml
  PATHS ${CMAKE_INSTALL_FULL_DATADIR} /usr/share
  PATH_SUFFIXES plasma-wayland-protocols)
# kde_output_device_registry_v2 needs plasma-wayland-protocols 1.21 or newer
if(PLASMA_WAYLAND_PROTOCOLS_DIR)
  file(STRINGS ${PLASMA_WAYLAND_PROTOCOLS_DIR}/kde-output-device-v2.xml HAS_OUTPUT_REGISTRY
    REGEX "kde_output_device_registry_v2")
endif()
if(Qt6WaylandClient_FOUND AND HAS_OUTPUT_REGISTRY)
  set(KDEWAYLAND_FOUND 1)
  add_definitions(-DUSE_KDE_WAYLAND)
  message(STATUS "Enabling KDE Wayland display mode switching")
endif()

# HDR video on Wayland: mpv's own video output shown as a subsurface of the
# window through an in-process protocol translator (src/player/wayland).
set(WAYLANDHDR_PROTOCOLS
  stable/xdg-shell/xdg-shell.xml
  stable/linux-dmabuf/linux-dmabuf-v1.xml
  stable/viewporter/viewporter.xml
  stable/presentation-time/presentation-time.xml
  staging/color-management/color-management-v1.xml
  staging/color-representation/color-representation-v1.xml
  staging/fractional-scale/fractional-scale-v1.xml
  staging/single-pixel-buffer/single-pixel-buffer-v1.xml
  staging/content-type/content-type-v1.xml
  staging/linux-drm-syncobj/linux-drm-syncobj-v1.xml
  staging/fifo/fifo-v1.xml
)
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  # wl_proxy_marshal_array_flags() needs libwayland 1.20
  pkg_check_modules(WAYLAND QUIET IMPORTED_TARGET wayland-client>=1.20 wayland-server>=1.20)
  pkg_check_modules(WAYLAND_PROTOCOLS QUIET wayland-protocols)
endif()
find_program(WAYLAND_SCANNER wayland-scanner)
if(WAYLAND_FOUND AND WAYLAND_PROTOCOLS_FOUND AND WAYLAND_SCANNER AND NOT OPENELEC
   AND NOT BUILD_TARGET STREQUAL "RPI")
  pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
  set(WAYLANDHDR_FOUND 1)
  foreach(protocol ${WAYLANDHDR_PROTOCOLS})
    if(NOT EXISTS ${WAYLAND_PROTOCOLS_DIR}/${protocol})
      message(STATUS "HDR video on Wayland disabled: wayland-protocols lacks ${protocol}")
      unset(WAYLANDHDR_FOUND)
      break()
    endif()
  endforeach()
  # The window's wl_surface is only reachable through Qt's private native interface.
  find_package(Qt6 COMPONENTS GuiPrivate QUIET)
endif()
if(WAYLANDHDR_FOUND)
  add_definitions(-DUSE_WAYLAND_HDR)
  message(STATUS "Enabling HDR video output on Wayland")
endif()

if(LINUX_X11POWER)
  add_definitions(-DUSE_X11POWER)
  Message(STATUS "Enabling X11/XDG screensaver management")
else()
  add_definitions(-DLINUX_DBUS=1)
  Message(STATUS "Enabling D-Bus power management")
endif()

set(INSTALL_BIN_DIR ${CMAKE_INSTALL_BINDIR})
set(INSTALL_RESOURCE_DIR ${CMAKE_INSTALL_DATADIR}/jellyfin-desktop)

if(NOT OPENELEC)
  include(InstallLinuxDesktopFile)
endif()
