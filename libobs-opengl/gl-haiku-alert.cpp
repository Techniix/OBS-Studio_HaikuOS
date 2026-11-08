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

#include "gl-haiku-alert.h"

#include <new>

#include <Alert.h>
#include <Application.h>
#include <Invoker.h>
#include <Message.h>
#include <Messenger.h>
#include <OS.h>

namespace {

const uint32 kAlertAnswer = 0x6f476c41; /* 'oGlA' */

/* The asynchronous BAlert::Go(BInvoker *) calls Invoke() from the alert's window thread with the pressed button
 * in "which". This invoker sends nothing: it keeps the answer and wakes the thread waiting in
 * gl_haiku_alert_wait(), so no looper is needed on the watchdog's side. BAlert does not delete its invoker. */
class AnswerInvoker : public BInvoker {
public:
	AnswerInvoker() : BInvoker(new BMessage(kAlertAnswer), NULL), fSem(create_sem(0, "obs gl alert")), fAnswer(-1)
	{
	}

	~AnswerInvoker() override
	{
		if (fSem >= B_OK)
			delete_sem(fSem);
	}

	status_t Invoke(BMessage *message) override
	{
		BMessage *msg = message ? message : Message();
		int32 which = -1;
		if (msg && msg->FindInt32("which", &which) == B_OK)
			atomic_set(&fAnswer, which);
		/* nothing of this object is touched after the release: the waiter may delete it right away */
		release_sem(fSem);
		return B_OK;
	}

	sem_id fSem;
	int32 fAnswer;
};

} // namespace

struct gl_haiku_alert {
	BMessenger window;
	AnswerInvoker *invoker;
};

extern "C" struct gl_haiku_alert *gl_haiku_alert_show(const char *title, const char *text, const char *button0,
						       const char *button1)
{
	/* a BWindow needs the app_server link of a BApplication (the Qt Haiku platform plugin creates it) */
	if (be_app == NULL)
		return NULL;

	AnswerInvoker *invoker = new (std::nothrow) AnswerInvoker();
	if (!invoker || invoker->fSem < B_OK || !invoker->Message()) {
		delete invoker;
		return NULL;
	}
	gl_haiku_alert *alert = new (std::nothrow) gl_haiku_alert;
	BAlert *window = new (std::nothrow) BAlert(title, text, button0, button1, NULL, B_WIDTH_AS_USUAL,
						   B_EVEN_SPACING, B_WARNING_ALERT);
	if (!alert || !window) {
		delete alert;
		delete window;
		delete invoker;
		return NULL;
	}

	window->SetShortcut(0, B_ESCAPE);
	alert->invoker = invoker;
	alert->window = BMessenger(window);
	/* asynchronous: returns at once; the window runs in its own thread */
	window->Go(invoker);
	return alert;
}

extern "C" int gl_haiku_alert_wait(struct gl_haiku_alert *alert, int timeout_ms)
{
	if (!alert)
		return GL_HAIKU_ALERT_PENDING;

	status_t status;
	do {
		status = acquire_sem_etc(alert->invoker->fSem, 1, B_RELATIVE_TIMEOUT, (bigtime_t)timeout_ms * 1000);
	} while (status == B_INTERRUPTED);
	if (status != B_OK)
		return GL_HAIKU_ALERT_PENDING;

	const int answer = (int)atomic_get(&alert->invoker->fAnswer);
	/* the alert quits itself after an answer */
	delete alert->invoker;
	delete alert;
	return answer < 0 ? 0 : answer;
}

extern "C" void gl_haiku_alert_close(struct gl_haiku_alert *alert)
{
	if (!alert)
		return;

	/* A button pressed just now could still reach the invoker from the window thread, so the invoker
	 * (a few dozen bytes, once per hang) is left alive. */
	alert->window.SendMessage(B_QUIT_REQUESTED);
	delete alert;
}
