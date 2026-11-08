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
 * pattern then costs a fallback to the CPU renderer (OSMesa) instead of a frozen OBS.
 *
 * It does what libobs-opengl does on Haiku, through the same glad loaders:
 *  - EGL_DEFAULT_DISPLAY, EGL_KHR_surfaceless_context, eglBindAPI(EGL_OPENGL_API), the config and the
 *    OpenGL 3.3 core context attributes of gl-haiku.c; the context is made current without a surface;
 *  - one context is handed between threads under a mutex, as OBS's graphics mutex does: the main thread
 *    creates the resources, the render thread (OBS's graphics thread) renders every frame, and an upload
 *    thread (OBS's UI/source threads) takes the context between frames to update and create textures. Each
 *    release is eglMakeCurrent(NO_SURFACE, NO_SURFACE, NO_CONTEXT), like device_leave_context();
 *  - each frame, into a "canvas" framebuffer object with an RGBA8 renderbuffer: clear + a textured quad drawn
 *    from a vertex buffer with a GLSL 330 program, a fence waited on with glClientWaitSync, glReadPixels as
 *    device_present() does (GL_BGRA), and a pixel pack buffer readback mapped with glMapBuffer as stage
 *    surfaces do. Every pixel of both readbacks is checked;
 *  - in the first 48 frames, a 2560x1080 async-video texture streamed exactly like an async source (Screen Capture:
 *    BGRX, 30 fps) through libobs's gs_texture_set_image(): a dynamic texture (GS_BGRX: GL_SRGB8 from
 *    GL_BGRA) with its own GL_PIXEL_UNPACK_BUFFER (glBufferData(NULL, GL_DYNAMIC_DRAW), never orphaned),
 *    glMapBuffer(GL_WRITE_ONLY), the pixel buffer unbound while the frame is written, glUnmapBuffer and
 *    glTexImage2D from offset 0, as gs_texture_map()/gs_texture_unmap() in gl-texture2d.c do. Odd frames
 *    upload on the upload thread, even frames on the render thread; the texture is created on the first
 *    frame and recreated once mid-run, like set_async_texture_size();
 *  - two "swapchains" (the preview and a properties-dialog preview), framebuffer objects of different sizes
 *    created and resized the way gl-haiku.c does it (glRenderbufferStorage on the same renderbuffer), each
 *    drawn with the streamed texture and read back into client memory in those frames like device_load_swapchain()
 *    + device_present(); the second one is resized several times and destroyed/recreated once from the
 *    upload thread. Every frame ends with a second fence. The streamed colour of every block is checked in
 *    both swapchains, so a stale or partial upload is caught too.
 *
 * Output: "IDENTITY <hash>" and "IDENTITY-PART <part>" lines with the driver identity (gl-identity.h, used
 * by gl-haiku.c to recognise a driver that hung before), then one line on success ("OK: GL_VENDOR=...");
 * diagnostics on stderr. The process exits with _exit() everywhere, so a hung driver thread can never keep it
 * alive, and it leaves no files behind. alarm() ends it should the parent die before it could kill a hung
 * probe.
 *
 * Usage: obs-haiku-gl-probe [--frames N] [--timeout SECONDS]
 */

#include <pthread.h>
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
	PROBE_STALL = 5,      /* a fence did not signal in time */
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

#define WIDTH 256
#define HEIGHT 144
#define TEX_SIZE 64
#define FENCE_TIMEOUT_NS 2000000000ull

/* the streamed async-video texture: Screen Capture's frame size; a frame-dependent colour per block */
#define STREAM_W 2560
#define STREAM_H 1080
#define STREAM_BX 8
#define STREAM_BY 4

/* swapchain sizes: the preview, and a properties-dialog preview that gets resized */
#define DISPLAY_A_W 1280
#define DISPLAY_A_H 720
static const int display_b_sizes[][2] = {{640, 360}, {800, 450}, {480, 270}, {720, 405}};
#define DISPLAY_B_SIZES (sizeof(display_b_sizes) / sizeof(display_b_sizes[0]))
#define DISPLAY_B_RESIZE_EVERY 8

/* frames with streaming and swapchains (1.6 s of a 30 fps capture); the others only do the canvas work, so the
 * probe stays well under a second on a healthy driver */
#define STREAM_FRAMES 48

static const uint8_t clear_rgba[4] = {0x30, 0x20, 0x10, 0xff};

static EGLDisplay display = EGL_NO_DISPLAY;
static EGLContext context = EGL_NO_CONTEXT;

/* models OBS's graphics mutex; whoever holds it has the context current */
static pthread_mutex_t ctx_lock = PTHREAD_MUTEX_INITIALIZER;

/* render thread <-> upload thread hand-off */
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t state_cond = PTHREAD_COND_INITIALIZER;
static unsigned uploads_requested, uploads_done, upload_frame;
static bool stop_uploads;

/* GL objects and the colour last uploaded into tex; guarded by ctx_lock */
static GLuint fbo, color_rb, tex, prog, vao, vbo, pbo, vao_full, vbo_full;
static uint8_t tex_rgba[4];

/* the streamed texture and its pixel unpack buffer (gs_texture_create(..., GS_DYNAMIC)); guarded by ctx_lock */
static GLuint stream_tex, stream_pbo;
static GLint stream_internal_format;
static unsigned stream_frame; /* frame number whose pattern is in stream_tex */
static unsigned stream_uploads;

/* a swapchain: framebuffer object + colour renderbuffer, like struct gl_windowinfo in gl-haiku.c */
struct swapchain {
	const char *name;
	GLuint fbo, rb;
	int cx, cy;
	uint8_t *readback;
	size_t readback_size;
};
static struct swapchain swapchains[2] = {{.name = "preview swapchain"}, {.name = "properties swapchain"}};
static unsigned display_b_size;    /* index into display_b_sizes; guarded by ctx_lock */
static unsigned swapchain_resizes; /* render thread only */

static unsigned frames = 120;
static unsigned stream_frames = STREAM_FRAMES;

_Noreturn static void quit(int code)
{
	fflush(stdout);
	fflush(stderr);
	_exit(code);
}

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

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

static void take_context(void)
{
	pthread_mutex_lock(&ctx_lock);
	if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) {
		fprintf(stderr, "eglMakeCurrent failed: %s\n", egl_error());
		quit(PROBE_NO_CONTEXT);
	}
}

static void release_context(void)
{
	if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT)) {
		fprintf(stderr, "eglMakeCurrent(EGL_NO_CONTEXT) failed: %s\n", egl_error());
		quit(PROBE_NO_CONTEXT);
	}
	pthread_mutex_unlock(&ctx_lock);
}

static void check_gl(const char *what, int code)
{
	GLenum err = glGetError();
	if (err != GL_NO_ERROR) {
		fprintf(stderr, "GL error 0x%x during %s\n", err, what);
		quit(code);
	}
}

static void wait_fence(unsigned frame, const char *what)
{
	GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	if (!sync) {
		fprintf(stderr, "frame %u: glFenceSync (%s) failed\n", frame, what);
		quit(PROBE_BAD_PIXELS);
	}
	GLenum wait = glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, FENCE_TIMEOUT_NS);
	glDeleteSync(sync);
	if (wait == GL_TIMEOUT_EXPIRED || wait == GL_WAIT_FAILED) {
		fprintf(stderr, "frame %u: fence (%s) %s\n", frame, what,
			wait == GL_WAIT_FAILED ? "wait failed" : "not signalled after 2 s");
		quit(PROBE_STALL);
	}
}

static void upload_color(unsigned n)
{
	uint8_t rgba[4] = {(uint8_t)(0x40 + n * 0x3b), (uint8_t)(n * 0x71), (uint8_t)(0x80 + n * 0x17), 0xff};
	static uint32_t pixels[TEX_SIZE * TEX_SIZE];
	uint32_t value;
	memcpy(&value, rgba, 4);
	for (size_t i = 0; i < TEX_SIZE * TEX_SIZE; i++)
		pixels[i] = value;

	glBindTexture(GL_TEXTURE_2D, tex);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, TEX_SIZE, TEX_SIZE, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	glBindTexture(GL_TEXTURE_2D, 0);
	memcpy(tex_rgba, rgba, 4);
}

/* ---------------------------------------------------------------------------------------------------------- */
/* Streamed async-video texture                                                                               */

/* BGRX colour of block (bx, by) in frame n; changes every frame in every channel but X */
static void stream_color(unsigned n, unsigned bx, unsigned by, uint8_t bgra[4])
{
	bgra[0] = (uint8_t)(bx * 29 + by * 7 + n * 11 + 3);
	bgra[1] = (uint8_t)(by * 53 + bx * 3 + n * 37 + 0x20);
	bgra[2] = (uint8_t)(bx * 17 + by * 89 + n * 5 + 0x60);
	bgra[3] = 0xff;
}

/* gs_texture_create(STREAM_W, STREAM_H, GS_BGRX, 1, NULL, GS_DYNAMIC) as set_async_texture_size() does it */
static void stream_create(void)
{
	/* create_pixel_unpack_buffer() */
	glGenBuffers(1, &stream_pbo);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, stream_pbo);
	glBufferData(GL_PIXEL_UNPACK_BUFFER, (GLsizeiptr)STREAM_W * 4 * STREAM_H, NULL, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

	/* upload_texture_2d() with no data: GS_BGRX is GL_SRGB8 from GL_BGRA (gl-subsystem.h). It is sampled with
	 * GL_SKIP_DECODE_EXT (device_load_texture()); without EXT_texture_sRGB_decode that is not possible, and
	 * a plain RGBA8 texture keeps the pixel check exact. */
	stream_internal_format = GLAD_GL_EXT_texture_sRGB_decode ? GL_SRGB8 : GL_RGBA8;
	glGenTextures(1, &stream_tex);
	glBindTexture(GL_TEXTURE_2D, stream_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, stream_internal_format, STREAM_W, STREAM_H, 0, GL_BGRA, GL_UNSIGNED_BYTE,
		     NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
	check_gl("streamed texture creation", PROBE_NO_CONTEXT);
	stream_frame = (unsigned)-1;
}

static void stream_destroy(void)
{
	glDeleteTextures(1, &stream_tex);
	glDeleteBuffers(1, &stream_pbo);
	stream_tex = stream_pbo = 0;
}

/* gs_texture_set_image(): gs_texture_map(), copy the frame, gs_texture_unmap() */
static void stream_upload(unsigned n)
{
	/* gs_texture_map(): map, then unbind while the caller writes */
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, stream_pbo);
	uint8_t *ptr = glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY);
	check_gl("glMapBuffer(GL_PIXEL_UNPACK_BUFFER)", PROBE_BAD_PIXELS);
	if (!ptr) {
		fprintf(stderr, "frame %u: glMapBuffer(GL_PIXEL_UNPACK_BUFFER) failed\n", n);
		quit(PROBE_BAD_PIXELS);
	}
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

	/* the frame: one row template per block row, copied row by row (linesize == width * 4) */
	static uint32_t row[STREAM_W];
	const size_t linesize = (size_t)STREAM_W * 4;
	for (unsigned by = 0; by < STREAM_BY; by++) {
		for (unsigned bx = 0; bx < STREAM_BX; bx++) {
			uint8_t bgra[4];
			uint32_t value;
			stream_color(n, bx, by, bgra);
			memcpy(&value, bgra, 4);
			for (unsigned x = bx * STREAM_W / STREAM_BX; x < (bx + 1) * STREAM_W / STREAM_BX; x++)
				row[x] = value;
		}
		for (unsigned y = by * STREAM_H / STREAM_BY; y < (by + 1) * STREAM_H / STREAM_BY; y++)
			memcpy(ptr + (size_t)y * linesize, row, linesize);
	}

	/* gs_texture_unmap() */
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, stream_pbo);
	glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
	check_gl("glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER)", PROBE_BAD_PIXELS);
	glBindTexture(GL_TEXTURE_2D, stream_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, stream_internal_format, STREAM_W, STREAM_H, 0, GL_BGRA, GL_UNSIGNED_BYTE, 0);
	check_gl("glTexImage2D from the pixel unpack buffer", PROBE_BAD_PIXELS);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);

	stream_frame = n;
	stream_uploads++;
}

/* ---------------------------------------------------------------------------------------------------------- */
/* Swapchains                                                                                                 */

/* egl_swapchain_fbo() in gl-haiku.c: (re)specify the renderbuffer storage at the new size and reattach */
static void swapchain_storage(struct swapchain *sc, int cx, int cy)
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
	check_gl("swapchain framebuffer", PROBE_NO_CONTEXT);

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

/* device_load_swapchain() + drawing the streamed texture + device_present() into client memory */
static void swapchain_render(struct swapchain *sc)
{
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sc->fbo);
	glViewport(0, 0, sc->cx, sc->cy);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glUseProgram(prog);
	glBindVertexArray(vao_full);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, stream_tex);
	/* device_load_texture(): no sRGB decode; the preview is scaled with linear filtering */
	if (stream_internal_format == GL_SRGB8)
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SRGB_DECODE_EXT, GL_SKIP_DECODE_EXT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindVertexArray(0);
	glUseProgram(0);

	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, sc->fbo);
	glReadBuffer(GL_COLOR_ATTACHMENT0);
	glReadPixels(0, 0, sc->cx, sc->cy, GL_BGRA, GL_UNSIGNED_BYTE, sc->readback);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	check_gl(sc->name, PROBE_BAD_PIXELS);
}

static bool close_enough(const uint8_t *px, const uint8_t want[4])
{
	for (int i = 0; i < 4; i++) {
		int d = (int)px[i] - (int)want[i];
		if (d < -1 || d > 1)
			return false;
	}
	return true;
}

/* the centre of every block of the streamed frame n, scaled to the swapchain (BGRA readback, bottom-up rows
 * matching the texture's rows) */
static void check_swapchain(unsigned frame, const struct swapchain *sc, unsigned n)
{
	for (unsigned by = 0; by < STREAM_BY; by++) {
		for (unsigned bx = 0; bx < STREAM_BX; bx++) {
			const int x = (int)((2 * bx + 1) * (unsigned)sc->cx / (2 * STREAM_BX));
			const int y = (int)((2 * by + 1) * (unsigned)sc->cy / (2 * STREAM_BY));
			const uint8_t *px = sc->readback + ((size_t)y * sc->cx + x) * 4;
			uint8_t want[4];
			stream_color(n, bx, by, want);
			if (!close_enough(px, want)) {
				fprintf(stderr,
					"frame %u, %s %dx%d, streamed texture frame %u: pixel (%d,%d) is %02x %02x %02x "
					"%02x, expected %02x %02x %02x %02x\n",
					frame, sc->name, sc->cx, sc->cy, n, x, y, px[0], px[1], px[2], px[3], want[0],
					want[1], want[2], want[3]);
				quit(PROBE_BAD_PIXELS);
			}
		}
	}
}

/* ---------------------------------------------------------------------------------------------------------- */

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

static void make_quad(GLuint *out_vao, GLuint *out_vbo, const GLfloat *quad, size_t size)
{
	glGenVertexArrays(1, out_vao);
	glBindVertexArray(*out_vao);
	glGenBuffers(1, out_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, *out_vbo);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)size, quad, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (const void *)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (const void *)(2 * sizeof(GLfloat)));
	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}

/* Main thread, context current: what OBS creates at startup. */
static void setup_gl(void)
{
	glGenRenderbuffers(1, &color_rb);
	glBindRenderbuffer(GL_RENDERBUFFER, color_rb);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, WIDTH, HEIGHT);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
	glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color_rb);
	GLenum status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		fprintf(stderr, "framebuffer incomplete (0x%x)\n", status);
		quit(PROBE_NO_CONTEXT);
	}

	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, TEX_SIZE, TEX_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	upload_color(0);

	static const char *vs = "#version 330 core\n"
				"layout(location = 0) in vec2 pos;\n"
				"layout(location = 1) in vec2 in_uv;\n"
				"out vec2 uv;\n"
				"void main() { uv = in_uv; gl_Position = vec4(pos, 0.0, 1.0); }\n";
	static const char *fs = "#version 330 core\n"
				"uniform sampler2D image;\n"
				"in vec2 uv;\n"
				"out vec4 color;\n"
				"void main() { color = texture(image, uv); }\n";
	prog = glCreateProgram();
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

	/* x, y, u, v: a quad over the left half, and one over the whole target */
	/* clang-format off */
	static const GLfloat quad[] = {
		-1.0f, -1.0f, 0.0f, 0.0f,   0.0f, -1.0f, 1.0f, 0.0f,   0.0f, 1.0f, 1.0f, 1.0f,
		-1.0f, -1.0f, 0.0f, 0.0f,   0.0f,  1.0f, 1.0f, 1.0f,  -1.0f, 1.0f, 0.0f, 1.0f,
	};
	static const GLfloat full[] = {
		-1.0f, -1.0f, 0.0f, 0.0f,   1.0f, -1.0f, 1.0f, 0.0f,   1.0f, 1.0f, 1.0f, 1.0f,
		-1.0f, -1.0f, 0.0f, 0.0f,   1.0f,  1.0f, 1.0f, 1.0f,  -1.0f, 1.0f, 0.0f, 1.0f,
	};
	/* clang-format on */
	make_quad(&vao, &vbo, quad, sizeof(quad));
	make_quad(&vao_full, &vbo_full, full, sizeof(full));

	glGenBuffers(1, &pbo);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
	glBufferData(GL_PIXEL_PACK_BUFFER, WIDTH * HEIGHT * 4, NULL, GL_DYNAMIC_READ);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

	/* the preview swapchain exists from the start */
	swapchain_storage(&swapchains[0], DISPLAY_A_W, DISPLAY_A_H);

	check_gl("setup", PROBE_NO_CONTEXT);
}

/* bgra: true for GL_BGRA data, false for GL_RGBA */
static void check_pixels(unsigned frame, const char *what, const uint8_t *data, bool bgra, const uint8_t quad_rgba[4])
{
	uint8_t quad[4], back[4];
	memcpy(quad, quad_rgba, 4);
	memcpy(back, clear_rgba, 4);
	if (bgra) {
		quad[0] = quad_rgba[2];
		quad[2] = quad_rgba[0];
		back[0] = clear_rgba[2];
		back[2] = clear_rgba[0];
	}

	for (int y = 0; y < HEIGHT; y++) {
		for (int x = 0; x < WIDTH; x++) {
			const uint8_t *px = data + ((size_t)y * WIDTH + x) * 4;
			const uint8_t *want = x < WIDTH / 2 ? quad : back;
			if (!close_enough(px, want)) {
				fprintf(stderr,
					"frame %u, %s: pixel (%d,%d) is %02x %02x %02x %02x, expected %02x %02x %02x %02x\n",
					frame, what, x, y, px[0], px[1], px[2], px[3], want[0], want[1], want[2],
					want[3]);
				quit(PROBE_BAD_PIXELS);
			}
		}
	}
}

static void request_upload(unsigned frame)
{
	pthread_mutex_lock(&state_lock);
	upload_frame = frame;
	unsigned want = ++uploads_requested;
	pthread_cond_broadcast(&state_cond);
	while (uploads_done < want)
		pthread_cond_wait(&state_cond, &state_lock);
	pthread_mutex_unlock(&state_lock);
}

/* OBS's graphics thread: render, fence, read back. */
static void *render_thread(void *arg)
{
	(void)arg;
	static uint8_t pixels[WIDTH * HEIGHT * 4];

	for (unsigned frame = 0; frame < frames; frame++) {
		/* another thread updates textures between frames (and streams odd frames) */
		if (frame % 2 == 1)
			request_upload(frame);

		take_context();

		/* the async source's texture appears with its first frame and is recreated once (a size or format
		 * change in set_async_texture_size()), on an even frame, which is uploaded here as the graphics
		 * thread does */
		const bool streaming = frame < stream_frames;
		if (frame == 0 || frame == ((stream_frames / 3) & ~1u)) {
			if (stream_tex)
				stream_destroy();
			stream_create();
		}
		if (streaming && frame % 2 == 0)
			stream_upload(frame);

		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
		glViewport(0, 0, WIDTH, HEIGHT);
		glClearColor(clear_rgba[0] / 255.0f, clear_rgba[1] / 255.0f, clear_rgba[2] / 255.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glUseProgram(prog);
		glBindVertexArray(vao);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, tex);
		glDrawArrays(GL_TRIANGLES, 0, 6);
		glBindTexture(GL_TEXTURE_2D, 0);
		glBindVertexArray(0);
		glUseProgram(0);

		wait_fence(frame, "canvas");

		/* device_present(): synchronous readback of the swapchain */
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		glPixelStorei(GL_PACK_ALIGNMENT, 4);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
		glReadBuffer(GL_COLOR_ATTACHMENT0);
		glReadPixels(0, 0, WIDTH, HEIGHT, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
		check_gl("glReadPixels", PROBE_BAD_PIXELS);

		/* stage surface: readback into a pixel pack buffer, then map it */
		glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
		glReadPixels(0, 0, WIDTH, HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		const uint8_t *mapped = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
		check_gl("pixel pack buffer readback", PROBE_BAD_PIXELS);
		if (!mapped) {
			fprintf(stderr, "frame %u: glMapBuffer failed\n", frame);
			quit(PROBE_BAD_PIXELS);
		}

		uint8_t quad[4];
		memcpy(quad, tex_rgba, 4);
		check_pixels(frame, "pixel pack buffer", mapped, false, quad);
		glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		check_gl("frame", PROBE_BAD_PIXELS);

		/* render_displays(): every display, each its own swapchain; the properties dialog's preview is
		 * resized now and then (gl_update() + the lazy egl_swapchain_fbo()) */
		struct swapchain *b = &swapchains[1];
		if (streaming && b->fbo && frame % DISPLAY_B_RESIZE_EVERY == DISPLAY_B_RESIZE_EVERY / 2) {
			display_b_size = (display_b_size + 1) % DISPLAY_B_SIZES;
			swapchain_storage(b, display_b_sizes[display_b_size][0], display_b_sizes[display_b_size][1]);
			swapchain_resizes++;
		}
		if (streaming) {
			swapchain_render(&swapchains[0]);
			if (b->fbo)
				swapchain_render(b);
		}
		const bool rendered_b = streaming && b->fbo != 0;
		const unsigned shown = stream_frame;

		wait_fence(frame, "end of frame");

		release_context();

		check_pixels(frame, "glReadPixels", pixels, true, quad);
		if (streaming)
			check_swapchain(frame, &swapchains[0], shown);
		if (rendered_b)
			check_swapchain(frame, b, shown);
	}

	pthread_mutex_lock(&state_lock);
	stop_uploads = true;
	pthread_cond_broadcast(&state_cond);
	pthread_mutex_unlock(&state_lock);
	return NULL;
}

/* OBS's UI and source threads: take the context between frames to update and create textures and swapchains. */
static void *upload_thread(void *arg)
{
	(void)arg;
	static uint32_t scratch[TEX_SIZE * TEX_SIZE];

	for (;;) {
		pthread_mutex_lock(&state_lock);
		while (!stop_uploads && uploads_done == uploads_requested)
			pthread_cond_wait(&state_cond, &state_lock);
		const bool stop = stop_uploads;
		const unsigned n = uploads_done + 1;
		const unsigned frame = upload_frame;
		pthread_mutex_unlock(&state_lock);
		if (stop)
			break;

		take_context();
		upload_color(n);
		/* a short-lived texture, like a source creating and dropping one */
		GLuint t = 0;
		glGenTextures(1, &t);
		glBindTexture(GL_TEXTURE_2D, t);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, TEX_SIZE, TEX_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, scratch);
		glBindTexture(GL_TEXTURE_2D, 0);
		glDeleteTextures(1, &t);

		if (frame < stream_frames) {
			/* gs_texture_set_image() under obs_enter_graphics() on another thread */
			stream_upload(frame);

			/* the properties dialog opens (a second display: gs_swapchain_create() on the UI thread)
			 * and, half way through, closes and opens again */
			struct swapchain *b = &swapchains[1];
			if (frame == (stream_frames / 2 | 1u) && b->fbo)
				swapchain_free(b);
			if (!b->fbo)
				swapchain_storage(b, display_b_sizes[display_b_size][0],
						  display_b_sizes[display_b_size][1]);
		}

		glFlush();
		check_gl("texture upload", PROBE_BAD_PIXELS);
		release_context();

		pthread_mutex_lock(&state_lock);
		uploads_done = n;
		pthread_cond_broadcast(&state_cond);
		pthread_mutex_unlock(&state_lock);
	}
	return NULL;
}

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

int main(int argc, char **argv)
{
	unsigned timeout = 30;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
			frames = (unsigned)strtoul(argv[++i], NULL, 10);
		} else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
			timeout = (unsigned)strtoul(argv[++i], NULL, 10);
		} else {
			fprintf(stderr, "usage: %s [--frames N] [--timeout SECONDS]\n", argv[0]);
			return PROBE_USAGE;
		}
	}
	/* enough for the stream recreation, a swapchain resize and the swapchain's destruction to happen */
	if (frames < 2 * DISPLAY_B_RESIZE_EVERY)
		frames = 2 * DISPLAY_B_RESIZE_EVERY;
	if (stream_frames > frames)
		stream_frames = frames;
	/* safety net should the parent be gone before it could kill a hung probe */
	if (timeout)
		alarm(timeout + 5);

	const double start = now_ms();

	if (!gladLoadEGL()) {
		fprintf(stderr, "unable to load EGL entry points\n");
		quit(PROBE_NO_EGL);
	}
	display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (display == EGL_NO_DISPLAY) {
		fprintf(stderr, "eglGetDisplay failed: %s\n", egl_error());
		quit(PROBE_NO_EGL);
	}
	EGLint major = 0, minor = 0;
	if (!eglInitialize(display, &major, &minor)) {
		fprintf(stderr, "eglInitialize failed: %s\n", egl_error());
		quit(PROBE_NO_EGL);
	}

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

	take_context();
	gladLoadGLLoader((GLADloadproc)eglGetProcAddress);
	if (!GLAD_GL_VERSION_3_3 && !gladLoadGL()) {
		fprintf(stderr, "unable to load OpenGL entry points\n");
		quit(PROBE_NO_CONTEXT);
	}
	if (!GLAD_GL_VERSION_3_3) {
		fprintf(stderr, "OpenGL 3.3 is required, got %d.%d\n", GLVersion.major, GLVersion.minor);
		quit(PROBE_NO_CONTEXT);
	}
	const char *vendor = (const char *)glGetString(GL_VENDOR);
	const char *renderer = (const char *)glGetString(GL_RENDERER);
	const char *version = (const char *)glGetString(GL_VERSION);
	char info[512];
	snprintf(info, sizeof(info), "GL_VENDOR=%s, GL_RENDERER=%s, GL_VERSION=%s", vendor ? vendor : "?",
		 renderer ? renderer : "?", version ? version : "?");
	print_identity(renderer, version);
	setup_gl();
	release_context();

	pthread_t render, upload;
	if (pthread_create(&upload, NULL, upload_thread, NULL) != 0 ||
	    pthread_create(&render, NULL, render_thread, NULL) != 0) {
		fprintf(stderr, "unable to create threads\n");
		quit(PROBE_USAGE);
	}
	pthread_join(render, NULL);
	pthread_join(upload, NULL);

	/* No teardown: a driver that only misbehaves on exit should not cost OBS the hardware renderer, and the
	 * kernel frees everything with the team. */
	printf("OK: EGL %d.%d, %s (%u frames, %u context hand-offs, %u streamed %dx%d uploads, %u swapchain "
	       "resizes, %.0f ms)\n",
	       major, minor, info, frames, uploads_done, stream_uploads, STREAM_W, STREAM_H, swapchain_resizes,
	       now_ms() - start);
	quit(PROBE_OK);
}
