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

/*
 * The parts of HaikuWebKit's ABI the helper needs beyond the installed WebView.h/WebPage.h/WebWindow.h.
 *
 * haikuwebkit_devel installs WebFrame.h, but it includes WebCore and JavaScriptCore internals that the package
 * does not ship, so it cannot be included by third-party code (WebPositive only uses BWebView/BWebPage for
 * the same reason). The declarations below name the same exported symbols with the same signatures:
 *
 *   BWebFrame::SetTransparent(bool)                  _ZN9BWebFrame14SetTransparentEb
 *   BWebFrame::GlobalContext() const                 _ZNK9BWebFrame13GlobalContextEv
 *   WebCore::DumpRenderTreeClient::getOffscreen()    _ZN7WebCore20DumpRenderTreeClient12getOffscreenEP8BWebView
 *
 * getOffscreen() is the exported accessor HaikuWebKit's own test runner uses to read a view's backing bitmap
 * (HaikuWebKit renders every page into that bitmap; it is built without accelerated compositing, so the
 * bitmap holds the complete page). The JavaScriptCore functions are its public C API.
 *
 * Never include WebFrame.h or DumpRenderTreeClient.h in the same translation unit as this file.
 */

#pragma once

class BBitmap;
class BWebView;

extern "C" {
typedef const struct OpaqueJSContext *JSContextRef;
typedef struct OpaqueJSContext *JSGlobalContextRef;
typedef struct OpaqueJSString *JSStringRef;
typedef struct OpaqueJSValue *JSObjectRef;
typedef const struct OpaqueJSValue *JSValueRef;

JSStringRef JSStringCreateWithUTF8CString(const char *string);
void JSStringRelease(JSStringRef string);
JSValueRef JSEvaluateScript(JSContextRef ctx, JSStringRef script, JSObjectRef thisObject, JSStringRef sourceURL,
			    int startingLineNumber, JSValueRef *exception);
}

class __attribute__((visibility("default"))) BWebFrame {
public:
	void SetTransparent(bool transparent);
	JSGlobalContextRef GlobalContext() const;

private:
	BWebFrame() = delete; /* only ever used through pointers handed out by BWebPage */
};

namespace WebCore {
class __attribute__((visibility("default"))) DumpRenderTreeClient {
public:
	static BBitmap *getOffscreen(BWebView *view);

private:
	DumpRenderTreeClient() = delete;
};
} // namespace WebCore
