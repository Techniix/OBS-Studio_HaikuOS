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
 * obs-haiku-gl-probe: self-test of the EGL OpenGL driver, run by libobs-opengl (gl-haiku.c) in a separate
 * process before OBS commits to EGL. A driver that hangs, crashes or renders garbage under OBS's usage
 * pattern then costs a fallback to the CPU renderer (OSMesa) instead of a frozen OBS. It is also meant as a
 * reproduction tool for driver developers: the options below switch the parts of the pattern on and off, and
 * --verbose prints every step before it runs, so the last line of a hung run names the call that hung.
 *
 * It does what libobs-opengl does on Haiku with a Screen Capture source in the scene, through the same glad
 * loaders and the same GL calls, frame by frame as obs_graphics_thread_loop() in libobs/obs-video.c does:
 *  - EGL_DEFAULT_DISPLAY, EGL_KHR_surfaceless_context, eglBindAPI(EGL_OPENGL_API), the config and the
 *    OpenGL 3.3 core context attributes of gl-haiku.c; the context is made current without a surface. One
 *    context is handed between threads under a mutex, as libobs's graphics mutex does, and every release is
 *    eglMakeCurrent(NO_SURFACE, NO_SURFACE, NO_CONTEXT), like device_leave_context();
 *  - per frame, on the render thread (OBS's graphics thread), three holds of the context:
 *    1. gs_begin_frame(): enter and leave;
 *    2. output_frame(): the capture's new frame (every other frame: a 30 fps capture in a 60 fps canvas) is
 *       uploaded as gs_texture_set_image() does (gl-texture2d.c): a GS_BGRX dynamic texture (GL_SRGB8 from
 *       GL_BGRA) with its own GL_PIXEL_UNPACK_BUFFER (glBufferData(NULL, GL_DYNAMIC_DRAW), never orphaned),
 *       glMapBuffer(GL_WRITE_ONLY) while the previous upload from it may still be in flight, the buffer
 *       unbound while the frame is written, glUnmapBuffer and glTexImage2D from offset 0. Then the capture is
 *       drawn into the canvas, a GS_RGBA render target texture (GL_SRGB8_ALPHA8) attached with
 *       glFramebufferTexture2D. With --stage (the default; a recording or stream in progress), the canvas is
 *       converted into NV12 planes (GS_R8 and GS_R8G8 render targets), each read into a pixel pack buffer
 *       with glGetTexImage (device_stage_texture()), and the previous frame's buffers are mapped with
 *       glMapBuffer(GL_READ_ONLY) (download_frame()), read outside the context and unmapped in the next
 *       frame. The hold ends with glFlush() (gs_flush()). No fence is waited for: libobs never waits on one,
 *       its synchronisation is implicit in glMapBuffer and glReadPixels, which is where a driver hangs;
 *    3. render_displays(): the main preview (1280x720) draws the canvas and the selection box of the source
 *       (through libobs's immediate vertex buffer: glMapBufferRange(GL_MAP_INVALIDATE_BUFFER_BIT) per edge),
 *       the properties dialog's preview draws the capture itself, each into its swapchain framebuffer object (a
 *       GL_RGBA8 renderbuffer, resized with glRenderbufferStorage as egl_swapchain_fbo() does), and both are
 *       read back with glReadPixels(GL_BGRA) into client memory (device_present());
 *    then it sleeps until the next frame is due (--fps), as video_sleep() does;
 *  - the capture's texture is created in a hold of its own before the first frame and recreated once mid-run
 *    (set_async_texture_size(): a size change), deleting the old one while the GPU may still use it;
 *  - a second thread (OBS's UI thread) takes the context now and then: it creates, fills and deletes a small
 *    texture, and opens the properties dialog's display (gs_swapchain_create()), closes it half way through
 *    and opens it again;
 *  - every readback is checked: the colour of every block of the captured frame in both previews and in both
 *    NV12 planes, so a stale or partial upload or readback is caught too. A final fence must signal within 2 s.
 *
 * Output: "IDENTITY <hash>" and "IDENTITY-PART <part>" lines with the driver identity (gl-identity.h, used
 * by gl-haiku.c to recognise a driver that hung before), then one line on success ("OK: EGL ..."); diagnostics
 * on stderr. A step that does not return within --stall seconds ends the probe with exit status 5 and a
 * "STALL:" report whose last line is the step that has been stuck longest. The process exits with _exit()
 * everywhere, so a hung driver thread can never keep it alive, and it leaves no files behind. alarm() ends it
 * should the parent die before it could kill a hung probe.
 *
 * Exit status: 0 pass, 1 usage, 2 no EGL display, 3 no usable OpenGL 3.3 core context, 4 wrong pixels or a GL
 * error, 5 a fence or a driver call did not return.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <glad/glad.h>
#include <glad/glad_egl.h>

#include "gl-identity.h"

enum probe_exit {
	PROBE_OK = 0,
	PROBE_USAGE = 1,
	PROBE_NO_EGL = 2,     /* no EGL display / eglInitialize failed */
	PROBE_NO_CONTEXT = 3, /* EGL works, but not an OpenGL 3.3 core surfaceless context with what OBS needs */
	PROBE_BAD_PIXELS = 4, /* wrong pixels read back or a GL error */
	PROBE_STALL = 5,      /* a fence did not signal in time, or a driver call did not return */
};

/* keep in sync with gl-haiku.c */
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

#define FENCE_TIMEOUT_NS 2000000000ull

/* the captured frame: a frame-dependent colour per block */
#define CAP_BX 8
#define CAP_BY 4

/* the main preview, and the properties dialog's preview that gets resized */
#define DISPLAY_A_W 1280
#define DISPLAY_A_H 720
static const int display_b_sizes[][2] = {{640, 360}, {800, 450}, {480, 270}, {720, 405}};
#define DISPLAY_B_SIZES (sizeof(display_b_sizes) / sizeof(display_b_sizes[0]))
#define DISPLAY_B_RESIZE_EVERY 8

/* options */
static unsigned opt_frames = 60;
static unsigned opt_fps = 60;
static unsigned opt_stall_s = 5;
static int cap_w = 2560, cap_h = 1080;       /* --size: the captured screen */
static int canvas_w = 1920, canvas_h = 1080; /* --canvas: OBS's base resolution */
static bool opt_stream = true, opt_second = true, opt_handoff = true, opt_stage = true;
static bool opt_fences = false, opt_verbose = false;

static EGLDisplay display = EGL_NO_DISPLAY;
static EGLContext context = EGL_NO_CONTEXT;

/* models libobs's graphics mutex; whoever holds it has the context current */
static pthread_mutex_t ctx_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---------------------------------------------------------------------------------------------------------- */
/* Steps and the stall monitor                                                                                */

/* what each thread is doing right now; NULL: nothing that can hang */
struct step_slot {
	const char *name;
	_Atomic(const char *) what;
	atomic_bool waiting; /* only waiting for the other thread to release the context */
	atomic_uint frame;
	_Atomic uint64_t since;
};
enum { SLOT_RENDER, SLOT_UI, SLOT_COUNT };
static struct step_slot slots[SLOT_COUNT] = {{.name = "render"}, {.name = "ui"}};

static pthread_mutex_t print_lock = PTHREAD_MUTEX_INITIALIZER;

_Noreturn static void quit(int code)
{
	fflush(stdout);
	fflush(stderr);
	_exit(code);
}

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Marks the start of a step that could hang (what != NULL) or the end of it (NULL). --verbose prints every
 * step before it runs. */
static void step_ex(int slot, unsigned frame, const char *what, bool waiting)
{
	struct step_slot *s = &slots[slot];
	atomic_store(&s->waiting, waiting);
	atomic_store(&s->frame, frame);
	atomic_store(&s->since, now_ns());
	atomic_store(&s->what, what);
	if (what && opt_verbose) {
		pthread_mutex_lock(&print_lock);
		printf("frame %u [%s]: %s\n", frame, s->name, what);
		fflush(stdout);
		pthread_mutex_unlock(&print_lock);
	}
}

static void step(int slot, unsigned frame, const char *what)
{
	step_ex(slot, frame, what, false);
}

static void *stall_monitor(void *arg)
{
	(void)arg;
	const uint64_t limit = (uint64_t)opt_stall_s * 1000000000ull;
	for (;;) {
		usleep(100000);
		const uint64_t now = now_ns();
		int order[SLOT_COUNT];
		uint64_t age[SLOT_COUNT];
		unsigned frame[SLOT_COUNT];
		const char *what[SLOT_COUNT];
		bool waiting[SLOT_COUNT];
		int n = 0;
		bool stalled = false;
		for (int i = 0; i < SLOT_COUNT; i++) {
			what[i] = atomic_load(&slots[i].what);
			if (!what[i])
				continue;
			const uint64_t since = atomic_load(&slots[i].since);
			frame[i] = atomic_load(&slots[i].frame);
			waiting[i] = atomic_load(&slots[i].waiting);
			age[i] = now > since ? now - since : 0;
			if (age[i] > limit)
				stalled = true;
			order[n++] = i;
		}
		if (!stalled)
			continue;
		/* Threads that only wait for the context first, then the others youngest first: the last line is
		 * the step stuck longest that holds the context, normally the call inside the driver. */
		for (int a = 0; a < n; a++) {
			for (int b = a + 1; b < n; b++) {
				const int oa = order[a], ob = order[b];
				const bool before = waiting[ob] != waiting[oa] ? waiting[ob] : age[ob] < age[oa];
				if (before) {
					int t = order[a];
					order[a] = order[b];
					order[b] = t;
				}
			}
		}
		pthread_mutex_lock(&print_lock);
		fflush(stdout);
		for (int k = 0; k < n; k++) {
			const int i = order[k];
			fprintf(stderr, "STALL: frame %u [%s]: %s has not returned after %.1f s\n", frame[i],
				slots[i].name, what[i], (double)age[i] / 1e9);
		}
		quit(PROBE_STALL);
	}
	return NULL;
}

/* ---------------------------------------------------------------------------------------------------------- */
/* Helpers                                                                                                    */

static const char *egl_error(void)
{
	static char buf[16];
	snprintf(buf, sizeof(buf), "0x%x", (unsigned)eglGetError());
	return buf;
}

static bool has_extension(const char *extensions, const char *name)
{
	if (!extensions)
		return false;
	size_t len = strlen(name);
	for (const char *pos = extensions; (pos = strstr(pos, name)) != NULL; pos += len) {
		if ((pos == extensions || pos[-1] == ' ') && (pos[len] == ' ' || pos[len] == '\0'))
			return true;
	}
	return false;
}

/* gs_enter_context(): the graphics mutex, then device_enter_context() */
static void take_context(int slot, unsigned frame)
{
	step_ex(slot, frame, "waiting for the context (held by the other thread)", true);
	pthread_mutex_lock(&ctx_lock);
	step(slot, frame, "eglMakeCurrent(context)");
	if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) {
		fprintf(stderr, "eglMakeCurrent failed: %s\n", egl_error());
		quit(PROBE_NO_CONTEXT);
	}
}

/* gs_leave_context() */
static void release_context(int slot, unsigned frame)
{
	step(slot, frame, "eglMakeCurrent(EGL_NO_CONTEXT)");
	if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT)) {
		fprintf(stderr, "eglMakeCurrent(EGL_NO_CONTEXT) failed: %s\n", egl_error());
		quit(PROBE_NO_CONTEXT);
	}
	pthread_mutex_unlock(&ctx_lock);
	step(slot, frame, NULL);
}

static void check_gl(unsigned frame, const char *what, int code)
{
	GLenum err = glGetError();
	if (err != GL_NO_ERROR) {
		fprintf(stderr, "frame %u: GL error 0x%x during %s\n", frame, err, what);
		quit(code);
	}
}

static void wait_fence(int slot, unsigned frame, const char *what)
{
	step(slot, frame, what);
	GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	if (!sync) {
		fprintf(stderr, "frame %u: glFenceSync (%s) failed\n", frame, what);
		quit(PROBE_BAD_PIXELS);
	}
	GLenum wait = glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, FENCE_TIMEOUT_NS);
	glDeleteSync(sync);
	if (wait == GL_TIMEOUT_EXPIRED || wait == GL_WAIT_FAILED) {
		fprintf(stderr, "frame %u: %s: %s\n", frame, what,
			wait == GL_WAIT_FAILED ? "wait failed" : "not signalled after 2 s");
		quit(PROBE_STALL);
	}
}

static bool close_enough(const uint8_t *px, const uint8_t *want, int channels)
{
	for (int i = 0; i < channels; i++) {
		int d = (int)px[i] - (int)want[i];
		if (d < -1 || d > 1)
			return false;
	}
	return true;
}

/* BGRX colour of block (bx, by) in captured frame n; changes every frame in every channel but X */
static void cap_color(unsigned n, unsigned bx, unsigned by, uint8_t bgra[4])
{
	bgra[0] = (uint8_t)(bx * 29 + by * 7 + n * 11 + 3);
	bgra[1] = (uint8_t)(by * 53 + bx * 3 + n * 37 + 0x20);
	bgra[2] = (uint8_t)(bx * 17 + by * 89 + n * 5 + 0x60);
	bgra[3] = 0xff;
}

/* ---------------------------------------------------------------------------------------------------------- */
/* GL objects                                                                                                 */

static GLuint prog_draw, prog_y, prog_uv, prog_solid, vao_full, vbo_full;
/* libobs's immediate vertex buffer (gs_render_start()/gs_render_stop(), GS_DYNAMIC: GL_STREAM_DRAW) */
static GLuint vao_imm, vbo_imm;
#define IMM_VERTICES 4
static bool srgb_decode; /* EXT_texture_sRGB_decode: sRGB formats as libobs uses them, sampled undecoded */

/* a 2D texture as device_texture_create() makes it; with an unpack buffer if dynamic */
struct texture {
	GLuint tex, unpack, fbo;
	GLint internal_format;
	GLenum format;
	int cx, cy;
};

/* the capture's texture (guarded by ctx_lock) and the frame number whose pattern is in it */
static struct texture cap_tex;
static unsigned cap_frame_in_tex = (unsigned)-1;
static unsigned cap_uploads, cap_recreations;

/* the canvas (GS_RGBA render target) and the NV12 conversion targets */
static struct texture canvas, conv_y, conv_uv;

/* stage surfaces, two sets as libobs's NUM_TEXTURES ring */
struct stage_set {
	GLuint y, uv;      /* pixel pack buffers */
	bool staged;       /* holds frame "shown" */
	unsigned shown;    /* captured frame number in it */
	const uint8_t *py, *puv;
};
static struct stage_set stage[2];
static struct stage_set *mapped_set; /* mapped by download_frame(), unmapped in the next frame */
static size_t y_stride, uv_stride;
static unsigned stage_reads;

/* a swapchain: framebuffer object + colour renderbuffer, like struct gl_windowinfo in gl-haiku.c */
struct swapchain {
	const char *name;
	GLuint fbo, rb;
	int cx, cy;
	uint8_t *readback;
	size_t readback_size;
};
static struct swapchain swapchains[2] = {{.name = "main preview"}, {.name = "properties preview"}};
static unsigned display_b_size;    /* guarded by ctx_lock */
static unsigned swapchain_resizes; /* render thread only */
static unsigned handoffs;          /* ui thread only */

static GLuint compile_shader(GLenum type, const char *src)
{
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &src, NULL);
	glCompileShader(shader);
	GLint ok = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512] = "";
		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		fprintf(stderr, "shader compilation failed: %s\n", log);
		quit(PROBE_NO_CONTEXT);
	}
	return shader;
}

static GLuint make_program(const char *fs)
{
	static const char *vs = "#version 330 core\n"
				"layout(location = 0) in vec2 pos;\n"
				"layout(location = 1) in vec2 in_uv;\n"
				"out vec2 uv;\n"
				"void main() { uv = in_uv; gl_Position = vec4(pos, 0.0, 1.0); }\n";
	GLuint prog = glCreateProgram();
	GLuint v = compile_shader(GL_VERTEX_SHADER, vs);
	GLuint f = compile_shader(GL_FRAGMENT_SHADER, fs);
	glAttachShader(prog, v);
	glAttachShader(prog, f);
	glLinkProgram(prog);
	glDeleteShader(v);
	glDeleteShader(f);
	GLint ok = 0;
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		fprintf(stderr, "program link failed\n");
		quit(PROBE_NO_CONTEXT);
	}
	glUseProgram(prog);
	glUniform1i(glGetUniformLocation(prog, "image"), 0);
	glUseProgram(0);
	return prog;
}

/* device_texture_create(cx, cy, format, 1, NULL, flags): glTexImage2D without data, GL_TEXTURE_MAX_LEVEL 0 */
static void texture_create(struct texture *t, int cx, int cy, GLint internal_format, GLenum format, int bpp,
			   bool dynamic, bool render_target)
{
	memset(t, 0, sizeof(*t));
	t->cx = cx;
	t->cy = cy;
	t->internal_format = internal_format;
	t->format = format;
	glGenTextures(1, &t->tex);
	if (dynamic) {
		/* create_pixel_unpack_buffer() */
		glGenBuffers(1, &t->unpack);
		glBindBuffer(GL_PIXEL_UNPACK_BUFFER, t->unpack);
		const GLsizeiptr row = ((GLsizeiptr)cx * bpp + 3) & ~(GLsizeiptr)3;
		glBufferData(GL_PIXEL_UNPACK_BUFFER, row * cy, NULL, GL_DYNAMIC_DRAW);
		glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	}
	glBindTexture(GL_TEXTURE_2D, t->tex);
	glTexImage2D(GL_TEXTURE_2D, 0, internal_format, cx, cy, 0, format, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
	if (render_target) {
		/* get_fbo() + attach_rendertarget() */
		glGenFramebuffers(1, &t->fbo);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, t->fbo);
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
		const GLenum status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		if (status != GL_FRAMEBUFFER_COMPLETE) {
			fprintf(stderr, "render target %dx%d (0x%x) incomplete (0x%x)\n", cx, cy, internal_format,
				status);
			quit(PROBE_NO_CONTEXT);
		}
	}
}

/* gs_texture_destroy() */
static void texture_destroy(struct texture *t)
{
	if (t->unpack)
		glDeleteBuffers(1, &t->unpack);
	if (t->tex)
		glDeleteTextures(1, &t->tex);
	if (t->fbo)
		glDeleteFramebuffers(1, &t->fbo);
	memset(t, 0, sizeof(*t));
}

/* set_async_texture_size(): gs_texture_create(cap_w, cap_h, GS_BGRX, 1, NULL, GS_DYNAMIC) */
static void cap_texture_create(int slot, unsigned frame)
{
	step(slot, frame, "set_async_texture_size(): delete and create the capture texture");
	texture_destroy(&cap_tex);
	texture_create(&cap_tex, cap_w, cap_h, srgb_decode ? GL_SRGB8 : GL_RGBA8, GL_BGRA, 4, true, false);
	check_gl(frame, "capture texture creation", PROBE_NO_CONTEXT);
	cap_frame_in_tex = (unsigned)-1;
	cap_recreations++;
}

/* gs_texture_set_image(): gs_texture_map(), copy the frame, gs_texture_unmap() */
static void cap_upload(int slot, unsigned frame, unsigned n)
{
	step(slot, frame, "gs_texture_map(): glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY)");
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, cap_tex.unpack);
	uint8_t *ptr = glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY);
	check_gl(frame, "glMapBuffer(GL_PIXEL_UNPACK_BUFFER)", PROBE_BAD_PIXELS);
	if (!ptr) {
		fprintf(stderr, "frame %u: glMapBuffer(GL_PIXEL_UNPACK_BUFFER) failed\n", frame);
		quit(PROBE_BAD_PIXELS);
	}
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

	/* the frame, copied row by row from one row template per block row (linesize == width * 4) */
	step(slot, frame, "writing the captured frame into the mapped unpack buffer");
	static uint32_t *row;
	if (!row && !(row = malloc((size_t)cap_w * 4))) {
		fprintf(stderr, "out of memory\n");
		quit(PROBE_USAGE);
	}
	const size_t linesize = (size_t)cap_w * 4;
	for (unsigned by = 0; by < CAP_BY; by++) {
		for (unsigned bx = 0; bx < CAP_BX; bx++) {
			uint8_t bgra[4];
			uint32_t value;
			cap_color(n, bx, by, bgra);
			memcpy(&value, bgra, 4);
			for (unsigned x = bx * (unsigned)cap_w / CAP_BX; x < (bx + 1) * (unsigned)cap_w / CAP_BX; x++)
				row[x] = value;
		}
		for (unsigned y = by * (unsigned)cap_h / CAP_BY; y < (by + 1) * (unsigned)cap_h / CAP_BY; y++)
			memcpy(ptr + (size_t)y * linesize, row, linesize);
	}

	step(slot, frame, "gs_texture_unmap(): glUnmapBuffer + glTexImage2D from the unpack buffer");
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, cap_tex.unpack);
	glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
	check_gl(frame, "glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER)", PROBE_BAD_PIXELS);
	glBindTexture(GL_TEXTURE_2D, cap_tex.tex);
	glTexImage2D(GL_TEXTURE_2D, 0, cap_tex.internal_format, cap_w, cap_h, 0, GL_BGRA, GL_UNSIGNED_BYTE, 0);
	check_gl(frame, "glTexImage2D from the pixel unpack buffer", PROBE_BAD_PIXELS);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);

	cap_frame_in_tex = n;
	cap_uploads++;
}

/* a textured quad over the whole target; device_load_texture(): sRGB textures sampled without decoding */
static void draw_texture(GLuint prog, const struct texture *src, GLuint target_fbo, int cx, int cy)
{
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target_fbo);
	glViewport(0, 0, cx, cy);
	glUseProgram(prog);
	glBindVertexArray(vao_full);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, src->tex);
	if (srgb_decode)
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SRGB_DECODE_EXT, GL_SKIP_DECODE_EXT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindVertexArray(0);
	glUseProgram(0);
}

/* The preview's selection box around the new source, as the frontend draws it (display-helpers.hpp): every edge
 * through gs_render_start()/gs_render_stop(), i.e. gs_vertexbuffer_flush() -> update_buffer(): glMapBufferRange
 * with GL_MAP_INVALIDATE_BUFFER_BIT on a buffer the previous draw may still read. Along the border, away from
 * the pixels that are checked. */
static void draw_selection_box(unsigned frame)
{
	/* clang-format off */
	static const GLfloat e = 0.995f;
	static const GLfloat edges[4][4] = {{-e, -e, e, -e}, {e, -e, e, e}, {e, e, -e, e}, {-e, e, -e, -e}};
	/* clang-format on */
	glUseProgram(prog_solid);
	glBindVertexArray(vao_imm);
	for (int i = 0; i < 4; i++) {
		step(SLOT_RENDER, frame, "main preview: selection box: glMapBufferRange(INVALIDATE) + draw");
		const GLfloat v[IMM_VERTICES * 4] = {edges[i][0], edges[i][1], 0.0f, 0.0f,
						     edges[i][2], edges[i][3], 0.0f, 0.0f};
		glBindBuffer(GL_ARRAY_BUFFER, vbo_imm);
		void *ptr = glMapBufferRange(GL_ARRAY_BUFFER, 0, sizeof(v), GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
		check_gl(frame, "glMapBufferRange(GL_ARRAY_BUFFER)", PROBE_BAD_PIXELS);
		if (!ptr) {
			fprintf(stderr, "frame %u: glMapBufferRange(GL_ARRAY_BUFFER) failed\n", frame);
			quit(PROBE_BAD_PIXELS);
		}
		memcpy(ptr, v, sizeof(v));
		glUnmapBuffer(GL_ARRAY_BUFFER);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glDrawArrays(GL_LINES, 0, 2);
	}
	glBindVertexArray(0);
	glUseProgram(0);
	check_gl(frame, "selection box", PROBE_BAD_PIXELS);
}

/* egl_swapchain_fbo() in gl-haiku.c: (re)specify the renderbuffer storage at the new size and reattach */
static void swapchain_storage(unsigned frame, struct swapchain *sc, int cx, int cy)
{
	if (!sc->fbo) {
		glGenFramebuffers(1, &sc->fbo);
		glGenRenderbuffers(1, &sc->rb);
	}

	GLint prev_rb = 0, prev_draw = 0;
	glGetIntegerv(GL_RENDERBUFFER_BINDING, &prev_rb);
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
	glBindRenderbuffer(GL_RENDERBUFFER, sc->rb);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, cx, cy);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sc->fbo);
	glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, sc->rb);
	const GLenum status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
	glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)prev_rb);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prev_draw);
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		fprintf(stderr, "%s framebuffer %dx%d incomplete (0x%x)\n", sc->name, cx, cy, status);
		quit(PROBE_NO_CONTEXT);
	}
	check_gl(frame, "swapchain framebuffer", PROBE_NO_CONTEXT);

	sc->cx = cx;
	sc->cy = cy;
	const size_t size = (size_t)cx * cy * 4;
	if (sc->readback_size < size) {
		free(sc->readback);
		sc->readback = malloc(size);
		sc->readback_size = size;
		if (!sc->readback) {
			fprintf(stderr, "out of memory\n");
			quit(PROBE_USAGE);
		}
	}
}

/* gl_platform_cleanup_swapchain() -> egl_swapchain_free() */
static void swapchain_free(struct swapchain *sc)
{
	glDeleteFramebuffers(1, &sc->fbo);
	glDeleteRenderbuffers(1, &sc->rb);
	sc->fbo = sc->rb = 0;
}

/* render_display(): device_load_swapchain(), clear, draw, device_present() into client memory */
static void swapchain_render(unsigned frame, struct swapchain *sc, const struct texture *src)
{
	step(SLOT_RENDER, frame, sc == &swapchains[0] ? "main preview: draw" : "properties preview: draw");
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sc->fbo);
	glViewport(0, 0, sc->cx, sc->cy);
	glClearColor(0.3f, 0.3f, 0.3f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	draw_texture(prog_draw, src, sc->fbo, sc->cx, sc->cy);
	if (sc == &swapchains[0])
		draw_selection_box(frame);

	step(SLOT_RENDER, frame,
	     sc == &swapchains[0] ? "main preview: device_present(): glReadPixels(GL_BGRA)"
				  : "properties preview: device_present(): glReadPixels(GL_BGRA)");
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, sc->fbo);
	glReadBuffer(GL_COLOR_ATTACHMENT0);
	glReadPixels(0, 0, sc->cx, sc->cy, GL_BGRA, GL_UNSIGNED_BYTE, sc->readback);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	check_gl(frame, sc->name, PROBE_BAD_PIXELS);
}

/* The centre of every block of captured frame n, scaled to a cx x cy image with the given stride (rows
 * bottom-up like the texture's rows, which is what GL readbacks return). channels: 4 BGRA, 1 R, 2 RG. */
static void check_blocks(unsigned frame, const char *what, const uint8_t *data, int cx, int cy, size_t stride,
			 int channels, unsigned n)
{
	for (unsigned by = 0; by < CAP_BY; by++) {
		for (unsigned bx = 0; bx < CAP_BX; bx++) {
			const int x = (int)((2 * bx + 1) * (unsigned)cx / (2 * CAP_BX));
			const int y = (int)((2 * by + 1) * (unsigned)cy / (2 * CAP_BY));
			const uint8_t *px = data + (size_t)y * stride + (size_t)x * channels;
			uint8_t bgra[4], want[4];
			cap_color(n, bx, by, bgra);
			if (channels == 4)
				memcpy(want, bgra, 4);
			else {
				/* sampled as RGBA: r = bgra[2], g = bgra[1] */
				want[0] = bgra[2];
				want[1] = bgra[1];
			}
			if (!close_enough(px, want, channels)) {
				fprintf(stderr,
					"frame %u, %s %dx%d, captured frame %u: pixel (%d,%d) is %02x %02x %02x %02x, "
					"expected %02x %02x %02x %02x (first %d)\n",
					frame, what, cx, cy, n, x, y, px[0], channels > 1 ? px[1] : 0,
					channels > 2 ? px[2] : 0, channels > 3 ? px[3] : 0, want[0],
					channels > 1 ? want[1] : 0, channels > 2 ? want[2] : 0, channels > 3 ? want[3] : 0,
					channels);
				quit(PROBE_BAD_PIXELS);
			}
		}
	}
}

/* Main thread, context current: what OBS creates at startup (obs_init_video(): the canvas, the conversion
 * targets and the stage surfaces; the main preview's display). */
static void setup_gl(void)
{
	prog_draw = make_program("#version 330 core\n"
				 "uniform sampler2D image;\n"
				 "in vec2 uv;\n"
				 "out vec4 color;\n"
				 "void main() { color = texture(image, uv); }\n");
	prog_y = make_program("#version 330 core\n"
			      "uniform sampler2D image;\n"
			      "in vec2 uv;\n"
			      "out vec4 color;\n"
			      "void main() { color = vec4(texture(image, uv).r, 0.0, 0.0, 1.0); }\n");
	prog_uv = make_program("#version 330 core\n"
			       "uniform sampler2D image;\n"
			       "in vec2 uv;\n"
			       "out vec4 color;\n"
			       "void main() { vec4 c = texture(image, uv); color = vec4(c.r, c.g, 0.0, 1.0); }\n");

	prog_solid = make_program("#version 330 core\n"
				  "uniform sampler2D image;\n"
				  "in vec2 uv;\n"
				  "out vec4 color;\n"
				  "void main() { color = vec4(1.0, 0.0, 0.0, 1.0); }\n");

	/* x, y, u, v: a quad over the whole target */
	/* clang-format off */
	static const GLfloat full[] = {
		-1.0f, -1.0f, 0.0f, 0.0f,   1.0f, -1.0f, 1.0f, 0.0f,   1.0f, 1.0f, 1.0f, 1.0f,
		-1.0f, -1.0f, 0.0f, 0.0f,   1.0f,  1.0f, 1.0f, 1.0f,  -1.0f, 1.0f, 0.0f, 1.0f,
	};
	/* clang-format on */
	glGenVertexArrays(1, &vao_full);
	glBindVertexArray(vao_full);
	glGenBuffers(1, &vbo_full);
	glBindBuffer(GL_ARRAY_BUFFER, vbo_full);
	glBufferData(GL_ARRAY_BUFFER, sizeof(full), full, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (const void *)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (const void *)(2 * sizeof(GLfloat)));
	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	glGenVertexArrays(1, &vao_imm);
	glBindVertexArray(vao_imm);
	glGenBuffers(1, &vbo_imm);
	glBindBuffer(GL_ARRAY_BUFFER, vbo_imm);
	glBufferData(GL_ARRAY_BUFFER, IMM_VERTICES * 4 * sizeof(GLfloat), NULL, GL_STREAM_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (const void *)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (const void *)(2 * sizeof(GLfloat)));
	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	/* GS_RGBA is GL_SRGB8_ALPHA8 (gl-subsystem.h); without EXT_texture_sRGB_decode it could not be sampled
	 * undecoded, and plain RGBA8 keeps the pixel checks exact */
	texture_create(&canvas, canvas_w, canvas_h, srgb_decode ? GL_SRGB8_ALPHA8 : GL_RGBA8, GL_RGBA, 4, false,
		       true);
	if (opt_stage) {
		/* obs_init_gpu_conversion() for NV12, obs_init_gpu_copy_surfaces() */
		texture_create(&conv_y, canvas_w, canvas_h, GL_R8, GL_RED, 1, false, true);
		texture_create(&conv_uv, canvas_w / 2, canvas_h / 2, GL_RG8, GL_RG, 2, false, true);
		y_stride = ((size_t)canvas_w + 3) & ~(size_t)3;
		uv_stride = ((size_t)(canvas_w / 2) * 2 + 3) & ~(size_t)3;
		for (int i = 0; i < 2; i++) {
			glGenBuffers(1, &stage[i].y);
			glBindBuffer(GL_PIXEL_PACK_BUFFER, stage[i].y);
			glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)(y_stride * (size_t)canvas_h), NULL,
				     GL_DYNAMIC_READ);
			glGenBuffers(1, &stage[i].uv);
			glBindBuffer(GL_PIXEL_PACK_BUFFER, stage[i].uv);
			glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)(uv_stride * (size_t)(canvas_h / 2)), NULL,
				     GL_DYNAMIC_READ);
		}
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	}

	/* the main preview exists from the start */
	swapchain_storage(0, &swapchains[0], DISPLAY_A_W, DISPLAY_A_H);

	check_gl(0, "setup", PROBE_NO_CONTEXT);
}

/* ---------------------------------------------------------------------------------------------------------- */
/* The render thread (OBS's graphics thread)                                                                  */

/* gs_stagesurface_unmap() of last frame's set (unmap_last_surface()) */
static void stage_unmap(unsigned frame)
{
	if (!mapped_set)
		return;
	step(SLOT_RENDER, frame, "unmap_last_surface(): glUnmapBuffer(GL_PIXEL_PACK_BUFFER)");
	glBindBuffer(GL_PIXEL_PACK_BUFFER, mapped_set->y);
	glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, mapped_set->uv);
	glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	check_gl(frame, "stage surface unmap", PROBE_BAD_PIXELS);
	mapped_set->py = mapped_set->puv = NULL;
	mapped_set = NULL;
}

/* render_convert_texture() + stage_output_texture() into set cur, then download_frame() of set prev */
static void stage_frame(unsigned frame, unsigned shown)
{
	step(SLOT_RENDER, frame, "render_convert_texture(): draw the NV12 planes");
	draw_texture(prog_y, &canvas, conv_y.fbo, conv_y.cx, conv_y.cy);
	draw_texture(prog_uv, &canvas, conv_uv.fbo, conv_uv.cx, conv_uv.cy);

	stage_unmap(frame);

	struct stage_set *cur = &stage[frame % 2];
	step(SLOT_RENDER, frame, "device_stage_texture(): glGetTexImage into the pixel pack buffers");
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, cur->y);
	glBindTexture(GL_TEXTURE_2D, conv_y.tex);
	glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, 0);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, cur->uv);
	glBindTexture(GL_TEXTURE_2D, conv_uv.tex);
	glGetTexImage(GL_TEXTURE_2D, 0, GL_RG, GL_UNSIGNED_BYTE, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	check_gl(frame, "glGetTexImage into a pixel pack buffer", PROBE_BAD_PIXELS);
	cur->staged = true;
	cur->shown = shown;

	struct stage_set *prev = &stage[(frame + 1) % 2];
	if (!prev->staged)
		return;
	step(SLOT_RENDER, frame, "download_frame(): glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY)");
	glBindBuffer(GL_PIXEL_PACK_BUFFER, prev->y);
	prev->py = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, prev->uv);
	prev->puv = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	check_gl(frame, "glMapBuffer(GL_PIXEL_PACK_BUFFER)", PROBE_BAD_PIXELS);
	if (!prev->py || !prev->puv) {
		fprintf(stderr, "frame %u: glMapBuffer(GL_PIXEL_PACK_BUFFER) failed\n", frame);
		quit(PROBE_BAD_PIXELS);
	}
	mapped_set = prev;
}

/* posted frame numbers for the ui thread */
static pthread_mutex_t ui_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ui_cond = PTHREAD_COND_INITIALIZER;
static unsigned ui_posted;    /* frame + 1 */
static unsigned ui_processed; /* ui_posted as last taken over by the ui thread, once done with it */
static bool ui_stop;

/* wait: until the ui thread has handled this frame (where it opens or reopens the properties preview: a
 * render thread that keeps the context busy must not starve it out of the test) */
static void post_ui(unsigned frame, bool stop, bool wait)
{
	pthread_mutex_lock(&ui_lock);
	ui_posted = frame + 1;
	ui_stop = stop;
	pthread_cond_broadcast(&ui_cond);
	if (wait) {
		step_ex(SLOT_RENDER, frame, "waiting for the ui thread", true);
		while (ui_processed < frame + 1)
			pthread_cond_wait(&ui_cond, &ui_lock);
		step(SLOT_RENDER, frame, NULL);
	}
	pthread_mutex_unlock(&ui_lock);
}

static void ui_handled(unsigned posted)
{
	pthread_mutex_lock(&ui_lock);
	ui_processed = posted;
	pthread_cond_broadcast(&ui_cond);
	pthread_mutex_unlock(&ui_lock);
}

/* the properties dialog: opened in a context hold of the ui thread (or of the render thread with --no-handoff),
 * closed half way through and opened again */
static bool second_opened, second_reopened;
static void second_display_events(unsigned frame, int slot)
{
	struct swapchain *b = &swapchains[1];
	if (!opt_second)
		return;
	if (!second_opened && frame >= 2) {
		step(slot, frame, "gs_swapchain_create(): open the properties preview");
		swapchain_storage(frame, b, display_b_sizes[display_b_size][0], display_b_sizes[display_b_size][1]);
		second_opened = true;
	} else if (second_opened && !second_reopened && frame >= opt_frames / 2 && b->fbo) {
		step(slot, frame, "gs_swapchain_destroy() + gs_swapchain_create(): reopen the properties preview");
		/* same size as before: the client-side readback buffer the render thread checks stays */
		swapchain_free(b);
		swapchain_storage(frame, b, display_b_sizes[display_b_size][0], display_b_sizes[display_b_size][1]);
		second_reopened = true;
	}
}

static void *render_thread(void *arg)
{
	(void)arg;
	const uint64_t interval = opt_fps ? 1000000000ull / opt_fps : 0;
	uint64_t next = now_ns();

	for (unsigned frame = 0; frame < opt_frames; frame++) {
		if (opt_handoff)
			post_ui(frame, false, opt_second && (frame == 3 || frame == opt_frames / 2 + 1));

		/* gs_begin_frame() */
		take_context(SLOT_RENDER, frame);
		release_context(SLOT_RENDER, frame);

		/* tick_sources(): set_async_texture_size() before the first frame and once mid-run, on a frame
		 * that brings a new captured frame, as a size change does */
		const unsigned recreate_at = (opt_frames / 3) & ~1u;
		const bool new_texture = frame == 0 || (opt_stream && frame == recreate_at);
		if (new_texture) {
			take_context(SLOT_RENDER, frame);
			cap_texture_create(SLOT_RENDER, frame);
			release_context(SLOT_RENDER, frame);
		}

		/* output_frame() */
		take_context(SLOT_RENDER, frame);
		if (!opt_handoff)
			second_display_events(frame, SLOT_RENDER);
		/* a new captured frame every other frame (the 30 fps capture); with --no-stream only the first */
		if (frame % 2 == 0 && (opt_stream || cap_frame_in_tex == (unsigned)-1))
			cap_upload(SLOT_RENDER, frame, frame);
		const unsigned shown = cap_frame_in_tex;

		step(SLOT_RENDER, frame, "render_main_texture(): draw the capture into the canvas");
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, canvas.fbo);
		glViewport(0, 0, canvas_w, canvas_h);
		glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		draw_texture(prog_draw, &cap_tex, canvas.fbo, canvas_w, canvas_h);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		check_gl(frame, "canvas", PROBE_BAD_PIXELS);

		if (opt_stage)
			stage_frame(frame, shown);
		struct stage_set *to_check = mapped_set;

		step(SLOT_RENDER, frame, "gs_flush(): glFlush");
		glFlush();
		if (opt_fences)
			wait_fence(SLOT_RENDER, frame, "fence after the output frame");
		release_context(SLOT_RENDER, frame);

		/* output_video_data(): reads the mapped stage surfaces outside the context */
		if (to_check) {
			step(SLOT_RENDER, frame, "reading the mapped stage surfaces");
			check_blocks(frame, "NV12 Y plane", to_check->py, canvas_w, canvas_h, y_stride, 1,
				     to_check->shown);
			check_blocks(frame, "NV12 UV plane", to_check->puv, canvas_w / 2, canvas_h / 2, uv_stride, 2,
				     to_check->shown);
			step(SLOT_RENDER, frame, NULL);
			stage_reads++;
		}

		/* render_displays() */
		take_context(SLOT_RENDER, frame);
		struct swapchain *b = &swapchains[1];
		if (b->fbo && frame % DISPLAY_B_RESIZE_EVERY == DISPLAY_B_RESIZE_EVERY / 2) {
			/* gs_resize() -> gl_update(); egl_swapchain_fbo() reallocates on next use */
			step(SLOT_RENDER, frame, "properties preview: resize (glRenderbufferStorage)");
			display_b_size = (display_b_size + 1) % DISPLAY_B_SIZES;
			swapchain_storage(frame, b, display_b_sizes[display_b_size][0],
					  display_b_sizes[display_b_size][1]);
			swapchain_resizes++;
		}
		swapchain_render(frame, &swapchains[0], &canvas);
		/* the ui thread may reopen it once this hold ends: take what is needed for the check now */
		const bool rendered_b = b->fbo != 0;
		const int b_cx = b->cx, b_cy = b->cy;
		const uint8_t *b_readback = b->readback;
		if (rendered_b)
			swapchain_render(frame, b, &cap_tex);
		if (opt_fences)
			wait_fence(SLOT_RENDER, frame, "fence after the displays");
		release_context(SLOT_RENDER, frame);

		/* the previews' frames, in client memory (OBSQTDisplay copies them) */
		check_blocks(frame, swapchains[0].name, swapchains[0].readback, swapchains[0].cx, swapchains[0].cy,
			     (size_t)swapchains[0].cx * 4, 4, shown);
		if (rendered_b)
			check_blocks(frame, b->name, b_readback, b_cx, b_cy, (size_t)b_cx * 4, 4, shown);

		/* video_sleep() */
		if (interval) {
			next += interval;
			const uint64_t now = now_ns();
			if (next > now) {
				struct timespec ts = {(time_t)((next - now) / 1000000000ull),
						      (long)((next - now) % 1000000000ull)};
				while (nanosleep(&ts, &ts) != 0)
					;
			} else {
				next = now;
			}
		}
	}

	if (opt_handoff)
		post_ui(opt_frames, true, false);
	return NULL;
}

/* OBS's UI thread: takes the context between frames to create and update resources and swapchains. */
static void *ui_thread(void *arg)
{
	(void)arg;
	static uint32_t scratch[64 * 64];
	unsigned done = 0;

	for (;;) {
		pthread_mutex_lock(&ui_lock);
		while (!ui_stop && ui_posted == done)
			pthread_cond_wait(&ui_cond, &ui_lock);
		const bool stop = ui_stop;
		done = ui_posted;
		pthread_mutex_unlock(&ui_lock);
		if (stop)
			break;
		const unsigned frame = done - 1;
		/* events now and then, not every frame */
		if (frame % 3 != 1 && !(opt_second && !second_opened && frame >= 2) &&
		    !(opt_second && !second_reopened && frame >= opt_frames / 2)) {
			ui_handled(done);
			continue;
		}

		take_context(SLOT_UI, frame);
		/* a short-lived texture, like a source or a dialog creating and dropping one */
		step(SLOT_UI, frame, "create, fill and delete a texture");
		for (size_t i = 0; i < 64 * 64; i++)
			scratch[i] = 0xff000000u | (frame * 0x010203u);
		GLuint t = 0;
		glGenTextures(1, &t);
		glBindTexture(GL_TEXTURE_2D, t);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, scratch);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 32, 32, GL_RGBA, GL_UNSIGNED_BYTE, scratch);
		glBindTexture(GL_TEXTURE_2D, 0);
		glDeleteTextures(1, &t);
		second_display_events(frame, SLOT_UI);
		check_gl(frame, "ui thread", PROBE_BAD_PIXELS);
		release_context(SLOT_UI, frame);
		handoffs++;
		ui_handled(done);
	}
	return NULL;
}

/* ---------------------------------------------------------------------------------------------------------- */

/* "IDENTITY <hash>" plus one "IDENTITY-PART <part>" line per part; parsed by gl-haiku.c (run_gl_probe()) */
static void print_identity(const char *renderer, const char *version)
{
	static char text[GL_IDENTITY_MAX];
	gl_identity_build(text, sizeof(text), renderer, version);
	printf("IDENTITY %016llx\n", (unsigned long long)gl_identity_hash(text));
	for (char *part = text; part && *part;) {
		char *end = strchr(part, '\n');
		if (end)
			*end = '\0';
		printf("IDENTITY-PART %s\n", part);
		part = end ? end + 1 : NULL;
	}
	/* flushed now: should the driver hang below, the parent still gets it */
	fflush(stdout);
}

static void usage(const char *argv0, bool help)
{
	fprintf(help ? stdout : stderr,
		"usage: %s [options]\n"
		"  --frames N          frames to render (default 60, at least 16)\n"
		"  --fps N             frame pacing like OBS's video thread; 0: as fast as possible (default 60)\n"
		"  --size WxH          size of the captured screen (default 2560x1080)\n"
		"  --canvas WxH        OBS's base resolution (default 1920x1080)\n"
		"  --no-stream         upload the capture once instead of every other frame\n"
		"  --no-stage          no NV12 conversion and stage surface readback (no recording in progress)\n"
		"  --no-second-display no properties dialog preview\n"
		"  --no-handoff        no second thread taking the context\n"
		"  --fences            wait for a fence after every frame (OBS never does)\n"
		"  --stall SECONDS     report a step that has not returned after this long; 0: never (default 5)\n"
		"  --timeout SECONDS   end the probe after this long plus 5 s (default 30; 0: never)\n"
		"  --verbose           print every step before it runs\n"
		"  --help              this text\n"
		"exit status: 0 pass, 1 usage, 2 no EGL, 3 no OpenGL 3.3 core context, 4 wrong pixels or GL error,\n"
		"             5 a fence or a driver call did not return\n",
		argv0);
	quit(help ? PROBE_OK : PROBE_USAGE);
}

static unsigned parse_uint(const char *argv0, const char *text, unsigned max)
{
	char *end = NULL;
	unsigned long value = strtoul(text, &end, 10);
	if (!*text || *end || value > max)
		usage(argv0, false);
	return (unsigned)value;
}

static void parse_size(const char *argv0, const char *text, int *w, int *h)
{
	char x = 0, extra = 0;
	if (sscanf(text, "%d%c%d%c", w, &x, h, &extra) != 3 || (x != 'x' && x != 'X') || *w < 16 || *h < 16 ||
	    *w > 8192 || *h > 8192)
		usage(argv0, false);
}

int main(int argc, char **argv)
{
	unsigned timeout = 30;
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		const bool has_value = i + 1 < argc;
		if (strcmp(a, "--frames") == 0 && has_value)
			opt_frames = parse_uint(argv[0], argv[++i], 1000000);
		else if (strcmp(a, "--fps") == 0 && has_value)
			opt_fps = parse_uint(argv[0], argv[++i], 1000);
		else if (strcmp(a, "--size") == 0 && has_value)
			parse_size(argv[0], argv[++i], &cap_w, &cap_h);
		else if (strcmp(a, "--canvas") == 0 && has_value)
			parse_size(argv[0], argv[++i], &canvas_w, &canvas_h);
		else if (strcmp(a, "--stall") == 0 && has_value)
			opt_stall_s = parse_uint(argv[0], argv[++i], 3600);
		else if (strcmp(a, "--timeout") == 0 && has_value)
			timeout = parse_uint(argv[0], argv[++i], 86400);
		else if (strcmp(a, "--no-stream") == 0)
			opt_stream = false;
		else if (strcmp(a, "--no-stage") == 0)
			opt_stage = false;
		else if (strcmp(a, "--no-second-display") == 0)
			opt_second = false;
		else if (strcmp(a, "--no-handoff") == 0)
			opt_handoff = false;
		else if (strcmp(a, "--fences") == 0)
			opt_fences = true;
		else if (strcmp(a, "--verbose") == 0)
			opt_verbose = true;
		else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0)
			usage(argv[0], true);
		else
			usage(argv[0], false);
	}
	/* enough for the texture recreation, a resize and the reopening of the second display */
	if (opt_frames < 2 * DISPLAY_B_RESIZE_EVERY)
		opt_frames = 2 * DISPLAY_B_RESIZE_EVERY;
	/* safety net should the parent be gone before it could kill a hung probe */
	if (timeout)
		alarm(timeout + 5);

	const uint64_t start = now_ns();
	pthread_t monitor;
	if (opt_stall_s && pthread_create(&monitor, NULL, stall_monitor, NULL) != 0) {
		fprintf(stderr, "unable to create threads\n");
		quit(PROBE_USAGE);
	}

	step(SLOT_RENDER, 0, "loading EGL");
	if (!gladLoadEGL()) {
		fprintf(stderr, "unable to load EGL entry points\n");
		quit(PROBE_NO_EGL);
	}
	step(SLOT_RENDER, 0, "eglGetDisplay(EGL_DEFAULT_DISPLAY)");
	display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (display == EGL_NO_DISPLAY) {
		fprintf(stderr, "eglGetDisplay failed: %s\n", egl_error());
		quit(PROBE_NO_EGL);
	}
	step(SLOT_RENDER, 0, "eglInitialize");
	EGLint major = 0, minor = 0;
	if (!eglInitialize(display, &major, &minor)) {
		fprintf(stderr, "eglInitialize failed: %s\n", egl_error());
		quit(PROBE_NO_EGL);
	}

	step(SLOT_RENDER, 0, "creating the OpenGL context");
	if (!has_extension(eglQueryString(display, EGL_EXTENSIONS), "EGL_KHR_surfaceless_context")) {
		fprintf(stderr, "EGL_KHR_surfaceless_context missing\n");
		quit(PROBE_NO_CONTEXT);
	}
	if (!eglBindAPI(EGL_OPENGL_API)) {
		fprintf(stderr, "desktop OpenGL is not available: %s\n", egl_error());
		quit(PROBE_NO_CONTEXT);
	}
	EGLConfig config;
	EGLint count = 0;
	if (!eglChooseConfig(display, config_attribs, &config, 1, &count) || count == 0) {
		fprintf(stderr, "no suitable EGL config: %s\n", egl_error());
		quit(PROBE_NO_CONTEXT);
	}
	context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctx_attribs);
#ifdef _DEBUG
	if (context == EGL_NO_CONTEXT && eglGetError() == EGL_BAD_ATTRIBUTE)
		context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctx_attribs + 2);
#endif
	if (context == EGL_NO_CONTEXT) {
		fprintf(stderr, "unable to create an OpenGL 3.3 core context: %s\n", egl_error());
		quit(PROBE_NO_CONTEXT);
	}

	take_context(SLOT_RENDER, 0);
	step(SLOT_RENDER, 0, "loading OpenGL entry points");
	gladLoadGLLoader((GLADloadproc)eglGetProcAddress);
	if (!GLAD_GL_VERSION_3_3 && !gladLoadGL()) {
		fprintf(stderr, "unable to load OpenGL entry points\n");
		quit(PROBE_NO_CONTEXT);
	}
	if (!GLAD_GL_VERSION_3_3) {
		fprintf(stderr, "OpenGL 3.3 is required, got %d.%d\n", GLVersion.major, GLVersion.minor);
		quit(PROBE_NO_CONTEXT);
	}
	srgb_decode = GLAD_GL_EXT_texture_sRGB_decode;
	const char *vendor = (const char *)glGetString(GL_VENDOR);
	const char *renderer = (const char *)glGetString(GL_RENDERER);
	const char *version = (const char *)glGetString(GL_VERSION);
	char info[512];
	snprintf(info, sizeof(info), "GL_VENDOR=%s, GL_RENDERER=%s, GL_VERSION=%s", vendor ? vendor : "?",
		 renderer ? renderer : "?", version ? version : "?");
	step(SLOT_RENDER, 0, "computing the driver identity");
	print_identity(renderer, version);
	step(SLOT_RENDER, 0, "creating OBS's startup resources");
	setup_gl();
	release_context(SLOT_RENDER, 0);
	const double init_ms = (double)(now_ns() - start) / 1e6;

	pthread_t render, ui;
	if ((opt_handoff && pthread_create(&ui, NULL, ui_thread, NULL) != 0) ||
	    pthread_create(&render, NULL, render_thread, NULL) != 0) {
		fprintf(stderr, "unable to create threads\n");
		quit(PROBE_USAGE);
	}
	pthread_join(render, NULL);
	if (opt_handoff)
		pthread_join(ui, NULL);

	/* everything submitted must complete */
	take_context(SLOT_RENDER, opt_frames);
	stage_unmap(opt_frames);
	wait_fence(SLOT_RENDER, opt_frames, "final fence");
	release_context(SLOT_RENDER, opt_frames);

	/* No teardown: a driver that only misbehaves on exit should not cost OBS the hardware renderer, and the
	 * kernel frees everything with the team. */
	char pacing[32];
	if (opt_fps)
		snprintf(pacing, sizeof(pacing), "%u fps", opt_fps);
	else
		snprintf(pacing, sizeof(pacing), "unpaced");
	printf("OK: EGL %d.%d, %s (%u frames %s, %dx%d capture: %u uploads, %u recreations; %u stage "
	       "readbacks, %u swapchain resizes, %u context hand-offs; init %.0f ms, total %.0f ms)\n",
	       major, minor, info, opt_frames, pacing, cap_w, cap_h, cap_uploads,
	       cap_recreations, stage_reads, swapchain_resizes, handoffs, init_ms, (double)(now_ns() - start) / 1e6);
	quit(PROBE_OK);
}
