/******************************************************************************
    Copyright (C) 2026 by the OBS Studio Haiku port contributors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#pragma once

/* A native Haiku alert (BAlert) shown from a plain thread without blocking it, for gl-haiku.c's GPU hang
 * watchdog: the UI thread is usually stuck behind the hung graphics thread then, so a Qt dialog can't be used. */

#ifdef __cplusplus
extern "C" {
#endif

struct gl_haiku_alert;

#define GL_HAIKU_ALERT_PENDING (-1)
#define GL_HAIKU_ALERT_ACTION 0 /* the left button */
#define GL_HAIKU_ALERT_WAIT 1   /* the right button: the default one (Enter), also picked by Escape */

/* Shows a warning alert with two buttons without blocking. The alert is modal for the application's windows
 * only. Returns NULL if there is no BApplication (be_app) or the alert could not be created. */
struct gl_haiku_alert *gl_haiku_alert_show(const char *title, const char *text, const char *action_button,
					   const char *wait_button);

/* Never blocks. Returns GL_HAIKU_ALERT_PENDING while the alert is open; otherwise the button pressed, after
 * which the alert is gone and must not be used again. An alert that went away without an answer (closed by the
 * system) counts as GL_HAIKU_ALERT_WAIT. */
int gl_haiku_alert_poll(struct gl_haiku_alert *alert);

/* Closes an alert that is still pending and releases it. */
void gl_haiku_alert_close(struct gl_haiku_alert *alert);

#ifdef __cplusplus
}
#endif
