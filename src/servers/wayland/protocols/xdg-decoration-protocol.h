/* Generated for wayland_server */
#ifndef XDG_DECORATION_UNSTABLE_V1_SERVER_PROTOCOL_H
#define XDG_DECORATION_UNSTABLE_V1_SERVER_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include "wayland-server.h"

#ifdef  __cplusplus
extern "C" {
#endif

struct wl_client;
struct wl_resource;

#ifndef ZXDG_DECORATION_MANAGER_V1_INTERFACE
#define ZXDG_DECORATION_MANAGER_V1_INTERFACE
extern const struct wl_interface zxdg_decoration_manager_v1_interface;
#endif
#ifndef ZXDG_TOPLEVEL_DECORATION_V1_INTERFACE
#define ZXDG_TOPLEVEL_DECORATION_V1_INTERFACE
extern const struct wl_interface zxdg_toplevel_decoration_v1_interface;
#endif

enum zxdg_toplevel_decoration_v1_mode {
	ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE = 1,
	ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE = 2,
};

#ifdef  __cplusplus
}
#endif

#endif
