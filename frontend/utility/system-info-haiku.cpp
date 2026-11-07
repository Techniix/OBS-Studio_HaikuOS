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

/* System capabilities reported to multitrack video (Enhanced Broadcasting) on Haiku. */

#include "system-info.hpp"
#include "platform.hpp"

#include <util/platform.h>

#include <sys/utsname.h>

#include <cstring>
#include <string>
#include <vector>

/* Note: <OS.h> cannot be included here, its system_info struct clashes with system_info() below. */

#if defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#endif

namespace {

std::optional<std::string> get_cpu_name()
{
#if defined(__i386__) || defined(__x86_64__)
	unsigned int regs[12] = {};
	if (__get_cpuid_max(0x80000000, nullptr) < 0x80000004)
		return std::nullopt;

	__get_cpuid(0x80000002, &regs[0], &regs[1], &regs[2], &regs[3]);
	__get_cpuid(0x80000003, &regs[4], &regs[5], &regs[6], &regs[7]);
	__get_cpuid(0x80000004, &regs[8], &regs[9], &regs[10], &regs[11]);

	char name[sizeof(regs) + 1] = {};
	memcpy(name, regs, sizeof(regs));

	std::string result(name);
	const auto first = result.find_first_not_of(' ');
	const auto last = result.find_last_not_of(' ');
	if (first == std::string::npos)
		return std::nullopt;
	return result.substr(first, last - first + 1);
#else
	return std::nullopt;
#endif
}

} // namespace

void system_info(GoLiveApi::Capabilities &capabilities)
{
	/* No GPU enumeration API is exposed to applications on Haiku yet. */
	capabilities.gpu = std::nullopt;

	auto &cpu_data = capabilities.cpu;
	cpu_data.physical_cores = os_get_physical_cores();
	cpu_data.logical_cores = os_get_logical_cores();
	cpu_data.name = get_cpu_name().value_or("Unknown");
	cpu_data.speed = HaikuGetCpuFrequencyMHz();

	auto &memory_data = capabilities.memory;
	memory_data.total = os_get_sys_total_size();
	memory_data.free = os_get_sys_free_size();

	auto &system_data = capabilities.system;
	system_data.name = "Haiku";
	system_data.release = "unknown";
	system_data.build = 0;
	system_data.bits = sizeof(void *) == 8 ? 64 : 32;
	system_data.arm = false;
	system_data.armEmulation = false;

	struct utsname utsinfo;
	if (uname(&utsinfo) == 0) {
		/* release is "1" for R1; version carries the hrev, e.g. "hrev58000 Oct  1 2026 06:00:00" */
		system_data.release = utsinfo.release;
		system_data.version = std::string(utsinfo.sysname) + " " + utsinfo.release + " " + utsinfo.version;
		system_data.arm = strstr(utsinfo.machine, "arm") != nullptr || strstr(utsinfo.machine, "aarch") != nullptr;

		std::string version(utsinfo.version);
		system_data.revision = version.substr(0, version.find(' '));

		if (system_data.version.size() > 128)
			system_data.version.resize(128);
	} else {
		system_data.version = "unknown";
	}
}
