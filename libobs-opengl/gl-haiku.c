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
 * Readback costs one glReadPixels per displayed frame, which is acceptable for preview/projector sizes and
 * keeps the renderer independent of any toolkit. The rows are handed over bottom-up as GL returns them, with
 * the pointer at the top row and a negative stride, so the consumer's copy into its own image does the vertical
 * flip.
 */

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <obs.h>
#include <util/bmem.h>
#include <util/platform.h>

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

struct gl_platform {
	bool use_osmesa;

	/* EGL backend */
	EGLDisplay display;
	EGLConfig config;
	EGLContext context;

	/* OSMesa backend */
	OSMesaContext osmesa_ctx;
	uint32_t osmesa_dummy; /* 1x1 buffer used when no swapchain is loaded */
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
		return "a GPU fence never signalled";
	default:
		return "unexpected exit status";
	}
}

struct gl_probe_output {
	char line[256];
	size_t len;
	unsigned lines;
};

static void gl_probe_flush_line(struct gl_probe_output *out)
{
	if (!out->len)
		return;
	out->line[out->len] = '\0';
	if (out->lines < GL_PROBE_MAX_LOG_LINES)
		blog(LOG_INFO, "Haiku GL probe: %s", out->line);
	else if (out->lines == GL_PROBE_MAX_LOG_LINES)
		blog(LOG_INFO, "Haiku GL probe: (further output dropped)");
	out->lines++;
	out->len = 0;
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
 * Runs obs-haiku-gl-probe, which drives the EGL driver the way OBS does (surfaceless context, one context
 * handed between threads, FBO rendering, fences, readback) for 120 frames and checks the pixels. A driver
 * that hangs (as a young NVK/Zink once did on its first frames) would otherwise freeze OBS's graphics thread
 * and with it the UI; in a child process it costs the timeout and a fallback to OSMesa. The probe runs on
 * every launch, so a driver update is always re-tested; on a healthy driver it costs one EGL initialisation
 * and a few milliseconds of rendering. Its output goes into the OBS log.
 */
static enum gl_probe_result run_gl_probe(void)
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

	char timeout_arg[16];
	snprintf(timeout_arg, sizeof(timeout_arg), "%u", (unsigned)(timeout + 0.999));
	char *argv[] = {path, "--timeout", timeout_arg, NULL};
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
	if (want_egl && want_osmesa)
		probe = run_gl_probe();

	bool created = false;
	if (probe == GL_PROBE_FAILED) {
		blog(LOG_WARNING, "Haiku GL: using the CPU renderer (OSMesa) instead of the hardware OpenGL driver. "
				  "Start OBS with OBS_HAIKU_GL=egl to use the driver anyway.");
		created = osmesa_create_and_load(device);
		if (!created) {
			/* without OSMesa (Mesa 25 has none) the failed driver is the only chance left */
			blog(LOG_WARNING, "Haiku GL: OSMesa is not available; trying the driver that failed the "
					  "self-test");
			created = egl_create_and_load(device, false);
		}
	} else {
		if (want_egl) {
			created = egl_create_and_load(device, want_osmesa);
			if (created && probe == GL_PROBE_NO_EGL)
				blog(LOG_WARNING, "Haiku GL: the self-test found no EGL, but OBS did; the driver is "
						  "used untested");
		}
		if (!created && want_osmesa)
			created = osmesa_create_and_load(device);
	}

	if (!created) {
		blog(LOG_ERROR, "Haiku GL: no OpenGL backend available. Install Mesa: 'pkgman install mesa'");
		bfree(plat);
		device->plat = NULL;
		return NULL;
	}

	blog(LOG_INFO, "Haiku GL: rendering through %s", plat->use_osmesa ? "OSMesa" : "EGL (surfaceless)");
	blog(LOG_INFO, "Haiku GL: %s", (const char *)glGetString(GL_RENDERER));
	return plat;
}

extern void gl_platform_destroy(struct gl_platform *plat)
{
	if (!plat)
		return;

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
