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
 * Haiku window system integration for the OpenGL renderer.
 *
 * A Haiku window surface belongs to a BWindow/BView whose looper thread owns it, which does not fit OBS's model
 * of a single graphics thread rendering into arbitrary windows. Instead, every swapchain renders into its own
 * offscreen buffer; on present, the finished frame is read back and handed to the owner of the window through
 * the callback in struct gs_window. The owner (e.g. the Qt frontend) blits it into its view.
 *
 * Two backends provide those offscreen buffers, tried in this order:
 *
 *  - EGL pbuffers. Mesa 25 and later (packages mesa_eglvnd + libglvnd) implement EGL for Haiku.
 *  - OSMesa. Mesa 22.0.5, the version HaikuPorts currently ships, has its Haiku EGL driver disabled (its
 *    eglInitialize() always fails), but it builds OSMesa on the same llvmpipe renderer. OSMesa contexts render
 *    into a caller-provided memory buffer, which maps onto swapchains exactly like pbuffers. libOSMesa is
 *    loaded at runtime, so the same binary works whether or not it is installed (Mesa 25 dropped OSMesa).
 *
 * OBS_HAIKU_GL=egl or OBS_HAIKU_GL=osmesa forces one backend.
 *
 * Readback costs one glReadPixels per displayed frame, which is acceptable for preview/projector sizes and
 * keeps the renderer independent of any toolkit.
 */

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <obs.h>
#include <util/bmem.h>

#include "gl-subsystem.h"

#include <glad/glad_egl.h>

/* clang-format off */
static const EGLint ctx_attribs[] = {
#ifdef _DEBUG
	EGL_CONTEXT_OPENGL_DEBUG, EGL_TRUE,
#endif
	EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
	EGL_CONTEXT_MAJOR_VERSION, 3,
	EGL_CONTEXT_MINOR_VERSION, 3,
	EGL_NONE,
};

static const EGLint config_attribs[] = {
	EGL_RED_SIZE, 8,
	EGL_GREEN_SIZE, 8,
	EGL_BLUE_SIZE, 8,
	EGL_ALPHA_SIZE, 0,
	EGL_DEPTH_SIZE, 0,
	EGL_STENCIL_SIZE, 0,
	EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
	EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
	EGL_NONE,
};
/* clang-format on */

/* OSMesa (GL/osmesa.h), declared here because it is loaded with dlopen() */
typedef struct osmesa_context *OSMesaContext;
typedef void (*OSMESAproc)(void);
#define OSMESA_BGRA 0x1
#define OSMESA_Y_UP 0x11
#define OSMESA_FORMAT 0x22
#define OSMESA_DEPTH_BITS 0x30
#define OSMESA_STENCIL_BITS 0x31
#define OSMESA_ACCUM_BITS 0x32
#define OSMESA_PROFILE 0x33
#define OSMESA_CORE_PROFILE 0x34
#define OSMESA_CONTEXT_MAJOR_VERSION 0x36
#define OSMESA_CONTEXT_MINOR_VERSION 0x37

struct osmesa_api {
	void *lib;
	OSMesaContext (*CreateContextAttribs)(const int *attribs, OSMesaContext share);
	void (*DestroyContext)(OSMesaContext ctx);
	unsigned char (*MakeCurrent)(OSMesaContext ctx, void *buffer, unsigned int type, int width, int height);
	void (*PixelStore)(int pname, int value);
	OSMESAproc (*GetProcAddress)(const char *name);
};

static struct osmesa_api osmesa;

struct gl_windowinfo {
	EGLSurface surface;
	uint8_t *osmesa_buffer; /* OSMesa backend: the swapchain's color buffer (cx * cy * 4) */
	uint32_t cx;
	uint32_t cy;

	void (*present)(void *param, const uint8_t *bgra, uint32_t width, uint32_t height, uint32_t linesize);
	void *param;

	uint8_t *readback;
	size_t readback_size;
};

struct gl_platform {
	bool use_osmesa;

	/* EGL backend */
	EGLDisplay display;
	EGLConfig config;
	EGLContext context;
	EGLSurface pbuffer; /* 1x1 surface used when no swapchain is loaded */
	bool surfaceless;

	/* OSMesa backend */
	OSMesaContext osmesa_ctx;
	uint32_t osmesa_dummy; /* 1x1 buffer used when no swapchain is loaded */
};

#define GL_UNSIGNED_BYTE_VALUE 0x1401 /* GL_UNSIGNED_BYTE, for OSMesaMakeCurrent */

static bool osmesa_load(void)
{
	if (osmesa.lib)
		return true;

	static const char *names[] = {"libOSMesa.so.8", "libOSMesa.so", NULL};
	for (const char **name = names; *name && !osmesa.lib; name++)
		osmesa.lib = dlopen(*name, RTLD_NOW | RTLD_LOCAL);
	if (!osmesa.lib) {
		blog(LOG_ERROR, "Haiku GL: libOSMesa not found: %s", dlerror());
		return false;
	}

	osmesa.CreateContextAttribs = dlsym(osmesa.lib, "OSMesaCreateContextAttribs");
	osmesa.DestroyContext = dlsym(osmesa.lib, "OSMesaDestroyContext");
	osmesa.MakeCurrent = dlsym(osmesa.lib, "OSMesaMakeCurrent");
	osmesa.PixelStore = dlsym(osmesa.lib, "OSMesaPixelStore");
	osmesa.GetProcAddress = dlsym(osmesa.lib, "OSMesaGetProcAddress");

	if (!osmesa.CreateContextAttribs || !osmesa.DestroyContext || !osmesa.MakeCurrent || !osmesa.PixelStore ||
	    !osmesa.GetProcAddress) {
		blog(LOG_ERROR, "Haiku GL: libOSMesa lacks required functions");
		dlclose(osmesa.lib);
		memset(&osmesa, 0, sizeof(osmesa));
		return false;
	}
	return true;
}

static bool osmesa_context_create(struct gl_platform *plat)
{
	if (!osmesa_load())
		return false;

	/* clang-format off */
	const int attribs[] = {
		OSMESA_FORMAT, OSMESA_BGRA,
		OSMESA_DEPTH_BITS, 0,
		OSMESA_STENCIL_BITS, 0,
		OSMESA_ACCUM_BITS, 0,
		OSMESA_PROFILE, OSMESA_CORE_PROFILE,
		OSMESA_CONTEXT_MAJOR_VERSION, 3,
		OSMESA_CONTEXT_MINOR_VERSION, 3,
		0,
	};
	/* clang-format on */

	plat->osmesa_ctx = osmesa.CreateContextAttribs(attribs, NULL);
	if (!plat->osmesa_ctx) {
		blog(LOG_ERROR, "Haiku GL: OSMesa could not create an OpenGL 3.3 core context");
		return false;
	}

	plat->use_osmesa = true;
	blog(LOG_INFO, "Haiku GL: using OSMesa (offscreen Mesa) for rendering");
	return true;
}

static void osmesa_context_destroy(struct gl_platform *plat)
{
	if (!plat->osmesa_ctx)
		return;
	osmesa.MakeCurrent(NULL, NULL, 0, 0, 0);
	osmesa.DestroyContext(plat->osmesa_ctx);
	plat->osmesa_ctx = NULL;
}

static const char *egl_error_string(EGLint error)
{
	switch (error) {
#define OBS_EGL_CASE_ERROR(e) \
	case e:               \
		return #e;
		OBS_EGL_CASE_ERROR(EGL_SUCCESS)
		OBS_EGL_CASE_ERROR(EGL_NOT_INITIALIZED)
		OBS_EGL_CASE_ERROR(EGL_BAD_ACCESS)
		OBS_EGL_CASE_ERROR(EGL_BAD_ALLOC)
		OBS_EGL_CASE_ERROR(EGL_BAD_ATTRIBUTE)
		OBS_EGL_CASE_ERROR(EGL_BAD_CONTEXT)
		OBS_EGL_CASE_ERROR(EGL_BAD_CONFIG)
		OBS_EGL_CASE_ERROR(EGL_BAD_CURRENT_SURFACE)
		OBS_EGL_CASE_ERROR(EGL_BAD_DISPLAY)
		OBS_EGL_CASE_ERROR(EGL_BAD_SURFACE)
		OBS_EGL_CASE_ERROR(EGL_BAD_MATCH)
		OBS_EGL_CASE_ERROR(EGL_BAD_PARAMETER)
		OBS_EGL_CASE_ERROR(EGL_BAD_NATIVE_PIXMAP)
		OBS_EGL_CASE_ERROR(EGL_BAD_NATIVE_WINDOW)
		OBS_EGL_CASE_ERROR(EGL_CONTEXT_LOST)
#undef OBS_EGL_CASE_ERROR
	default:
		return "Unknown";
	}
}

static const char *egl_last_error(void)
{
	return egl_error_string(eglGetError());
}

static bool has_extension(const char *extensions, const char *name)
{
	if (!extensions || !name)
		return false;

	size_t len = strlen(name);
	const char *pos = extensions;

	while ((pos = strstr(pos, name)) != NULL) {
		if ((pos == extensions || pos[-1] == ' ') && (pos[len] == ' ' || pos[len] == '\0'))
			return true;
		pos += len;
	}

	return false;
}

static EGLSurface create_pbuffer(struct gl_platform *plat, uint32_t cx, uint32_t cy)
{
	const EGLint attribs[] = {
		EGL_WIDTH, (EGLint)(cx ? cx : 1), EGL_HEIGHT, (EGLint)(cy ? cy : 1), EGL_NONE,
	};

	EGLSurface surface = eglCreatePbufferSurface(plat->display, plat->config, attribs);
	if (surface == EGL_NO_SURFACE)
		blog(LOG_ERROR, "Haiku EGL: failed to create %ux%u pbuffer: %s", cx, cy, egl_last_error());

	return surface;
}

static EGLSurface current_surface(gs_device_t *device)
{
	if (device->cur_swap && device->cur_swap->wi && device->cur_swap->wi->surface != EGL_NO_SURFACE)
		return device->cur_swap->wi->surface;

	return device->plat->pbuffer;
}

static bool make_current(gs_device_t *device)
{
	struct gl_platform *plat = device->plat;

	if (plat->use_osmesa) {
		struct gl_windowinfo *wi = device->cur_swap ? device->cur_swap->wi : NULL;
		void *buffer = &plat->osmesa_dummy;
		int cx = 1, cy = 1;
		if (wi && wi->osmesa_buffer) {
			buffer = wi->osmesa_buffer;
			cx = (int)wi->cx;
			cy = (int)wi->cy;
		}
		if (!osmesa.MakeCurrent(plat->osmesa_ctx, buffer, GL_UNSIGNED_BYTE_VALUE, cx, cy)) {
			blog(LOG_ERROR, "Haiku GL: OSMesaMakeCurrent(%dx%d) failed", cx, cy);
			return false;
		}
		return true;
	}

	EGLSurface surface = current_surface(device);

	if (!eglMakeCurrent(plat->display, surface, surface, plat->context)) {
		blog(LOG_ERROR, "Haiku EGL: failed to make context current: %s", egl_last_error());
		return false;
	}

	return true;
}

static void release_current(struct gl_platform *plat)
{
	if (plat->use_osmesa) {
		osmesa.MakeCurrent(NULL, NULL, 0, 0, 0);
		return;
	}
	if (!eglMakeCurrent(plat->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT))
		blog(LOG_ERROR, "Haiku EGL: failed to release context: %s", egl_last_error());
}

static bool gl_context_create(struct gl_platform *plat)
{
	EGLint major = 0, minor = 0, count = 0;

	if (!gladLoadEGL()) {
		blog(LOG_ERROR, "Haiku EGL: unable to load EGL entry points");
		return false;
	}

	plat->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (plat->display == EGL_NO_DISPLAY) {
		blog(LOG_ERROR, "Haiku EGL: eglGetDisplay failed: %s", egl_last_error());
		return false;
	}

	if (!eglInitialize(plat->display, &major, &minor)) {
		blog(LOG_ERROR, "Haiku EGL: eglInitialize failed: %s", egl_last_error());
		return false;
	}

	blog(LOG_INFO, "Haiku EGL: version %d.%d, vendor: %s", major, minor,
	     eglQueryString(plat->display, EGL_VENDOR));

	if (!eglBindAPI(EGL_OPENGL_API)) {
		blog(LOG_ERROR, "Haiku EGL: desktop OpenGL is not available: %s", egl_last_error());
		goto fail;
	}

	if (!eglChooseConfig(plat->display, config_attribs, &plat->config, 1, &count) || count == 0) {
		blog(LOG_ERROR, "Haiku EGL: no suitable pbuffer config: %s", egl_last_error());
		goto fail;
	}

	plat->context = eglCreateContext(plat->display, plat->config, EGL_NO_CONTEXT, ctx_attribs);
#ifdef _DEBUG
	if (plat->context == EGL_NO_CONTEXT && eglGetError() == EGL_BAD_ATTRIBUTE) {
		blog(LOG_WARNING, "Haiku EGL: debug contexts unsupported, retrying without");
		plat->context = eglCreateContext(plat->display, plat->config, EGL_NO_CONTEXT, ctx_attribs + 2);
	}
#endif
	if (plat->context == EGL_NO_CONTEXT) {
		blog(LOG_ERROR, "Haiku EGL: unable to create OpenGL 3.3 core context: %s", egl_last_error());
		goto fail;
	}

	plat->pbuffer = create_pbuffer(plat, 1, 1);
	if (plat->pbuffer == EGL_NO_SURFACE) {
		const char *exts = eglQueryString(plat->display, EGL_EXTENSIONS);
		if (!has_extension(exts, "EGL_KHR_surfaceless_context")) {
			blog(LOG_ERROR, "Haiku EGL: neither pbuffers nor surfaceless contexts are available");
			goto fail;
		}

		blog(LOG_INFO, "Haiku EGL: using surfaceless context for offscreen work");
		plat->surfaceless = true;
	}

	return true;

fail:
	if (plat->context != EGL_NO_CONTEXT)
		eglDestroyContext(plat->display, plat->context);
	plat->context = EGL_NO_CONTEXT;
	eglTerminate(plat->display);
	plat->display = EGL_NO_DISPLAY;
	return false;
}

extern struct gl_windowinfo *gl_windowinfo_create(const struct gs_init_data *info)
{
	if (!info->window.present) {
		blog(LOG_ERROR, "Haiku GL: swapchain requested without a present callback");
		return NULL;
	}

	struct gl_windowinfo *wi = bzalloc(sizeof(struct gl_windowinfo));
	wi->surface = EGL_NO_SURFACE;
	/* EGL pbuffers can't be empty; a 0x0 widget gets a 1x1 surface until it is resized */
	wi->cx = info->cx ? info->cx : 1;
	wi->cy = info->cy ? info->cy : 1;
	wi->present = info->window.present;
	wi->param = info->window.param;
	return wi;
}

extern void gl_windowinfo_destroy(struct gl_windowinfo *wi)
{
	if (!wi)
		return;

	bfree(wi->readback);
	bfree(wi);
}

extern struct gl_platform *gl_platform_create(gs_device_t *device, uint32_t adapter)
{
	UNUSED_PARAMETER(adapter);

	struct gl_platform *plat = bzalloc(sizeof(struct gl_platform));
	plat->display = EGL_NO_DISPLAY;
	plat->context = EGL_NO_CONTEXT;
	plat->pbuffer = EGL_NO_SURFACE;

	const char *forced = getenv("OBS_HAIKU_GL");
	const bool want_egl = !forced || strcmp(forced, "osmesa") != 0;
	const bool want_osmesa = !forced || strcmp(forced, "egl") != 0;

	bool created = want_egl && gl_context_create(plat);
	if (!created && want_osmesa) {
		if (want_egl)
			blog(LOG_INFO, "Haiku GL: EGL unavailable (expected with Mesa 22), trying OSMesa");
		created = osmesa_context_create(plat);
	}
	if (!created) {
		blog(LOG_ERROR, "Haiku GL: no OpenGL backend available. Install Mesa: 'pkgman install mesa mesa_swpipe'");
		bfree(plat);
		return NULL;
	}

	device->plat = plat;

	if (!make_current(device))
		goto fail;

	if (plat->use_osmesa) {
		/* OSMesaGetProcAddress resolves every GL entry point through Mesa's dispatch table */
		gladLoadGLLoader((GLADloadproc)osmesa.GetProcAddress);
	} else {
		/* eglGetProcAddress returns core entry points too (EGL 1.5 / EGL_KHR_get_all_proc_addresses);
		 * fall back to dlsym on libGL if that is not available. */
		gladLoadGLLoader((GLADloadproc)eglGetProcAddress);
	}
	if (!GLAD_GL_VERSION_3_3 && !gladLoadGL()) {
		blog(LOG_ERROR, "Haiku GL: failed to load OpenGL entry points");
		goto fail;
	}

	if (!GLAD_GL_VERSION_3_3) {
		blog(LOG_ERROR, "Haiku GL: OpenGL 3.3 is required, got %d.%d", GLVersion.major, GLVersion.minor);
		goto fail;
	}

	blog(LOG_INFO, "Haiku GL: %s", (const char *)glGetString(GL_RENDERER));
	return plat;

fail:
	gl_platform_destroy(plat);
	device->plat = NULL;
	return NULL;
}

extern void gl_platform_destroy(struct gl_platform *plat)
{
	if (!plat)
		return;

	if (plat->use_osmesa) {
		osmesa_context_destroy(plat);
		bfree(plat);
		return;
	}

	eglMakeCurrent(plat->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	if (plat->pbuffer != EGL_NO_SURFACE)
		eglDestroySurface(plat->display, plat->pbuffer);
	eglDestroyContext(plat->display, plat->context);
	eglTerminate(plat->display);
	bfree(plat);
}

static bool swapchain_alloc(struct gl_platform *plat, struct gl_windowinfo *wi, uint32_t cx, uint32_t cy)
{
	if (plat->use_osmesa) {
		uint8_t *buffer = bzalloc((size_t)cx * cy * 4);
		bfree(wi->osmesa_buffer);
		wi->osmesa_buffer = buffer;
		return true;
	}

	EGLSurface surface = create_pbuffer(plat, cx, cy);
	if (surface == EGL_NO_SURFACE)
		return false;
	if (wi->surface != EGL_NO_SURFACE)
		eglDestroySurface(plat->display, wi->surface);
	wi->surface = surface;
	return true;
}

static bool swapchain_valid(const struct gl_platform *plat, const struct gl_windowinfo *wi)
{
	return plat->use_osmesa ? wi->osmesa_buffer != NULL : wi->surface != EGL_NO_SURFACE;
}

extern bool gl_platform_init_swapchain(struct gs_swap_chain *swap)
{
	struct gl_windowinfo *wi = swap->wi;
	return swapchain_alloc(swap->device->plat, wi, wi->cx, wi->cy);
}

extern void gl_platform_cleanup_swapchain(struct gs_swap_chain *swap)
{
	struct gl_windowinfo *wi = swap->wi;
	struct gl_platform *plat = swap->device->plat;

	if (!wi)
		return;

	if (plat->use_osmesa) {
		/* the buffer may be the current draw buffer: switch away before freeing it */
		if (swap->device->cur_swap == swap) {
			swap->device->cur_swap = NULL;
			make_current(swap->device);
		}
		bfree(wi->osmesa_buffer);
		wi->osmesa_buffer = NULL;
		return;
	}

	if (wi->surface != EGL_NO_SURFACE) {
		eglDestroySurface(plat->display, wi->surface);
		wi->surface = EGL_NO_SURFACE;
	}
}

extern void device_enter_context(gs_device_t *device)
{
	make_current(device);
}

extern void device_leave_context(gs_device_t *device)
{
	device->cur_render_target = NULL;
	device->cur_zstencil_buffer = NULL;
	device->cur_vertex_buffer = NULL;
	device->cur_index_buffer = NULL;
	device->cur_swap = NULL;
	device->cur_fbo = NULL;

	release_current(device->plat);
}

extern bool device_enum_adapters(gs_device_t *device, bool (*callback)(void *param, const char *name, uint32_t id),
				 void *param)
{
	UNUSED_PARAMETER(device);

	const char *renderer = (const char *)glGetString(GL_RENDERER);
	callback(param, renderer ? renderer : "Default", 0);
	return true;
}

extern void *device_get_device_obj(gs_device_t *device)
{
	return device->plat->use_osmesa ? (void *)device->plat->osmesa_ctx : (void *)device->plat->context;
}

extern void gl_getclientsize(const struct gs_swap_chain *swap, uint32_t *width, uint32_t *height)
{
	*width = swap->wi->cx;
	*height = swap->wi->cy;
}

extern void gl_clear_context(gs_device_t *device)
{
	release_current(device->plat);
}

/* Called after device_resize(): reallocate the current swapchain's buffer at the new size. */
extern void gl_update(gs_device_t *device)
{
	struct gs_swap_chain *swap = device->cur_swap;
	if (!swap)
		return;

	struct gl_platform *plat = device->plat;
	struct gl_windowinfo *wi = swap->wi;
	uint32_t cx = swap->info.cx ? swap->info.cx : 1;
	uint32_t cy = swap->info.cy ? swap->info.cy : 1;

	if (swapchain_valid(plat, wi) && wi->cx == cx && wi->cy == cy)
		return;

	if (!plat->use_osmesa) {
		/* keep the old pbuffer current until the new one exists */
		EGLSurface old = wi->surface;
		wi->surface = EGL_NO_SURFACE;
		if (!swapchain_alloc(plat, wi, cx, cy)) {
			wi->surface = old;
			return;
		}
		wi->cx = cx;
		wi->cy = cy;
		make_current(device);
		if (old != EGL_NO_SURFACE)
			eglDestroySurface(plat->display, old);
		return;
	}

	/* OSMesa: the old buffer is current; bind the new one before the old one is freed */
	uint8_t *old = wi->osmesa_buffer;
	wi->osmesa_buffer = bzalloc((size_t)cx * cy * 4);
	wi->cx = cx;
	wi->cy = cy;
	make_current(device);
	bfree(old);
}

extern void device_load_swapchain(gs_device_t *device, gs_swapchain_t *swap)
{
	if (device->cur_swap == swap)
		return;

	device->cur_swap = swap;
	make_current(device);
}

extern bool device_is_present_ready(gs_device_t *device)
{
	UNUSED_PARAMETER(device);
	return true;
}

extern void device_present(gs_device_t *device)
{
	struct gs_swap_chain *swap = device->cur_swap;
	if (!swap || !swap->wi || !swapchain_valid(device->plat, swap->wi))
		return;

	struct gl_windowinfo *wi = swap->wi;
	const uint32_t cx = wi->cx;
	const uint32_t cy = wi->cy;
	const uint32_t linesize = cx * 4;
	const size_t size = (size_t)linesize * cy;

	if (wi->readback_size < size * 2) {
		bfree(wi->readback);
		wi->readback = bmalloc(size * 2);
		wi->readback_size = size * 2;
	}

	GLint prev_read_fbo = 0, prev_pack_buffer = 0;
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read_fbo);
	glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prev_pack_buffer);

	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	/* EGL pbuffers are double buffered; OSMesa's framebuffer is single buffered */
	glReadBuffer(device->plat->use_osmesa ? GL_FRONT : GL_BACK);

	uint8_t *raw = wi->readback + size;
	glReadPixels(0, 0, (GLsizei)cx, (GLsizei)cy, GL_BGRA, GL_UNSIGNED_BYTE, raw);

	glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_read_fbo);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)prev_pack_buffer);

	if (!gl_success("glReadPixels"))
		return;

	/* GL rows are bottom-up; views expect top-down. */
	for (uint32_t y = 0; y < cy; y++)
		memcpy(wi->readback + (size_t)y * linesize, raw + (size_t)(cy - 1 - y) * linesize, linesize);

	wi->present(wi->param, wi->readback, cx, cy, linesize);
}

extern bool device_is_monitor_hdr(gs_device_t *device, void *monitor)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(monitor);
	return false;
}
