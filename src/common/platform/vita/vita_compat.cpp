/*
** Vita C/POSIX compatibility helpers.
**
** VitaSDK exposes the declarations for a few POSIX helpers used by the
** portable engine, but its newlib archive does not provide all of them.
** Keep the small replacements here so the engine sources remain shared with
** the other POSIX targets.
*/

#include <errno.h>
#include <fnmatch.h>
#include <malloc.h>
#include <stddef.h>

#include <psp2/kernel/sysmem.h>

#include "vita_platform.h"

namespace VitaPlatform
{
	namespace
	{
		int InitialFreeUserMemory = 0;
	}

	void InitializeMemoryStats()
	{
		SceKernelFreeMemorySizeInfo info{};
		info.size = sizeof(info);
		InitialFreeUserMemory = sceKernelGetFreeMemorySize(&info) >= 0 ? info.size_user : 0;
	}

	size_t GetMemoryUsageBytes()
	{
		const struct mallinfo heap = mallinfo();
		const size_t heapUsage = heap.uordblks;

		SceKernelFreeMemorySizeInfo info{};
		info.size = sizeof(info);
		if (InitialFreeUserMemory > 0 && sceKernelGetFreeMemorySize(&info) >= 0 &&
			info.size_user < InitialFreeUserMemory)
		{
			const size_t memoryDelta = size_t(InitialFreeUserMemory - info.size_user);
			return heapUsage > memoryDelta ? heapUsage : memoryDelta;
		}

		return heapUsage;
	}
}

namespace
{
	// UZDoom uses fnmatch with FNM_NOESCAPE for directory scans. Support the
	// wildcard forms needed by that API without depending on a shell library.
	bool MatchPattern(const char *pattern, const char *value, int flags)
	{
		while (*pattern != '\0')
		{
			if (*pattern == '*')
			{
				do
				{
					++pattern;
				} while (*pattern == '*');

				if (*pattern == '\0') return true;
				for (const char *candidate = value; ; ++candidate)
				{
					if (MatchPattern(pattern, candidate, flags)) return true;
					if (*candidate == '\0') return false;
				}
			}

			if (*value == '\0') return false;

			if (*pattern == '?')
			{
				++pattern;
				++value;
				continue;
			}

			if (*pattern == '\\' && !(flags & FNM_NOESCAPE))
			{
				++pattern;
				if (*pattern == '\0') return false;
			}

			if (*pattern != *value) return false;
			++pattern;
			++value;
		}

		return *value == '\0';
	}
}

extern "C" int fnmatch(const char *pattern, const char *value, int flags)
{
	return MatchPattern(pattern, value, flags) ? 0 : FNM_NOMATCH;
}

extern "C" int posix_memalign(void **memptr, size_t alignment, size_t size)
{
	if (memptr == nullptr || alignment < sizeof(void *) ||
		(alignment & (alignment - 1)) != 0)
		return EINVAL;

	void *memory = memalign(alignment, size);
	if (memory == nullptr)
		return ENOMEM;

	*memptr = memory;
	return 0;
}
