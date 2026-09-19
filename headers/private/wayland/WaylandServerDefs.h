/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _WAYLAND_SERVER_DEFS_H
#define _WAYLAND_SERVER_DEFS_H


#define kWaylandServerSignature	"application/x-vnd.Haiku-WaylandServer"

enum {
	// Listen for Wayland clients on a new socket at "path" (string). The
	// optional "label" (string) names whoever the socket was made for.
	// The reply carries "status" (int32).
	kMsgAddSocket		= 'wlas',

	// Stop listening on the socket at "path" and remove it.
	kMsgRemoveSocket	= 'wlrs'
};


#endif	// _WAYLAND_SERVER_DEFS_H
