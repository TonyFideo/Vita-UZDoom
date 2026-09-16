// Small C/POSIX compatibility layer for the Vita newlib environment.
#pragma once

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

// CCTZ uses this feature level to expose strptime() from <time.h>.
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 500
#endif

#include <strings.h>

#ifndef stricmp
#define stricmp strcasecmp
#endif

#ifndef strnicmp
#define strnicmp strncasecmp
#endif
