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
 * A Haiku OpenGL window surface belongs to a BWindow/BView whose looper thread owns it, which does not fit
 * OBS's model of a single graphics thread rendering into arbitrary windows. Instead, every swapchain renders
 * offscreen; on present, the finished frame is read back and handed to the owner of the window through the
 * callback in struct gs_window. The owner (e.g. the Qt frontend) blits it into its view.
 *
 * Two backends, tried in this order (OBS_HAIKU_GL=egl or OBS_HAIKU_GL=osmesa forces one):
 *
 *  - EGL with a surfaceless context; each swapchain is a framebuffer object with a color renderbuffer, which
 *    OBS uses wherever it would use framebuffer 0 (gl_platform_default_framebuffer()). Works with any Mesa EGL
 *    for Haiku: Mesa 25+ (mesa_eglvnd) and the Zink/NVK stack of the nvidia-haiku driver. EGL pbuffers are not
 *    used on purpose: Mesa's Haiku EGL driver reads the size of every surface it makes current from its
 *    window (BitmapHook), and pbuffers have none.
 *  - OSMesa. Mesa 22.0.5, the version HaikuPorts currently ships, has its Haiku EGL driver disabled (its
 *    eglInitialize() always fails), but builds OSMesa on llvmpipe. An OSMesa context renders into a
 *    caller-provided memory buffer, one per swapchain. libOSMesa is loaded at runtime, so the same binary works
 *    whether or not it exists (Mesa 25 removed OSMesa).
 *
 * Unless a backend is forced, EGL is only used after the driver passed a self-test: obs-haiku-gl-probe (installed
 * next to the obs executable, see haiku-gl-probe/gl-probe.c) runs OBS's usage pattern in a separate process
 * under a timeout. A driver that hangs, crashes or renders wrong pixels there gets OSMesa instead of a frozen
 * OBS. See run_gl_probe().
 *
 * A driver can also pass the self-test and hang later on a path the test doesn't reach. With EGL, a watchdog
 * thread notices when a thread has held the context for too long (see watchdog_thread()), logs the driver's
 * identity, records it in a marker file and offers to restart OBS with the CPU renderer. Later launches skip
 * EGL while that same driver is installed (see gl_platform_create()).
 *
 * Readback costs one glReadPixels per displayed frame, which is acceptable for preview/projector sizes and
 * keeps the renderer independent of any toolkit. The rows are handed over bottom-up as GL returns them, with
 * the pointer at the top row and a negative stride, so the consumer's copy into its own image does the vertical
 * flip.
 */

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <OS.h>
#include <image.h>

#include <obs.h>
#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include "gl-subsystem.h"
#include "gl-haiku-alert.h"
#include "haiku-gl-probe/gl-identity.h"

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
	EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
	EGL_NONE,
};
/* clang-format on */

/* OSMesa (GL/osmesa.h), declared here because it is loaded with dlopen() */
typedef struct osmesa_context *OSMesaContext;
typedef void (*OSMESAproc)(void);
#define OSMESA_BGRA 0x1
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
	OSMESAproc (*GetProcAddress)(const char *name);
};

static struct osmesa_api osmesa;

struct gl_windowinfo {
	uint32_t cx;
	uint32_t cy;

	/* EGL backend: framebuffer object + color renderbuffer (created lazily, needs a current context) */
	GLuint fbo;
	GLuint color;
	uint32_t fbo_cx, fbo_cy;
	/* size at which the framebuffer object was last found incomplete; not retried until the size changes */
	uint32_t fbo_failed_cx, fbo_failed_cy;

	/* OSMesa backend: the swapchain's color buffer (cx * cy * 4) */
	uint8_t *osmesa_buffer;

	void (*present)(void *param, const uint8_t *top_row, uint32_t width, uint32_t height, int32_t stride);
	void *param;

	uint8_t *readback;
	size_t readback_size;
};

/* see haiku-gl-probe/gl-identity.h */
struct gl_identity {
	bool valid;
	uint64_t hash;
	char text[GL_IDENTITY_MAX]; /* parts separated by '\n' */
};

struct gl_platform {
	bool use_osmesa;

	/* EGL backend */
	EGLDisplay display;
	EGLConfig config;
	EGLContext context;

	/* OSMesa backend */
	OSMesaContext osmesa_ctx;
	uint32_t osmesa_dummy; /* 1x1 buffer used when no swapchain is loaded */

	/* EGL backend: the driver's identity and the hang watchdog (see watchdog_thread()) */
	struct gl_identity identity;         /* reported by the self-test (valid only if it passed) */
	struct gl_identity process_identity; /* computed in this process once EGL is up */
	bool marker_preexisting;             /* a hang record existed at startup: never removed by the watchdog */
	char renderer[256];
	bool watchdog_running;
	pthread_t watchdog;
	os_event_t *watchdog_event;
	int watchdog_timeout_s;
	/* Heartbeat. Written only by the thread that holds libobs's graphics mutex (device_enter_context() and
	 * device_leave_context() run under it), read by the watchdog. Sequentially consistent atomics: holder
	 * and held_since are stored before hold_seq is incremented, and the watchdog loads hold_seq first. */
	_Atomic uint64_t held_since; /* os_gettime_ns() when the context was entered, 0 while released */
	atomic_int holder;           /* thread_id of the thread that entered */
	_Atomic uint64_t hold_seq;   /* number of device_enter_context() calls so far: identifies a hold */
};

#define GL_UNSIGNED_BYTE_VALUE 0x1401 /* GL_UNSIGNED_BYTE, for OSMesaMakeCurrent */

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

/* ---------------------------------------------------------------------------------------------------------- */
/* EGL                                                                                                        */

static bool egl_context_create(struct gl_platform *plat)
{
	EGLint major = 0, minor = 0, count = 0;

	if (!gladLoadEGL()) {
		blog(LOG_INFO, "Haiku EGL: unable to load EGL entry points");
		return false;
	}

	plat->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (plat->display == EGL_NO_DISPLAY) {
		blog(LOG_INFO, "Haiku EGL: eglGetDisplay failed: %s", egl_last_error());
		return false;
	}

	if (!eglInitialize(plat->display, &major, &minor)) {
		blog(LOG_INFO, "Haiku EGL: eglInitialize failed: %s", egl_last_error());
		plat->display = EGL_NO_DISPLAY;
		return false;
	}

	const char *vendor = eglQueryString(plat->display, EGL_VENDOR);
	blog(LOG_INFO, "Haiku EGL: version %d.%d, vendor: %s", major, minor, vendor ? vendor : "?");

	const char *exts = eglQueryString(plat->display, EGL_EXTENSIONS);
	if (!has_extension(exts, "EGL_KHR_surfaceless_context")) {
		blog(LOG_INFO, "Haiku EGL: EGL_KHR_surfaceless_context missing");
		goto fail;
	}

	if (!eglBindAPI(EGL_OPENGL_API)) {
		blog(LOG_INFO, "Haiku EGL: desktop OpenGL is not available: %s", egl_last_error());
		goto fail;
	}

	if (!eglChooseConfig(plat->display, config_attribs, &plat->config, 1, &count) || count == 0) {
		blog(LOG_INFO, "Haiku EGL: no suitable config: %s", egl_last_error());
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
		blog(LOG_INFO, "Haiku EGL: unable to create OpenGL 3.3 core context: %s", egl_last_error());
		goto fail;
	}

	return true;

fail:
	eglTerminate(plat->display);
	plat->display = EGL_NO_DISPLAY;
	return false;
}

static void egl_context_destroy(struct gl_platform *plat)
{
	if (plat->display == EGL_NO_DISPLAY)
		return;
	eglMakeCurrent(plat->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	if (plat->context != EGL_NO_CONTEXT)
		eglDestroyContext(plat->display, plat->context);
	eglTerminate(plat->display);
	plat->context = EGL_NO_CONTEXT;
	plat->display = EGL_NO_DISPLAY;
}

/* (Re)creates the swapchain's framebuffer object at its current size. Needs the context to be current. */
static bool egl_swapchain_fbo(struct gl_windowinfo *wi)
{
	if (wi->fbo && wi->fbo_cx == wi->cx && wi->fbo_cy == wi->cy)
		return true;
	if (wi->fbo && wi->fbo_failed_cx == wi->cx && wi->fbo_failed_cy == wi->cy)
		return false;

	if (!wi->fbo) {
		glGenFramebuffers(1, &wi->fbo);
		glGenRenderbuffers(1, &wi->color);
	}

	GLint prev_rb = 0, prev_draw = 0;
	glGetIntegerv(GL_RENDERBUFFER_BINDING, &prev_rb);
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);

	glBindRenderbuffer(GL_RENDERBUFFER, wi->color);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, (GLsizei)wi->cx, (GLsizei)wi->cy);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, wi->fbo);
	glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, wi->color);
	const GLenum status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);

	glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)prev_rb);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prev_draw);

	if (status != GL_FRAMEBUFFER_COMPLETE) {
		blog(LOG_ERROR, "Haiku GL: swapchain framebuffer %ux%u incomplete (0x%x)", wi->cx, wi->cy, status);
		/* the renderbuffer storage no longer matches fbo_cx/fbo_cy either */
		wi->fbo_cx = wi->fbo_cy = 0;
		wi->fbo_failed_cx = wi->cx;
		wi->fbo_failed_cy = wi->cy;
		return false;
	}

	wi->fbo_cx = wi->cx;
	wi->fbo_cy = wi->cy;
	wi->fbo_failed_cx = wi->fbo_failed_cy = 0;
	return true;
}

static void egl_swapchain_free(struct gl_windowinfo *wi)
{
	if (wi->fbo)
		glDeleteFramebuffers(1, &wi->fbo);
	if (wi->color)
		glDeleteRenderbuffers(1, &wi->color);
	wi->fbo = wi->color = 0;
	wi->fbo_cx = wi->fbo_cy = 0;
	wi->fbo_failed_cx = wi->fbo_failed_cy = 0;
}

/* ---------------------------------------------------------------------------------------------------------- */
/* OSMesa                                                                                                     */

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
	osmesa.GetProcAddress = dlsym(osmesa.lib, "OSMesaGetProcAddress");

	if (!osmesa.CreateContextAttribs || !osmesa.DestroyContext || !osmesa.MakeCurrent || !osmesa.GetProcAddress) {
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

/* ---------------------------------------------------------------------------------------------------------- */
/* Common                                                                                                     */

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

	if (!eglMakeCurrent(plat->display, EGL_NO_SURFACE, EGL_NO_SURFACE, plat->context)) {
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

/* With EGL, "framebuffer 0" of the current swapchain is its framebuffer object. Rebind it if OBS is currently
 * drawing to the swapchain (cur_fbo == NULL) so a swapchain switch or resize takes effect. */
static void bind_swapchain_framebuffer(gs_device_t *device)
{
	if (device->plat->use_osmesa || device->cur_fbo)
		return;
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gl_platform_default_framebuffer(device));
}

GLuint gl_platform_default_framebuffer(gs_device_t *device)
{
	if (device->plat->use_osmesa || !device->cur_swap || !device->cur_swap->wi)
		return 0;
	struct gl_windowinfo *wi = device->cur_swap->wi;
	if (!egl_swapchain_fbo(wi))
		return 0;
	return wi->fbo;
}

extern struct gl_windowinfo *gl_windowinfo_create(const struct gs_init_data *info)
{
	if (!info->window.present) {
		blog(LOG_ERROR, "Haiku GL: swapchain requested without a present callback");
		return NULL;
	}

	struct gl_windowinfo *wi = bzalloc(sizeof(struct gl_windowinfo));
	/* buffers can't be empty; a 0x0 widget gets 1x1 until it is resized */
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

	bfree(wi->osmesa_buffer);
	bfree(wi->readback);
	bfree(wi);
}

/* Makes the freshly created context current and loads the GL entry points through it. */
static bool load_gl(gs_device_t *device)
{
	struct gl_platform *plat = device->plat;

	if (!make_current(device))
		return false;

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
		return false;
	}

	if (!GLAD_GL_VERSION_3_3) {
		blog(LOG_ERROR, "Haiku GL: OpenGL 3.3 is required, got %d.%d", GLVersion.major, GLVersion.minor);
		return false;
	}
	return true;
}

/* ---------------------------------------------------------------------------------------------------------- */
/* Driver self-test                                                                                           */

#define GL_PROBE_NAME "obs-haiku-gl-probe"
#define GL_PROBE_DEFAULT_TIMEOUT_S 8.0
#define GL_PROBE_MAX_LOG_LINES 40
#define GL_PROBE_REAP_TIMEOUT_MS 2000

enum gl_probe_result {
	GL_PROBE_PASSED,
	GL_PROBE_NO_EGL,      /* the probe found no usable EGL display */
	GL_PROBE_FAILED,      /* hang, crash, wrong pixels, no suitable context: don't use EGL */
	GL_PROBE_UNAVAILABLE, /* skipped, missing or could not be run: behave as without a probe */
};

/* exit codes of obs-haiku-gl-probe (haiku-gl-probe/gl-probe.c) */
static const char *gl_probe_exit_string(int code)
{
	switch (code) {
	case 2:
		return "no usable EGL display";
	case 3:
		return "no usable OpenGL 3.3 core context";
	case 4:
		return "wrong pixels or an OpenGL error";
	case 5:
		return "the driver stopped responding (a fence or a driver call did not return)";
	default:
		return "unexpected exit status";
	}
}

struct gl_probe_output {
	char line[1536]; /* an identity part holds a path (B_PATH_NAME_LENGTH) */
	size_t len;
	unsigned lines;

	/* the driver identity the probe reports ("IDENTITY <hash>", "IDENTITY-PART <part>" lines) */
	bool have_hash;
	uint64_t hash;
	struct dstr identity;
};

#define GL_PROBE_IDENTITY "IDENTITY "
#define GL_PROBE_IDENTITY_PART "IDENTITY-PART "

static void gl_probe_flush_line(struct gl_probe_output *out)
{
	if (!out->len)
		return;
	out->line[out->len] = '\0';
	out->len = 0;

	if (strncmp(out->line, GL_PROBE_IDENTITY, strlen(GL_PROBE_IDENTITY)) == 0) {
		char *end = NULL;
		out->hash = strtoull(out->line + strlen(GL_PROBE_IDENTITY), &end, 16);
		out->have_hash = end && end != out->line + strlen(GL_PROBE_IDENTITY);
		return;
	}
	if (strncmp(out->line, GL_PROBE_IDENTITY_PART, strlen(GL_PROBE_IDENTITY_PART)) == 0) {
		if (out->identity.len)
			dstr_cat_ch(&out->identity, '\n');
		dstr_cat(&out->identity, out->line + strlen(GL_PROBE_IDENTITY_PART));
		return;
	}

	if (out->lines < GL_PROBE_MAX_LOG_LINES)
		blog(LOG_INFO, "Haiku GL probe: %s", out->line);
	else if (out->lines == GL_PROBE_MAX_LOG_LINES)
		blog(LOG_INFO, "Haiku GL probe: (further output dropped)");
	out->lines++;
}

/* The probe's identity, if it reported a complete one (its hash matches the text). */
static void gl_probe_take_identity(struct gl_probe_output *out, struct gl_identity *id)
{
	id->valid = false;
	if (!out->have_hash || !out->identity.len || out->identity.len >= sizeof(id->text))
		return;
	snprintf(id->text, sizeof(id->text), "%s", out->identity.array);
	id->hash = gl_identity_hash(id->text);
	if (id->hash != out->hash) {
		blog(LOG_WARNING, "Haiku GL: driver self-test reported an inconsistent driver identity, ignored");
		return;
	}
	id->valid = true;
}

static void gl_probe_add_output(struct gl_probe_output *out, const char *data, size_t size)
{
	for (size_t i = 0; i < size; i++) {
		if (data[i] == '\n' || data[i] == '\r') {
			gl_probe_flush_line(out);
		} else {
			out->line[out->len++] = data[i];
			if (out->len == sizeof(out->line) - 1)
				gl_probe_flush_line(out);
		}
	}
}

/* waitpid() with a deadline (os_gettime_ns() based). Returns pid when reaped, 0 on timeout, -1 on error. */
static pid_t gl_probe_reap(pid_t pid, int *status, uint64_t deadline)
{
	for (;;) {
		pid_t ret = waitpid(pid, status, WNOHANG);
		if (ret == pid || (ret == -1 && errno != EINTR))
			return ret;
		if (os_gettime_ns() >= deadline)
			return 0;
		os_sleep_ms(5);
	}
}

/*
 * The signals the probe gets with their default action (posix_spawnattr_setsigdefault). Haiku's posix_spawn
 * calls sigaction(SIG_DFL) for every member and fails the whole spawn on the first rejection, and the kernel
 * rejects SIGKILL, SIGKILLTHR, SIGSTOP and its internal thread signals (BLOCKABLE_SIGNALS), so a full set
 * never spawns. Only ignored dispositions survive exec, so these are the catchable signals OBS or one of its
 * libraries may have set to SIG_IGN.
 *
 * Note: the probe inherits every OBS file descriptor that lacks FD_CLOEXEC; Haiku's posix_spawn has no
 * close-by-default flag or closefrom file action, and the probe holds them only for its short lifetime.
 */
static void gl_probe_default_signals(sigset_t *set)
{
	static const int signals[] = {
		SIGHUP,  SIGINT,  SIGQUIT, SIGILL,  SIGABRT, SIGFPE,  SIGSEGV, SIGPIPE, SIGALRM, SIGTERM, SIGCHLD,
		SIGUSR1, SIGUSR2, SIGTSTP, SIGTTIN, SIGTTOU, SIGCONT,
#ifdef SIGBUS
		SIGBUS,
#endif
#ifdef SIGTRAP
		SIGTRAP,
#endif
#ifdef SIGPOLL
		SIGPOLL,
#endif
#if defined(SIGIO) && (!defined(SIGPOLL) || SIGIO != SIGPOLL)
		SIGIO,
#endif
#ifdef SIGWINCH
		SIGWINCH,
#endif
#ifdef SIGPROF
		SIGPROF,
#endif
#ifdef SIGSYS
		SIGSYS,
#endif
#ifdef SIGURG
		SIGURG,
#endif
#ifdef SIGVTALRM
		SIGVTALRM,
#endif
#ifdef SIGXCPU
		SIGXCPU,
#endif
#ifdef SIGXFSZ
		SIGXFSZ,
#endif
	};
	sigemptyset(set);
	for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++)
		sigaddset(set, signals[i]);
}

/* Moves fd above stdin/stdout/stderr (OBS may have been started with them closed) and marks it close-on-exec. */
static int gl_probe_fd(int fd)
{
	if (fd >= 0 && fd <= STDERR_FILENO) {
		int moved = fcntl(fd, F_DUPFD, STDERR_FILENO + 1);
		close(fd);
		fd = moved;
	}
	if (fd >= 0)
		fcntl(fd, F_SETFD, FD_CLOEXEC);
	return fd;
}

/*
 * Runs obs-haiku-gl-probe, which drives the EGL driver the way libobs does with a Screen Capture source in the
 * scene and a recording in progress (see haiku-gl-probe/gl-probe.c: surfaceless context handed between
 * threads, a 2560x1080 capture streamed through its pixel unpack buffer every other frame, a canvas render
 * target, NV12 stage surfaces mapped a frame late, two previews read back with glReadPixels, one of them
 * resized and reopened) for 60 frames paced at 60 fps, and checks the pixels. A driver that hangs (as a young
 * NVK/Zink once did on its first frames) would otherwise freeze OBS's graphics thread and with it the UI; in a
 * child process it costs the timeout and a fallback to OSMesa. A step that hangs inside the probe is reported
 * by the probe itself (a "STALL:" line naming the call) before this timeout kills it. The probe runs on every
 * launch, so a driver update is always re-tested; on a healthy driver it costs one EGL initialisation and about
 * a second of rendering. Its output goes into the OBS log, except the driver identity it reports, which is
 * returned in *identity (valid only if it passed).
 */
static enum gl_probe_result run_gl_probe(struct gl_identity *identity)
{
	const char *skip = getenv("OBS_HAIKU_GL_PROBE");
	if (skip && strcmp(skip, "0") == 0) {
		blog(LOG_INFO, "Haiku GL: driver self-test skipped (OBS_HAIKU_GL_PROBE=0)");
		return GL_PROBE_UNAVAILABLE;
	}

	double timeout = GL_PROBE_DEFAULT_TIMEOUT_S;
	const char *timeout_env = getenv("OBS_HAIKU_GL_PROBE_TIMEOUT");
	if (timeout_env && *timeout_env) {
		char *end = NULL;
		double value = strtod(timeout_env, &end);
		if (end != timeout_env && value > 0.0 && value <= 3600.0)
			timeout = value;
		else
			blog(LOG_WARNING, "Haiku GL: ignoring invalid OBS_HAIKU_GL_PROBE_TIMEOUT '%s'", timeout_env);
	}

	char *path = os_get_executable_path_ptr(GL_PROBE_NAME);
	if (!path || access(path, X_OK) != 0) {
		blog(LOG_WARNING, "Haiku GL: driver self-test %s not found, using EGL without it",
		     path ? path : GL_PROBE_NAME);
		bfree(path);
		return GL_PROBE_UNAVAILABLE;
	}

	int fds[2];
	if (pipe(fds) != 0) {
		blog(LOG_WARNING, "Haiku GL: driver self-test not run: pipe() failed (%s)", strerror(errno));
		bfree(path);
		return GL_PROBE_UNAVAILABLE;
	}
	fds[0] = gl_probe_fd(fds[0]);
	fds[1] = gl_probe_fd(fds[1]);
	if (fds[0] < 0 || fds[1] < 0) {
		if (fds[0] >= 0)
			close(fds[0]);
		if (fds[1] >= 0)
			close(fds[1]);
		blog(LOG_WARNING, "Haiku GL: driver self-test not run: no file descriptors");
		bfree(path);
		return GL_PROBE_UNAVAILABLE;
	}

	/* the child's stdout and stderr go into the pipe, stdin is /dev/null; dup2 clears close-on-exec */
	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
	posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);

	/* OBS's threads may block or catch signals; the probe starts with defaults so SIGALRM/SIGKILL work */
	posix_spawnattr_t attr;
	sigset_t set;
	posix_spawnattr_init(&attr);
	sigemptyset(&set);
	posix_spawnattr_setsigmask(&attr, &set);
	gl_probe_default_signals(&set);
	posix_spawnattr_setsigdefault(&attr, &set);
	posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);

	/* the probe reports a stuck step after half the timeout, so the report reaches the log before the kill */
	char timeout_arg[16], stall_arg[16];
	snprintf(timeout_arg, sizeof(timeout_arg), "%u", (unsigned)(timeout + 0.999));
	snprintf(stall_arg, sizeof(stall_arg), "%u", timeout >= 4.0 ? (unsigned)(timeout / 2.0) : 2u);
	char *argv[] = {path, "--timeout", timeout_arg, "--stall", stall_arg, NULL};
	extern char **environ;

	const uint64_t start = os_gettime_ns();
	pid_t pid = -1;
	int err = posix_spawn(&pid, path, &actions, &attr, argv, environ);
	posix_spawn_file_actions_destroy(&actions);
	posix_spawnattr_destroy(&attr);
	close(fds[1]);
	if (err != 0) {
		/* Haiku's posix_spawn forks first: if the child failed before exec, it exits right after
		 * reporting the error, too late for posix_spawn's own WNOHANG waitpid; reap it here */
		if (pid > 0) {
			int ignored;
			gl_probe_reap(pid, &ignored, os_gettime_ns() + GL_PROBE_REAP_TIMEOUT_MS * 1000000ull);
		}
		blog(LOG_WARNING, "Haiku GL: driver self-test %s could not be started (%s), using EGL without it", path,
		     strerror(err));
		close(fds[0]);
		bfree(path);
		return GL_PROBE_UNAVAILABLE;
	}
	bfree(path);

	/* collect the output until the probe closes the pipe (it exited) or the time is up */
	const uint64_t deadline = start + (uint64_t)(timeout * 1e9);
	struct gl_probe_output out = {0};
	dstr_init(&out.identity);
	bool timed_out = false;
	for (;;) {
		const uint64_t now = os_gettime_ns();
		if (now >= deadline) {
			timed_out = true;
			break;
		}
		struct pollfd pfd = {.fd = fds[0], .events = POLLIN};
		int ready = poll(&pfd, 1, (int)((deadline - now) / 1000000 + 1));
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready < 0)
			break;
		if (ready == 0)
			continue;
		char buf[512];
		ssize_t got = read(fds[0], buf, sizeof(buf));
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			break; /* EOF */
		gl_probe_add_output(&out, buf, (size_t)got);
	}
	gl_probe_flush_line(&out);
	close(fds[0]);
	gl_probe_take_identity(&out, identity);
	dstr_free(&out.identity);

	int status = 0;
	pid_t reaped = timed_out ? 0 : gl_probe_reap(pid, &status, deadline);
	if (reaped == 0) {
		timed_out = true;
		/* SIGKILL ends every thread of the team, including one stuck in the driver */
		kill(pid, SIGKILL);
		reaped = gl_probe_reap(pid, &status, os_gettime_ns() + GL_PROBE_REAP_TIMEOUT_MS * 1000000ull);
		if (reaped == 0)
			blog(LOG_WARNING, "Haiku GL: driver self-test (team %d) did not go away after SIGKILL",
			     (int)pid);
	}
	const double elapsed_ms = (double)(os_gettime_ns() - start) / 1e6;

	if (timed_out) {
		blog(LOG_WARNING,
		     "Haiku GL: the OpenGL driver failed its self-test: no result after %.0f s, the driver hangs "
		     "(OBS_HAIKU_GL_PROBE_TIMEOUT=<seconds> allows a slower driver more time)",
		     timeout);
		return GL_PROBE_FAILED;
	}
	if (reaped < 0) {
		/* ECHILD: someone else reaped it (SIGCHLD ignored); the result is unknown */
		blog(LOG_WARNING, "Haiku GL: driver self-test result unavailable (%s), using EGL without it",
		     strerror(errno));
		return GL_PROBE_UNAVAILABLE;
	}
	if (WIFSIGNALED(status)) {
		blog(LOG_WARNING, "Haiku GL: the OpenGL driver failed its self-test: the test crashed (signal %d)",
		     WTERMSIG(status));
		return GL_PROBE_FAILED;
	}
	const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	if (code == 0) {
		blog(LOG_INFO, "Haiku GL: driver self-test passed (%.0f ms)", elapsed_ms);
		return GL_PROBE_PASSED;
	}
	if (code == 2) {
		blog(LOG_INFO, "Haiku GL: driver self-test: %s", gl_probe_exit_string(code));
		return GL_PROBE_NO_EGL;
	}
	blog(LOG_WARNING, "Haiku GL: the OpenGL driver failed its self-test: %s (exit status %d)",
	     gl_probe_exit_string(code), code);
	return GL_PROBE_FAILED;
}

/* ---------------------------------------------------------------------------------------------------------- */
/* Driver identity and the hang record                                                                        */

/*
 * The hang record: a small text file in OBS's config directory, written by the watchdog when the hardware
 * driver hangs, holding the hung driver's identity (haiku-gl-probe/gl-identity.h). While it exists and the
 * installed driver has the same identity, OBS uses the CPU renderer; every such start says so in the log,
 * with the file's path. A changed identity (driver updated) removes it.
 *
 * It holds up to two identities, because the same driver stack does not look the same from two processes (OBS
 * has more libraries loaded than the self-test, and the image list is part of the identity): the one the
 * self-test reported ("identity", if it ran) and the one computed inside OBS ("process-identity", always).
 * A later start compares like with like: self-test against self-test if both exist, else process against
 * process. Only such a comparison can remove the record.
 */
#define GL_HANG_MARKER "obs-studio/haiku-gl-hang"
#define GL_HANG_MARKER_PROBE_KEY "identity "
#define GL_HANG_MARKER_PROCESS_KEY "process-identity "

struct hang_marker {
	bool have_probe, have_process;
	uint64_t probe_hash, process_hash;
};

static void log_identity(int level, const char *prefix, const struct gl_identity *id)
{
	blog(level, "%sdriver identity %016" PRIx64 ":", prefix, id->hash);
	const char *part = id->text;
	while (part && *part) {
		const char *end = strchr(part, '\n');
		const int len = end ? (int)(end - part) : (int)strlen(part);
		blog(level, "%s  %.*s", prefix, len, part);
		part = end ? end + 1 : NULL;
	}
}

/* in-process identity; needs the EGL context current */
static void identity_compute(struct gl_identity *id)
{
	gl_identity_build(id->text, sizeof(id->text), (const char *)glGetString(GL_RENDERER),
			  (const char *)glGetString(GL_VERSION));
	id->hash = gl_identity_hash(id->text);
	id->valid = true;
}

static bool hang_marker_parse_hash(const char *line, const char *key, uint64_t *hash)
{
	const size_t key_len = strlen(key);
	if (strncmp(line, key, key_len) != 0)
		return false;
	char *end = NULL;
	*hash = strtoull(line + key_len, &end, 16);
	return end && end != line + key_len;
}

/* Reads the hang record. Returns false if there is none (or it holds no identity). */
static bool hang_marker_read(struct hang_marker *marker)
{
	char *path = os_get_config_path_ptr(GL_HANG_MARKER);
	char *text = path ? os_quick_read_utf8_file(path) : NULL;

	memset(marker, 0, sizeof(*marker));
	for (const char *line = text; line && *line;) {
		if (hang_marker_parse_hash(line, GL_HANG_MARKER_PROBE_KEY, &marker->probe_hash))
			marker->have_probe = true;
		else if (hang_marker_parse_hash(line, GL_HANG_MARKER_PROCESS_KEY, &marker->process_hash))
			marker->have_process = true;
		line = strchr(line, '\n');
		if (line)
			line++;
	}
	const bool found = marker->have_probe || marker->have_process;
	if (text && !found)
		blog(LOG_WARNING, "Haiku GL: ignoring the unreadable hang record %s (delete it)", path);
	bfree(text);
	bfree(path);
	return found;
}

static void hang_marker_remove(void)
{
	char *path = os_get_config_path_ptr(GL_HANG_MARKER);
	if (path && os_unlink(path) == 0)
		blog(LOG_INFO, "Haiku GL: removed the hang record %s", path);
	bfree(path);
}

static void hang_marker_write_parts(FILE *file, const char *key, const struct gl_identity *id)
{
	const char *part = id->text;
	while (part && *part) {
		const char *end = strchr(part, '\n');
		const int len = end ? (int)(end - part) : (int)strlen(part);
		fprintf(file, "%s %.*s\n", key, len, part);
		part = end ? end + 1 : NULL;
	}
}

/* Written to a temporary file that is renamed over the record, so a reset in the middle (likely enough after
 * a GPU hang) leaves the old state or the new one, never half a file. */
static bool hang_marker_write(const struct gl_identity *probe_id, const struct gl_identity *process_id,
			      const char *renderer)
{
	char *dir = os_get_config_path_ptr("obs-studio");
	char *path = os_get_config_path_ptr(GL_HANG_MARKER);
	struct dstr tmp = {0};
	bool ok = false;

	if (dir && path && os_mkdirs(dir) != MKDIR_ERROR) {
		dstr_printf(&tmp, "%s.tmp", path);
		FILE *file = fopen(tmp.array, "w");
		if (file) {
			fprintf(file,
				"# Written by OBS Studio when the hardware OpenGL driver stopped responding.\n"
				"# While this driver (identity below) is installed, OBS uses the CPU renderer.\n"
				"# Updating the driver or deleting this file makes OBS use the driver again;\n"
				"# OBS_HAIKU_GL=egl uses it for one session regardless.\n"
				"# Driver: %s\n",
				renderer);
			if (probe_id->valid)
				fprintf(file, GL_HANG_MARKER_PROBE_KEY "%016" PRIx64 "\n", probe_id->hash);
			if (process_id->valid)
				fprintf(file, GL_HANG_MARKER_PROCESS_KEY "%016" PRIx64 "\n", process_id->hash);
			if (probe_id->valid)
				hang_marker_write_parts(file, "part", probe_id);
			if (process_id->valid)
				hang_marker_write_parts(file, "process-part", process_id);
			/* the machine may well need a reset after a GPU hang: get it onto the disk now */
			ok = fflush(file) == 0 && fsync(fileno(file)) == 0;
			ok = fclose(file) == 0 && ok;
			ok = ok && rename(tmp.array, path) == 0;
			if (!ok) {
				const int saved = errno;
				unlink(tmp.array);
				errno = saved;
			}
		}
	}
	if (ok)
		blog(LOG_ERROR, "Haiku GL watchdog: recorded the hang in %s", path);
	else
		blog(LOG_ERROR, "Haiku GL watchdog: unable to write the hang record %s (%s)", path ? path : "?",
		     strerror(errno));
	dstr_free(&tmp);
	bfree(dir);
	bfree(path);
	return ok;
}

/* Whether libOSMesa can be found on the library path, without loading it into a process that has a hung
 * driver in it. */
static bool osmesa_installed(void)
{
	if (osmesa.lib)
		return true;

	const char *env = getenv("LIBRARY_PATH");
	char *list = bstrdup(env && *env ? env
					 : "/boot/home/config/non-packaged/lib:/boot/home/config/lib:"
					   "/boot/system/non-packaged/lib:/boot/system/lib");
	char *app_dir = os_get_executable_path_ptr("");
	bool found = false;
	char *save = NULL;

	for (char *dir = strtok_r(list, ":", &save); dir && !found; dir = strtok_r(NULL, ":", &save)) {
		struct dstr path = {0};
		/* %A is the application's directory */
		if (strncmp(dir, "%A", 2) == 0) {
			dstr_copy(&path, app_dir ? app_dir : "");
			dstr_cat(&path, dir + 2);
		} else {
			dstr_copy(&path, dir);
		}
		const size_t base = path.len;
		static const char *names[] = {"/libOSMesa.so.8", "/libOSMesa.so"};
		for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && !found; i++) {
			dstr_resize(&path, base);
			dstr_cat(&path, names[i]);
			found = access(path.array, R_OK) == 0;
		}
		dstr_free(&path);
	}
	bfree(app_dir);
	bfree(list);
	return found;
}

/* ---------------------------------------------------------------------------------------------------------- */
/* Hang watchdog (EGL only)                                                                                   */

/*
 * libobs's graphics thread enters the context (gs_enter_context() -> device_enter_context(), once per outermost
 * gs_enter_context(): nested calls only count references) several times per frame and leaves it in between,
 * and it sleeps between frames without it; other threads enter it briefly to create or update resources. All
 * of them hold libobs's graphics mutex meanwhile, so one thread at a time writes the heartbeat. A thread that
 * holds the context for many seconds without getting anything done is stuck in the driver, and takes the UI
 * with it (the UI thread waits for the graphics mutex).
 *
 * The watchdog must never mistake a healthy OBS for a hung one: it can end OBS in the middle of a recording
 * (only if the user says so, but the offer alone does harm). So a hold counts as a hang only if all of this
 * is true, sampled once per second:
 *  - the same hold (hold_seq unchanged, held_since set) was seen for the whole timeout. The time is the sum of
 *    the watchdog's own ticks, not a difference of timestamps: when the watchdog itself did not run (the team
 *    was stopped by a debugger or SIGSTOP, the machine stalled or slept) the observation starts over instead
 *    of finding a "long" hold afterwards;
 *  - the holding thread was blocked (waiting, sleeping) in at least nine of ten samples and used next to no
 *    CPU time. A thread that computes (shader compilation, a software renderer, a slow plugin) is busy, not
 *    hung; that is logged once and watched further;
 *  - no debugger is attached to the team. That includes the crash dialog of the debug_server: a thread that
 *    crashed while holding the context is blocked too, and the driver must not be blamed for it;
 *  - the context was entered GL_WATCHDOG_ARM_ENTERS times before. obs_init_graphics() compiles every effect
 *    in one hold, and a cold shader cache on a young driver can make the first frames take seconds; the
 *    graphics thread alone gets there within a few seconds of running.
 * What remains undetected on purpose: a driver that spins instead of blocking. OBS then freezes as it would
 * without a watchdog.
 *
 * On a hang the watchdog logs it, writes the hang record (now, not when the user answers: after a GPU hang the
 * machine may have to be reset) and shows an alert offering to restart OBS with the CPU renderer. It never
 * ends OBS by itself. If the hold ends after all, the alert is closed and the record removed.
 */
#define GL_WATCHDOG_DEFAULT_TIMEOUT_S 15
#define GL_WATCHDOG_MIN_TIMEOUT_S 3
#define GL_WATCHDOG_ARM_ENTERS 300
#define GL_WATCHDOG_TICK_MS 1000
/* a tick longer than this means the watchdog thread itself was not running */
#define GL_WATCHDOG_MAX_TICK_NS 3000000000ull

/*
 * Restarts OBS with the CPU renderer: a shell waits for this team to end (so OBS's single-instance lock is
 * free again; at most 20 s, a thread stuck in a kernel driver can keep a dying team around), then runs the
 * same executable with the same arguments and OBS_HAIKU_GL=osmesa, so the new instance does not touch the
 * hung driver at all, not even for the self-test. Then ends this team. Only returns if nothing could be
 * started.
 *
 * The shell is started with load_image(), which creates the new team directly. posix_spawn() with attributes
 * or file actions forks on Haiku, and a fork of this team would have to copy the address space of a process
 * with a hung graphics driver in it (and reserve memory for all of it).
 */
static void relaunch_obs(void)
{
	char exe[B_PATH_NAME_LENGTH] = "";
	image_info image;
	int32 cookie = 0;
	while (get_next_image_info(B_CURRENT_TEAM, &cookie, &image) == B_OK) {
		if (image.type == B_APP_IMAGE) {
			/* image.name is the path as exec()ed, relative for "./obs"; the shell below starts in this
			 * team's working directory, which OBS may have changed since, so resolve it here */
			if (!realpath(image.name, exe))
				snprintf(exe, sizeof(exe), "%s", image.name);
			break;
		}
	}
	if (!*exe) {
		blog(LOG_ERROR, "Haiku GL watchdog: unable to find the OBS executable, not restarting");
		return;
	}

	/* libobs keeps a copy of main()'s arguments (obs_set_cmdline_args()) */
	const struct obs_cmdline_args args = obs_get_cmdline_args();
	const int extra = args.argc > 1 && args.argv ? args.argc - 1 : 0;
	char team[16];
	snprintf(team, sizeof(team), "%d", (int)getpid());

	/* every argument is passed as its own word ("$@"), nothing is re-parsed by the shell */
	static const char *script = "team=$1; shift; i=0; "
				    "while kill -0 \"$team\" 2>/dev/null && [ $i -lt 100 ]; do sleep 0.2; i=$((i+1)); done; "
				    "exec \"$@\"";
	const char **argv = bzalloc(sizeof(char *) * (size_t)(extra + 7));
	int argc = 0;
	argv[argc++] = "/bin/sh";
	argv[argc++] = "-c";
	argv[argc++] = script;
	argv[argc++] = "obs-haiku-restart";
	argv[argc++] = team;
	argv[argc++] = exe;
	for (int i = 1; i <= extra; i++)
		argv[argc++] = args.argv[i];
	argv[argc] = NULL;

	extern char **environ;
	size_t env_count = 0;
	while (environ[env_count])
		env_count++;
	const char **envp = bzalloc(sizeof(char *) * (env_count + 2));
	size_t e = 0;
	for (size_t i = 0; i < env_count; i++) {
		if (strncmp(environ[i], "OBS_HAIKU_GL=", strlen("OBS_HAIKU_GL=")) != 0)
			envp[e++] = environ[i];
	}
	envp[e++] = "OBS_HAIKU_GL=osmesa";
	envp[e] = NULL;

	/* the new team's main thread is created suspended */
	const thread_id shell = load_image(argc, argv, envp);
	status_t status = shell < 0 ? (status_t)shell : resume_thread(shell);
	bfree(envp);
	bfree(argv);

	if (status != B_OK) {
		blog(LOG_ERROR, "Haiku GL watchdog: unable to restart OBS (%s)", strerror(status));
		if (shell >= 0)
			kill_team(shell);
		return;
	}
	blog(LOG_ERROR, "Haiku GL watchdog: restarting %s with OBS_HAIKU_GL=osmesa, ending this instance", exe);

	/* Ends every thread of this team, the one stuck in the driver included (kill_team() wants a real team
	 * id, B_CURRENT_TEAM is rejected). The shell is a team of its own and is not affected. */
	kill_team(getpid());
	_exit(EXIT_FAILURE);
}

enum holder_state {
	HOLDER_BLOCKED, /* waiting for something or sleeping */
	HOLDER_ACTIVE,  /* running or ready to run */
	HOLDER_STOPPED, /* suspended (SIGSTOP, debugger) */
	HOLDER_GONE,    /* no such thread any more */
};

/* cpu_us: the CPU time the thread has used so far */
static enum holder_state holder_sample(thread_id thread, bigtime_t *cpu_us, char *name, size_t name_size)
{
	thread_info info;
	if (get_thread_info(thread, &info) != B_OK) {
		if (name)
			snprintf(name, name_size, "?");
		return HOLDER_GONE;
	}
	*cpu_us = info.user_time + info.kernel_time;
	if (name)
		snprintf(name, name_size, "%s", info.name);
	switch (info.state) {
	case B_THREAD_WAITING:
	case B_THREAD_ASLEEP:
	case B_THREAD_RECEIVING:
		return HOLDER_BLOCKED;
	case B_THREAD_SUSPENDED:
		return HOLDER_STOPPED;
	default:
		return HOLDER_ACTIVE;
	}
}

/* A debugger attached to the team: Debugger, strace, or the debug_server showing its crash dialog. If the
 * question cannot be answered, the answer is no. */
static bool team_is_debugged(void)
{
	team_info info;
	if (get_team_info(B_CURRENT_TEAM, &info) != B_OK)
		return false;
	return info.debugger_nub_thread >= 0;
}

static void watchdog_report(struct gl_platform *plat, uint64_t held_ns, thread_id holder, const char *holder_name,
			    bool *recorded)
{
	blog(LOG_ERROR,
	     "Haiku GL watchdog: the graphics driver stopped responding: thread %d (%s) has been blocked with the "
	     "OpenGL context for %.0f s",
	     (int)holder, holder_name, (double)held_ns / 1e9);
	blog(LOG_ERROR, "Haiku GL watchdog: driver: %s", plat->renderer);
	if (plat->identity.valid)
		log_identity(LOG_ERROR, "Haiku GL watchdog: self-test ", &plat->identity);
	log_identity(LOG_ERROR, "Haiku GL watchdog: process ", &plat->process_identity);
	*recorded = hang_marker_write(&plat->identity, &plat->process_identity, plat->renderer);
}

static struct gl_haiku_alert *watchdog_alert(struct gl_platform *plat, uint64_t held_ns, bool can_restart,
					     bool recorded)
{
	struct dstr text = {0};
	dstr_printf(&text,
		    "The graphics driver stopped responding.\n\n"
		    "OBS has been waiting %.0f seconds for the graphics driver (%s). It may still recover; this "
		    "message closes by itself if it does.\n\n",
		    (double)held_ns / 1e9, plat->renderer);
	if (can_restart)
		dstr_cat(&text, "OBS can restart with the CPU renderer, which is slower but does not depend on the "
				"driver. Restarting stops a recording or stream in progress, and the recording may "
				"be incomplete.");
	else
		dstr_cat(&text, "The CPU renderer (libOSMesa, HaikuPorts package \"mesa\") is not installed, so OBS "
				"cannot switch to it. Quitting stops a recording or stream in progress, and the "
				"recording may be incomplete.");
	if (recorded) {
		char *path = os_get_config_path_ptr(GL_HANG_MARKER);
		dstr_catf(&text,
			  "\n\nIf OBS is ended while the driver does not respond, it uses the CPU renderer from then "
			  "on, until the graphics driver is updated or the file %s is deleted.",
			  path ? path : GL_HANG_MARKER);
		bfree(path);
	}

	/* the harmless button is the default one */
	struct gl_haiku_alert *alert = gl_haiku_alert_show(
		"OBS Studio", text.array, can_restart ? "Restart with CPU renderer" : "Quit OBS", "Keep waiting");
	dstr_free(&text);
	return alert;
}

static void *watchdog_thread(void *data)
{
	struct gl_platform *plat = data;
	const uint64_t timeout_ns = (uint64_t)plat->watchdog_timeout_s * 1000000000ull;
	/* after "Keep waiting" the question is not repeated too soon */
	const uint64_t ask_again_ns = timeout_ns * 4 > 60000000000ull ? timeout_ns * 4 : 60000000000ull;

	/* the hold under observation (seen_seq == 0: none) */
	uint64_t seen_seq = 0;
	thread_id holder = -1;
	uint64_t held_ns = 0;   /* observed in total */
	uint64_t window_ns = 0; /* the current evaluation window: its length, samples and CPU time */
	unsigned samples = 0, blocked = 0;
	bigtime_t window_cpu = 0;
	bool noted = false;

	/* the hold that was declared a hang (hung_seq == 0: none) */
	uint64_t hung_seq = 0, ask_at = 0;
	bool recorded = false, can_restart = false;
	struct gl_haiku_alert *alert = NULL;

	os_set_thread_name("haiku-gl-watchdog");

	uint64_t last = os_gettime_ns();
	while (os_event_timedwait(plat->watchdog_event, GL_WATCHDOG_TICK_MS) == ETIMEDOUT) {
		const uint64_t now = os_gettime_ns();
		const uint64_t tick_ns = now > last ? now - last : 0;
		last = now;

		/* device_enter_context() writes holder and held_since before it increments hold_seq, so an
		 * unchanged hold_seq with held_since set means: still the hold seen before */
		const uint64_t seq = atomic_load(&plat->hold_seq);
		const bool held = atomic_load(&plat->held_since) != 0;
		const thread_id thread = (thread_id)atomic_load(&plat->holder);

		if (hung_seq) {
			if (!held || seq != hung_seq) {
				/* that hold ended after all: not a hang worth a CPU renderer next time */
				blog(LOG_WARNING,
				     "Haiku GL watchdog: the graphics driver responded again after %.0f s; keeping the "
				     "hardware renderer",
				     (double)held_ns / 1e9);
				if (alert)
					gl_haiku_alert_close(alert);
				alert = NULL;
				if (recorded && !plat->marker_preexisting)
					hang_marker_remove();
				hung_seq = 0;
				seen_seq = 0;
				recorded = false;
				continue;
			}
			held_ns += tick_ns;

			if (alert) {
				const int answer = gl_haiku_alert_poll(alert);
				if (answer == GL_HAIKU_ALERT_PENDING)
					continue;
				alert = NULL;
				if (answer == GL_HAIKU_ALERT_ACTION) {
					blog(LOG_ERROR, "Haiku GL watchdog: the user chose to %s",
					     can_restart ? "restart with the CPU renderer" : "quit");
					if (can_restart) {
						relaunch_obs();
					} else {
						kill_team(getpid());
						_exit(EXIT_FAILURE);
					}
					/* the restart could not be started (logged) */
				} else {
					blog(LOG_WARNING, "Haiku GL watchdog: still waiting for the graphics driver");
				}
				ask_at = now + ask_again_ns;
				continue;
			}
			if (now >= ask_at) {
				alert = watchdog_alert(plat, held_ns, can_restart, recorded);
				if (!alert) {
					blog(LOG_ERROR, "Haiku GL watchdog: no alert can be shown (no BApplication); OBS keeps "
							"waiting for the driver");
					ask_at = UINT64_MAX;
				}
			}
			continue;
		}

		if (!held || seq < GL_WATCHDOG_ARM_ENTERS || tick_ns > GL_WATCHDOG_MAX_TICK_NS || team_is_debugged()) {
			seen_seq = 0;
			continue;
		}

		bigtime_t cpu = 0;
		char name[B_OS_NAME_LENGTH];
		if (seq != seen_seq) {
			/* a hold not seen before: start observing it */
			seen_seq = seq;
			holder = thread;
			held_ns = window_ns = 0;
			samples = blocked = 0;
			noted = false;
			holder_sample(holder, &cpu, NULL, 0);
			window_cpu = cpu;
			continue;
		}

		const enum holder_state state = holder_sample(holder, &cpu, name, sizeof(name));
		if (state == HOLDER_STOPPED) {
			seen_seq = 0;
			continue;
		}
		held_ns += tick_ns;
		window_ns += tick_ns;
		samples++;
		if (state == HOLDER_BLOCKED)
			blocked++;
		if (window_ns < timeout_ns)
			continue;

		const uint64_t cpu_ns = cpu > window_cpu ? (uint64_t)(cpu - window_cpu) * 1000 : 0;
		const bool hung = state != HOLDER_GONE && blocked * 10 >= samples * 9 && cpu_ns * 4 <= window_ns;
		if (!hung) {
			if (!noted) {
				if (state == HOLDER_GONE)
					blog(LOG_ERROR,
					     "Haiku GL watchdog: thread %d ended without releasing the OpenGL context; "
					     "this is not a driver hang",
					     (int)holder);
				else
					blog(LOG_WARNING,
					     "Haiku GL watchdog: thread %d (%s) has held the OpenGL context for %.0f s "
					     "but is busy (blocked in %u of %u samples, %.1f s of CPU time); not treated "
					     "as a driver hang",
					     (int)holder, name, (double)held_ns / 1e9, blocked, samples,
					     (double)cpu_ns / 1e9);
				noted = true;
			}
			/* judge the next window by itself, so a thread that hangs after being busy is noticed */
			window_ns = 0;
			samples = blocked = 0;
			window_cpu = cpu;
			continue;
		}

		hung_seq = seq;
		watchdog_report(plat, held_ns, holder, name, &recorded);
		can_restart = osmesa_installed();
		if (!can_restart)
			blog(LOG_ERROR, "Haiku GL watchdog: libOSMesa is not installed, no CPU renderer to restart with");
		alert = watchdog_alert(plat, held_ns, can_restart, recorded);
		if (!alert) {
			blog(LOG_ERROR, "Haiku GL watchdog: no alert can be shown (no BApplication); OBS keeps waiting "
					"for the driver");
			ask_at = UINT64_MAX;
		}
	}

	if (alert)
		gl_haiku_alert_close(alert);
	return NULL;
}

/* OBS_HAIKU_GL_WATCHDOG=<seconds> (0 disables) */
static void watchdog_start(struct gl_platform *plat)
{
	plat->watchdog_timeout_s = GL_WATCHDOG_DEFAULT_TIMEOUT_S;
	const char *env = getenv("OBS_HAIKU_GL_WATCHDOG");
	if (env && *env) {
		char *end = NULL;
		long value = strtol(env, &end, 10);
		if (end != env && *end == '\0' && value >= 0 && value <= 3600)
			plat->watchdog_timeout_s = (int)value;
		else
			blog(LOG_WARNING, "Haiku GL: ignoring invalid OBS_HAIKU_GL_WATCHDOG '%s'", env);
	}
	if (plat->watchdog_timeout_s == 0) {
		blog(LOG_INFO, "Haiku GL: driver hang watchdog disabled (OBS_HAIKU_GL_WATCHDOG=0)");
		return;
	}
	if (plat->watchdog_timeout_s < GL_WATCHDOG_MIN_TIMEOUT_S)
		plat->watchdog_timeout_s = GL_WATCHDOG_MIN_TIMEOUT_S;

	if (os_event_init(&plat->watchdog_event, OS_EVENT_TYPE_MANUAL) != 0)
		return;
	if (pthread_create(&plat->watchdog, NULL, watchdog_thread, plat) != 0) {
		os_event_destroy(plat->watchdog_event);
		plat->watchdog_event = NULL;
		blog(LOG_WARNING, "Haiku GL: unable to start the driver hang watchdog");
		return;
	}
	plat->watchdog_running = true;
	blog(LOG_INFO, "Haiku GL: driver hang watchdog: %d s (OBS_HAIKU_GL_WATCHDOG=<seconds>, 0 disables it)",
	     plat->watchdog_timeout_s);
}

static void watchdog_stop(struct gl_platform *plat)
{
	if (!plat->watchdog_running)
		return;
	os_event_signal(plat->watchdog_event);
	pthread_join(plat->watchdog, NULL);
	os_event_destroy(plat->watchdog_event);
	plat->watchdog_event = NULL;
	plat->watchdog_running = false;
}

/* ---------------------------------------------------------------------------------------------------------- */

static bool egl_create_and_load(gs_device_t *device, bool fallback_available)
{
	struct gl_platform *plat = device->plat;

	if (!egl_context_create(plat)) {
		if (fallback_available)
			blog(LOG_INFO, "Haiku GL: no usable EGL (normal with HaikuPorts' Mesa 22), using OSMesa");
		return false;
	}
	if (load_gl(device))
		return true;

	/* a context that cannot be made current or is below 3.3 is as good as none */
	egl_context_destroy(plat);
	if (fallback_available)
		blog(LOG_INFO, "Haiku GL: EGL context unusable, falling back to OSMesa");
	return false;
}

static bool osmesa_create_and_load(gs_device_t *device)
{
	struct gl_platform *plat = device->plat;

	if (!osmesa_context_create(plat))
		return false;
	if (load_gl(device))
		return true;

	osmesa_context_destroy(plat);
	plat->use_osmesa = false;
	return false;
}

/* Without OSMesa (Mesa 25 has none) a driver that should be avoided is the only chance left. */
static bool osmesa_instead_of_egl(gs_device_t *device, const char *why)
{
	if (osmesa_create_and_load(device))
		return true;
	blog(LOG_WARNING, "Haiku GL: OSMesa is not available; using the hardware driver that %s", why);
	return egl_create_and_load(device, false);
}

extern struct gl_platform *gl_platform_create(gs_device_t *device, uint32_t adapter)
{
	UNUSED_PARAMETER(adapter);

	struct gl_platform *plat = bzalloc(sizeof(struct gl_platform));
	plat->display = EGL_NO_DISPLAY;
	plat->context = EGL_NO_CONTEXT;
	device->plat = plat;

	const char *forced = getenv("OBS_HAIKU_GL");
	const bool want_egl = !forced || strcmp(forced, "osmesa") != 0;
	const bool want_osmesa = !forced || strcmp(forced, "egl") != 0;

	/* Nothing forced: test the EGL driver in a separate process first (see run_gl_probe()). */
	enum gl_probe_result probe = GL_PROBE_UNAVAILABLE;
	struct gl_identity *probe_id = bzalloc(sizeof(struct gl_identity));
	if (want_egl && want_osmesa)
		probe = run_gl_probe(probe_id);
	if (probe != GL_PROBE_PASSED)
		probe_id->valid = false;

	/*
	 * The hang record (see watchdog_thread()) of a driver that hung in an earlier session. Not consulted when a
	 * backend is forced: OBS_HAIKU_GL=egl uses the driver regardless (and keeps the record), the relaunch after
	 * a hang forces osmesa. Identities are compared like with like (see hang_marker_read()): the self-test's
	 * with the self-test's, else the one computed in this process with the one computed in OBS back then. A
	 * match selects the CPU renderer; a mismatch means the driver stack changed, and removes the record.
	 */
	struct hang_marker marker = {0};
	const bool have_marker = want_egl && (probe == GL_PROBE_PASSED || probe == GL_PROBE_UNAVAILABLE) &&
				 hang_marker_read(&marker);
	char *marker_path = have_marker ? os_get_config_path_ptr(GL_HANG_MARKER) : NULL;
	bool marker_match = false, marker_check_late = false;
	if (have_marker && !want_osmesa) {
		blog(LOG_WARNING,
		     "Haiku GL: the hardware driver hung in a previous session (%s); using it anyway "
		     "(OBS_HAIKU_GL=egl)",
		     marker_path);
		plat->marker_preexisting = true;
	} else if (have_marker && marker.have_probe && probe_id->valid) {
		marker_match = probe_id->hash == marker.probe_hash;
		if (!marker_match) {
			blog(LOG_INFO,
			     "Haiku GL: the driver changed since it hung in a previous session (identity %016" PRIx64
			     ", was %016" PRIx64 "); trying the hardware driver again",
			     probe_id->hash, marker.probe_hash);
			hang_marker_remove();
		}
	} else if (have_marker && marker.have_process) {
		marker_check_late = true;
	} else if (have_marker) {
		/* only a self-test identity, and no self-test result to compare it with */
		marker_match = true;
		blog(LOG_WARNING, "Haiku GL: the driver self-test did not run, so the hang record %s cannot be checked "
				  "against the installed driver; honouring it",
				  marker_path);
	}

	struct dstr hung_warning = {0};
	dstr_printf(&hung_warning,
		    "Haiku GL: the hardware driver hung in a previous session; using the CPU renderer. Update the "
		    "driver or delete %s to use it again (OBS_HAIKU_GL=egl uses it for one session).",
		    marker_path ? marker_path : GL_HANG_MARKER);
	bool created = false;
	if (probe == GL_PROBE_FAILED) {
		blog(LOG_WARNING, "Haiku GL: using the CPU renderer (OSMesa) instead of the hardware OpenGL driver. "
				  "Start OBS with OBS_HAIKU_GL=egl to use the driver anyway.");
		created = osmesa_instead_of_egl(device, "failed the self-test");
	} else if (marker_match) {
		blog(LOG_WARNING, "%s", hung_warning.array);
		created = osmesa_instead_of_egl(device, "hung before");
		if (created && !plat->use_osmesa)
			plat->marker_preexisting = true;
	} else {
		if (want_egl) {
			created = egl_create_and_load(device, want_osmesa);
			if (created && probe == GL_PROBE_NO_EGL)
				blog(LOG_WARNING, "Haiku GL: the self-test found no EGL, but OBS did; the driver is "
						  "used untested");
			if (created && marker_check_late) {
				identity_compute(&plat->process_identity);
				if (plat->process_identity.hash == marker.process_hash) {
					blog(LOG_WARNING, "%s", hung_warning.array);
					if (osmesa_installed()) {
						egl_context_destroy(plat);
						created = osmesa_instead_of_egl(device, "hung before");
					} else {
						blog(LOG_WARNING, "Haiku GL: OSMesa is not available; using the hardware "
								  "driver that hung before");
					}
					if (created && !plat->use_osmesa)
						plat->marker_preexisting = true;
				} else {
					blog(LOG_INFO,
					     "Haiku GL: the driver changed since it hung in a previous session (identity "
					     "%016" PRIx64 ", was %016" PRIx64 "); trying the hardware driver again",
					     plat->process_identity.hash, marker.process_hash);
					hang_marker_remove();
				}
			}
		}
		if (!created && want_osmesa)
			created = osmesa_create_and_load(device);
	}
	dstr_free(&hung_warning);
	bfree(marker_path);

	if (!created) {
		blog(LOG_ERROR, "Haiku GL: no OpenGL backend available. Install Mesa: 'pkgman install mesa'");
		bfree(probe_id);
		bfree(plat);
		device->plat = NULL;
		return NULL;
	}

	const char *renderer = (const char *)glGetString(GL_RENDERER);
	snprintf(plat->renderer, sizeof(plat->renderer), "%s", renderer ? renderer : "?");
	blog(LOG_INFO, "Haiku GL: rendering through %s", plat->use_osmesa ? "OSMesa" : "EGL (surfaceless)");
	blog(LOG_INFO, "Haiku GL: %s", plat->renderer);

	if (!plat->use_osmesa) {
		/* both identities go into a hang record: the next launch compares like with like */
		if (probe_id->valid)
			plat->identity = *probe_id;
		if (!plat->process_identity.valid)
			identity_compute(&plat->process_identity);
		if (plat->identity.valid)
			blog(LOG_INFO, "Haiku GL: driver identity %016" PRIx64 " (self-test), %016" PRIx64 " (process)",
			     plat->identity.hash, plat->process_identity.hash);
		else
			blog(LOG_INFO, "Haiku GL: driver identity %016" PRIx64 " (process; no self-test)",
			     plat->process_identity.hash);
		log_identity(LOG_DEBUG, "Haiku GL: process ", &plat->process_identity);
		watchdog_start(plat);
	}
	bfree(probe_id);
	return plat;
}

extern void gl_platform_destroy(struct gl_platform *plat)
{
	if (!plat)
		return;

	watchdog_stop(plat);

	if (plat->use_osmesa)
		osmesa_context_destroy(plat);
	else
		egl_context_destroy(plat);
	bfree(plat);
}

extern bool gl_platform_init_swapchain(struct gs_swap_chain *swap)
{
	struct gl_windowinfo *wi = swap->wi;

	if (swap->device->plat->use_osmesa) {
		wi->osmesa_buffer = bzalloc((size_t)wi->cx * wi->cy * 4);
		return true;
	}
	/* the context is current while swapchains are created */
	return egl_swapchain_fbo(wi);
}

extern void gl_platform_cleanup_swapchain(struct gs_swap_chain *swap)
{
	struct gl_windowinfo *wi = swap->wi;
	gs_device_t *device = swap->device;

	if (!wi)
		return;

	if (device->cur_swap == swap) {
		/* switch away before the buffer goes */
		device->cur_swap = NULL;
		make_current(device);
		bind_swapchain_framebuffer(device);
	}

	if (device->plat->use_osmesa) {
		bfree(wi->osmesa_buffer);
		wi->osmesa_buffer = NULL;
	} else {
		egl_swapchain_free(wi);
	}
}

extern void device_enter_context(gs_device_t *device)
{
	struct gl_platform *plat = device->plat;

	/* heartbeat for the watchdog: eglMakeCurrent() itself may wait for the GPU, so this is set before it.
	 * libobs calls this once per outermost gs_enter_context(), with its graphics mutex held. */
	atomic_store(&plat->holder, (int)find_thread(NULL));
	atomic_store(&plat->held_since, os_gettime_ns());
	atomic_fetch_add(&plat->hold_seq, 1);
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
	atomic_store(&device->plat->held_since, 0);
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

/* Called after device_resize(): resize the current swapchain's buffer. */
extern void gl_update(gs_device_t *device)
{
	struct gs_swap_chain *swap = device->cur_swap;
	if (!swap)
		return;

	struct gl_windowinfo *wi = swap->wi;
	const uint32_t cx = swap->info.cx ? swap->info.cx : 1;
	const uint32_t cy = swap->info.cy ? swap->info.cy : 1;

	if (wi->cx == cx && wi->cy == cy)
		return;

	const uint32_t old_cx = wi->cx;
	const uint32_t old_cy = wi->cy;
	wi->cx = cx;
	wi->cy = cy;

	if (!device->plat->use_osmesa) {
		/* the framebuffer object is resized on next use; rebind if it is the current target */
		bind_swapchain_framebuffer(device);
		return;
	}

	/* OSMesa: the old buffer is current; bind the new one before freeing the old one, and keep the old one
	 * (and its size) if the new one cannot be bound. */
	uint8_t *old = wi->osmesa_buffer;
	wi->osmesa_buffer = bzalloc((size_t)cx * cy * 4);
	if (!make_current(device)) {
		bfree(wi->osmesa_buffer);
		wi->osmesa_buffer = old;
		wi->cx = old_cx;
		wi->cy = old_cy;
		make_current(device);
		return;
	}
	bfree(old);
}

extern void device_load_swapchain(gs_device_t *device, gs_swapchain_t *swap)
{
	if (device->cur_swap == swap)
		return;

	device->cur_swap = swap;
	if (device->plat->use_osmesa)
		make_current(device);
	else
		bind_swapchain_framebuffer(device);
}

extern bool device_is_present_ready(gs_device_t *device)
{
	UNUSED_PARAMETER(device);
	return true;
}

extern void device_present(gs_device_t *device)
{
	struct gs_swap_chain *swap = device->cur_swap;
	if (!swap || !swap->wi)
		return;

	struct gl_windowinfo *wi = swap->wi;
	const bool osmesa_backend = device->plat->use_osmesa;
	if (osmesa_backend ? !wi->osmesa_buffer : !egl_swapchain_fbo(wi))
		return;

	const uint32_t cx = wi->cx;
	const uint32_t cy = wi->cy;
	const uint32_t linesize = cx * 4;
	const size_t size = (size_t)linesize * cy;

	if (wi->readback_size < size) {
		bfree(wi->readback);
		wi->readback = bmalloc(size);
		wi->readback_size = size;
	}

	/* No glGetIntegerv round trips for the previous bindings: nothing in the GL subsystem relies on them
	 * persisting. Everything that reads binds its own GL_READ_FRAMEBUFFER (saving and restoring around it),
	 * and pixel pack buffers are only bound during a stage surface operation and reset to 0 after it. So
	 * both are simply left at 0 here. */
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	if (osmesa_backend) {
		/* OSMesa's default framebuffer is single buffered */
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		glReadBuffer(GL_FRONT);
	} else {
		glBindFramebuffer(GL_READ_FRAMEBUFFER, wi->fbo);
		glReadBuffer(GL_COLOR_ATTACHMENT0);
	}

	glReadPixels(0, 0, (GLsizei)cx, (GLsizei)cy, GL_BGRA, GL_UNSIGNED_BYTE, wi->readback);

	if (!osmesa_backend)
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

	if (!gl_success("glReadPixels"))
		return;

	/* GL rows are bottom-up. Instead of flipping them here, pass the top row (last in memory) with a
	 * negative stride; the consumer flips while copying into its own image. */
	const int32_t stride = -(int32_t)linesize;
	wi->present(wi->param, wi->readback + (size_t)(cy - 1) * linesize, cx, cy, stride);
}

extern bool device_is_monitor_hdr(gs_device_t *device, void *monitor)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(monitor);
	return false;
}
