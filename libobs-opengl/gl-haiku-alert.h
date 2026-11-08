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

/* Shows a warning alert with two buttons; button1 is the default (rightmost, Enter), Escape picks button0.
 * Returns NULL if there is no BApplication (be_app) or the alert could not be created. */
struct gl_haiku_alert *gl_haiku_alert_show(const char *title, const char *text, const char *button0,
					   const char *button1);

/* Waits up to timeout_ms for an answer. Returns GL_HAIKU_ALERT_PENDING, or the index of the button pressed
 * (0 or 1), after which the alert is gone and must not be used again. */
int gl_haiku_alert_wait(struct gl_haiku_alert *alert, int timeout_ms);

/* Closes an alert that is still pending and releases it. */
void gl_haiku_alert_close(struct gl_haiku_alert *alert);

#ifdef __cplusplus
}
#endif
