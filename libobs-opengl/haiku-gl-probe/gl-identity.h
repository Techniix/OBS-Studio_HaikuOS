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
 * Identity of the OpenGL driver stack, shared by obs-haiku-gl-probe (gl-probe.c) and gl-haiku.c.
 *
 * When the hardware driver hangs at run time, gl-haiku.c records the driver's identity in a marker file and
 * later sessions use the CPU renderer as long as the same driver is installed. "The same driver" means:
 *  - the same GL_RENDERER and GL_VERSION strings,
 *  - the same files (path, size, modification time) for every image loaded into the team that belongs to the
 *    GL/Vulkan driver stack (libEGL, libGL, Mesa/gallium, Vulkan loader and ICDs, ...), after EGL and the GL
 *    context are up,
 *  - the same Vulkan ICD manifests named in VK_DRIVER_FILES / VK_ICD_FILENAMES,
 *  - the same kernel graphics drivers (the entries of add-ons/kernel/drivers/dev/graphics; a driver fix in the
 *    kernel part of a stack must re-enable the hardware path too).
 * Installing any update of these changes the identity; that is the point: an updated driver gets a new chance.
 *
 * The parts are sorted and joined with '\n' (no trailing newline), so the text is independent of load order;
 * the compact id is the 64-bit FNV-1a hash of that text. Header-only (static functions), plain libc + the
 * Haiku kernel kit, no libobs, so the probe can use it too.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

#ifdef __HAIKU__
#include <image.h>
#endif

#define GL_IDENTITY_MAX 8192
#define GL_IDENTITY_MAX_PARTS 160

#ifdef __HAIKU__
/* lower-case substrings of an image path that mark it as part of the GL/Vulkan driver stack */
static const char *const gl_identity_patterns[] = {
	"/vulkan", "libvulkan", "libegl", "libgl", "/dri/", "mesa", "gallium",
	"zink",    "nouveau",   "nvk",    "nvidia", "glvnd",
};

/* kernel graphics drivers publish themselves here (symlinks into ../../bin) */
static const char *const gl_identity_kernel_dirs[] = {
	"/boot/system/add-ons/kernel/drivers/dev/graphics",
	"/boot/system/non-packaged/add-ons/kernel/drivers/dev/graphics",
	"/boot/home/config/non-packaged/add-ons/kernel/drivers/dev/graphics",
};
#endif

struct gl_identity_parts {
	char *part[GL_IDENTITY_MAX_PARTS];
	size_t count;
};

static bool gl_identity_contains_ci(const char *s, const char *needle)
{
	const size_t len = strlen(needle);
	for (; *s; s++) {
		if (strncasecmp(s, needle, len) == 0)
			return true;
	}
	return false;
}

#ifdef __HAIKU__
static bool gl_identity_driver_path(const char *path)
{
	for (size_t i = 0; i < sizeof(gl_identity_patterns) / sizeof(gl_identity_patterns[0]); i++) {
		if (gl_identity_contains_ci(path, gl_identity_patterns[i]))
			return true;
	}
	return false;
}
#endif

static void gl_identity_add(struct gl_identity_parts *parts, const char *text)
{
	if (parts->count >= GL_IDENTITY_MAX_PARTS)
		return;
	char *copy = strdup(text);
	if (!copy)
		return;
	/* parts are lines */
	for (char *c = copy; *c; c++) {
		if (*c == '\n' || *c == '\r')
			*c = ' ';
	}
	parts->part[parts->count++] = copy;
}

/* stat() follows symlinks: a link into the driver directory stands for the driver binary */
static void gl_identity_add_file(struct gl_identity_parts *parts, const char *what, const char *path)
{
	char line[1280];
	struct stat st;
	if (stat(path, &st) == 0)
		snprintf(line, sizeof(line), "%s %s (size %lld, modified %lld)", what, path, (long long)st.st_size,
			 (long long)st.st_mtime);
	else
		snprintf(line, sizeof(line), "%s %s (missing)", what, path);
	gl_identity_add(parts, line);
}

/* a ':'-separated list of files, as the Vulkan loader takes them */
static void gl_identity_add_file_list(struct gl_identity_parts *parts, const char *list)
{
	if (!list || !*list)
		return;
	char *copy = strdup(list);
	if (!copy)
		return;
	char *save = NULL;
	for (char *path = strtok_r(copy, ":", &save); path; path = strtok_r(NULL, ":", &save)) {
		if (*path)
			gl_identity_add_file(parts, "icd", path);
	}
	free(copy);
}

#ifdef __HAIKU__
static void gl_identity_add_dir(struct gl_identity_parts *parts, const char *dir_path)
{
	DIR *dir = opendir(dir_path);
	if (!dir)
		return;
	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL) {
		if (entry->d_name[0] == '.')
			continue;
		char path[1024];
		snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name);
		gl_identity_add_file(parts, "kernel", path);
	}
	closedir(dir);
}
#endif

static int gl_identity_compare(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

/*
 * Builds the identity text into text (size bytes, GL_IDENTITY_MAX recommended). Call with an OpenGL context
 * current, after EGL initialisation, so the whole driver stack is loaded. Returns the length of the text.
 */
static size_t gl_identity_build(char *text, size_t size, const char *renderer, const char *version)
{
	struct gl_identity_parts parts = {0};
	char line[1280];

#ifdef __HAIKU__
	image_info image;
	int32 cookie = 0;
	while (get_next_image_info(B_CURRENT_TEAM, &cookie, &image) == B_OK) {
		if (image.type != B_APP_IMAGE && gl_identity_driver_path(image.name))
			gl_identity_add_file(&parts, "image", image.name);
	}
	for (size_t i = 0; i < sizeof(gl_identity_kernel_dirs) / sizeof(gl_identity_kernel_dirs[0]); i++)
		gl_identity_add_dir(&parts, gl_identity_kernel_dirs[i]);
#endif
	gl_identity_add_file_list(&parts, getenv("VK_DRIVER_FILES"));
	gl_identity_add_file_list(&parts, getenv("VK_ICD_FILENAMES"));

	qsort(parts.part, parts.count, sizeof(parts.part[0]), gl_identity_compare);

	size_t len = 0;
	if (size)
		text[0] = '\0';
	snprintf(line, sizeof(line), "GL_RENDERER=%s\nGL_VERSION=%s", renderer ? renderer : "?",
		 version ? version : "?");
	for (char *c = line; *c; c++) {
		if (*c == '\r')
			*c = ' ';
	}
	if (size) {
		snprintf(text, size, "%s", line);
		len = strlen(text);
	}

	for (size_t i = 0; i < parts.count; i++) {
		/* the same image can be listed twice (e.g. loaded under two names) */
		if (i == 0 || strcmp(parts.part[i], parts.part[i - 1]) != 0) {
			if (len + 1 < size) {
				snprintf(text + len, size - len, "\n%s", parts.part[i]);
				len += strlen(text + len);
			}
		}
	}
	for (size_t i = 0; i < parts.count; i++)
		free(parts.part[i]);
	return len;
}

/* 64-bit FNV-1a */
static uint64_t gl_identity_hash(const char *text)
{
	uint64_t hash = 0xcbf29ce484222325ull;
	for (const unsigned char *c = (const unsigned char *)text; *c; c++) {
		hash ^= *c;
		hash *= 0x100000001b3ull;
	}
	return hash;
}
