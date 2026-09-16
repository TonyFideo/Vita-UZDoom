#pragma once

#include <stddef.h>

// Keep Vita paths and the native display size in one place.  The runtime
// implementation lives in this directory so desktop POSIX behavior remains
// unchanged.
#define UZDOOM_VITA_PROGRAM_ROOT "app0:/"
#define UZDOOM_VITA_DATA_ROOT "ux0:/data/uzdoom"
#define UZDOOM_VITA_DISPLAY_WIDTH 960
#define UZDOOM_VITA_DISPLAY_HEIGHT 544

// The first Vita milestone renders the software scene into a small logical
// canvas and lets the GLES2/VitaGL presenter scale it to the native display.
// This keeps the CPU renderer close to the resolution used by the PRBoom+
// Vita port while retaining the full 960x544 output surface.
#define UZDOOM_VITA_SOFTWARE_WIDTH 480
#define UZDOOM_VITA_SOFTWARE_HEIGHT 272

// The desktop GLES renderer reserves a deliberately generous two-million
// vertex stream.  That is far beyond what the Vita hardware path needs for
// the first porting milestone and consumes roughly 128 MiB with the default
// two pipelines.  Keep the Vita budget explicit so it can be tuned after a
// real-map stress test instead of silently inheriting the desktop reserve.
#define UZDOOM_VITA_HW_VERTEX_BUFFER_SIZE 65536
#define UZDOOM_VITA_HW_LIGHT_BUFFER_ENTRIES 8192
#define UZDOOM_VITA_HW_BONE_BUFFER_ENTRIES 8192
#define UZDOOM_VITA_HW_DEFAULT_PIPELINE_DEPTH 2

// This is a diagnostic ceiling for the on-screen Vita overlay. It is not a
// request for a 350 MiB application memory budget from the Vita loader.
#define UZDOOM_VITA_MEMORY_LIMIT_MB 350

namespace VitaPlatform
{
	constexpr size_t MemoryLimitBytes = size_t(UZDOOM_VITA_MEMORY_LIMIT_MB) * 1024 * 1024;

	// Capture the free-memory baseline before the engine allocates its large
	// resource tables. The resulting value is only an estimate of application
	// usage, but is useful for spotting leaks and memory pressure on Vita.
	void InitializeMemoryStats();
	size_t GetMemoryUsageBytes();
}
