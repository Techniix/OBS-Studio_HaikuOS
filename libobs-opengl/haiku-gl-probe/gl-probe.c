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
 *  - the "swapchain" is a framebuffer object with an RGBA8 renderbuffer;
 *  - one context is handed between threads under a mutex, as OBS's graphics mutex does: the main thread
 *    creates the resources, the render thread (OBS's graphics thread) renders every frame, and an upload
 *    thread (OBS's UI/source threads) takes the context between frames to update and create textures. Each
 *    release is eglMakeCurrent(NO_SURFACE, NO_SURFACE, NO_CONTEXT), like device_leave_context();
 *  - each frame: clear + a textured quad drawn from a vertex buffer with a GLSL 330 program, a fence waited
 *    on with glClientWaitSync, glReadPixels as device_present() does (GL_BGRA), and a pixel pack buffer
 *    readback mapped with glMapBuffer as stage surfaces do. Every pixel of both readbacks is checked.
 *
 * Output: one line on stdout on success ("OK: GL_VENDOR=..."), diagnostics on stderr. The process exits with
 * _exit() everywhere, so a hung driver thread can never keep it alive, and it leaves no files behind. alarm()
 * ends it should the parent die before it could kill a hung probe.
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

static const uint8_t clear_rgba[4] = {0x30, 0x20, 0x10, 0xff};

static EGLDisplay display = EGL_NO_DISPLAY;
static EGLContext context = EGL_NO_CONTEXT;

/* models OBS's graphics mutex; whoever holds it has the context current */
static pthread_mutex_t ctx_lock = PTHREAD_MUTEX_INITIALIZER;

/* render thread <-> upload thread hand-off */
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t state_cond = PTHREAD_COND_INITIALIZER;
static unsigned uploads_requested, uploads_done;
static bool stop_uploads;

/* GL objects and the colour last uploaded into tex; guarded by ctx_lock */
static GLuint fbo, color_rb, tex, prog, vao, vbo, pbo;
static uint8_t tex_rgba[4];

static unsigned frames = 120;

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

	/* a quad over the left half: x, y, u, v */
	/* clang-format off */
	static const GLfloat quad[] = {
		-1.0f, -1.0f, 0.0f, 0.0f,   0.0f, -1.0f, 1.0f, 0.0f,   0.0f, 1.0f, 1.0f, 1.0f,
		-1.0f, -1.0f, 0.0f, 0.0f,   0.0f,  1.0f, 1.0f, 1.0f,  -1.0f, 1.0f, 0.0f, 1.0f,
	};
	/* clang-format on */
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (const void *)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (const void *)(2 * sizeof(GLfloat)));
	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	glGenBuffers(1, &pbo);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
	glBufferData(GL_PIXEL_PACK_BUFFER, WIDTH * HEIGHT * 4, NULL, GL_DYNAMIC_READ);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

	check_gl("setup", PROBE_NO_CONTEXT);
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

static void request_upload(void)
{
	pthread_mutex_lock(&state_lock);
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
		/* another thread updates a texture between frames */
		if (frame % 2 == 1)
			request_upload();

		take_context();

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

		GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		if (!sync) {
			fprintf(stderr, "frame %u: glFenceSync failed\n", frame);
			quit(PROBE_BAD_PIXELS);
		}
		GLenum wait = glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, FENCE_TIMEOUT_NS);
		glDeleteSync(sync);
		if (wait == GL_TIMEOUT_EXPIRED || wait == GL_WAIT_FAILED) {
			fprintf(stderr, "frame %u: fence %s\n", frame,
				wait == GL_WAIT_FAILED ? "wait failed" : "not signalled after 2 s");
			quit(PROBE_STALL);
		}

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

		release_context();

		check_pixels(frame, "glReadPixels", pixels, true, quad);
	}

	pthread_mutex_lock(&state_lock);
	stop_uploads = true;
	pthread_cond_broadcast(&state_cond);
	pthread_mutex_unlock(&state_lock);
	return NULL;
}

/* OBS's UI and source threads: take the context between frames to update and create textures. */
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
	if (frames < 2)
		frames = 2;
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
	printf("OK: EGL %d.%d, %s (%u frames, %u context hand-offs, %.0f ms)\n", major, minor, info, frames,
	       uploads_done, now_ms() - start);
	quit(PROBE_OK);
}
