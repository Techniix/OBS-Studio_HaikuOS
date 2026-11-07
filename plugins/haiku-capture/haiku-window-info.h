/*
 * Declarations of Haiku's private window-list API (exported by libbe), as defined in
 * headers/private/interface/WindowInfo.h of the Haiku source tree:
 *
 *   Copyright 2005-2008, Haiku.
 *   Distributed under the terms of the MIT License.
 *   Authors: Axel Dörfler, axeld@pinc-software.de
 *
 * The same API is used by Deskbar, the Switcher and the Screenshot tool. It is reproduced here so that the
 * plugin builds without the private headers being installed. The layout must match the app_server's
 * (ServerWindow::GetInfo()); it has been stable since BeOS.
 */

#pragma once

#include <SupportDefs.h>

struct window_info {
	team_id team;
	int32 server_token;

	int32 thread;
	int32 client_token;
	int32 client_port;
	uint32 workspaces;

	int32 layer; /* 0: hidden, 1: not visible, 2: desktop, 3: normal, 4: floating/modal */
	uint32 feel;
	uint32 flags;
	int32 window_left; /* content frame in screen coordinates (decorator excluded) */
	int32 window_top;
	int32 window_right;
	int32 window_bottom;

	int32 show_hide_level;
	bool is_mini;
} _PACKED;

struct client_window_info : window_info {
	float tab_height;
	float border_size;
	char name[0];
} _PACKED;

/* Both return malloc()ed memory that the caller must free(). */
client_window_info *get_window_info(int32 token);
int32 *get_token_list(team_id app, int32 *count);

namespace BPrivate {
/* Window tokens of a workspace, front to back. */
status_t get_window_order(int32 workspace, int32 **_tokens, int32 *_count);
} // namespace BPrivate
