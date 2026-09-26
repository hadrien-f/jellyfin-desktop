#ifndef WAYLANDPROTOCOLRULES_H
#define WAYLANDPROTOCOLRULES_H

// Protocol facts the translator needs but libwayland does not expose: which
// messages destroy their object, and when a message became available. Kept
// free of translator state so they can be unit-tested against the real
// wl_interface tables.

#include <cstdlib>
#include <cstring>

#include <wayland-util.h>

namespace WaylandProtocolRules
{
  // Globals offered to mpv; everything else is hidden. Hiding wl_seat keeps
  // input with the web view, hiding shells/decorations/activation keeps mpv
  // from acting like a window. xdg_wm_base is not forwarded: the translator
  // implements it itself.
  inline const char* const s_forwardedGlobals[] = {
    "wl_compositor",
    "wl_subcompositor",
    "wl_shm",
    "wl_output",
    "zwp_linux_dmabuf_v1",
    "wp_viewporter",
    "wp_presentation",
    "wp_color_manager_v1",
    "wp_color_representation_manager_v1",
    "wp_fractional_scale_manager_v1",
    "wp_single_pixel_buffer_manager_v1",
    "wp_content_type_manager_v1",
    "wp_linux_drm_syncobj_manager_v1",
    "wp_fifo_manager_v1",
  };

  inline bool isForwardedGlobal(const char* interface)
  {
    for (const char* name : s_forwardedGlobals)
    {
      if (strcmp(name, interface) == 0)
        return true;
    }
    return false;
  }

  // The protocol version a message appeared in: the signature's leading digits.
  inline int sinceVersion(const wl_message* message)
  {
    int since = atoi(message->signature);
    return since > 0 ? since : 1;
  }

  // Requests that destroy their object. Every forwarded interface names its
  // destructor "destroy" or "release".
  inline bool isDestructorRequest(const wl_message* message)
  {
    return strcmp(message->name, "destroy") == 0 || strcmp(message->name, "release") == 0;
  }

  // Events after which the object no longer exists on either side.
  inline bool isDestructorEvent(const char* interface, const wl_message* message)
  {
    if (strcmp(interface, "wl_callback") == 0)
      return strcmp(message->name, "done") == 0;
    if (strcmp(interface, "wp_presentation_feedback") == 0)
      return strcmp(message->name, "presented") == 0 || strcmp(message->name, "discarded") == 0;
    if (strcmp(interface, "wp_image_description_info_v1") == 0)
      return strcmp(message->name, "done") == 0;
    return false;
  }

  // Object-creating requests sent every frame. The translator roundtrips after
  // other object creations (clients expect the compositor's reply to have
  // arrived by their next wl_display.sync), but not after these.
  inline bool isPerFrameRequest(const char* interface, const wl_message* message)
  {
    return (strcmp(interface, "wl_surface") == 0 && strcmp(message->name, "frame") == 0) ||
           (strcmp(interface, "wp_presentation") == 0 && strcmp(message->name, "feedback") == 0);
  }

  // The destructor request that exists at `version`, or -1. Used when a client
  // disconnects without destroying its objects.
  inline int destructorOpcode(const wl_interface* interface, int version)
  {
    for (int op = 0; op < interface->method_count; ++op)
    {
      const wl_message* message = &interface->methods[op];
      if (isDestructorRequest(message) && sinceVersion(message) <= version)
        return op;
    }
    return -1;
  }
}

#endif // WAYLANDPROTOCOLRULES_H
