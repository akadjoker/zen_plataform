#ifndef WIN32_UTIL_H
#define WIN32_UTIL_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "platform.h"

bool win32_widen(const char *utf8, wchar_t *out, size_t cap);
/* win32_widen for a path that goes to a file function: from LONG_PATH_FROM wide characters up (the file functions stop at
   MAX_PATH, 260, less what they append) the result is the absolute, cleaned-up path with the \\?\ prefix (\\?\UNC\ for a
   network path), the one form that works with no opt-in by the program or the machine. Shorter paths are left as they are. */
#define LONG_PATH_FROM 240
bool win32_widen_path(const char *utf8, wchar_t *out, size_t cap);
bool win32_narrow(const wchar_t *wide, char *out, size_t cap);
bool win32_error(const char *what, const char *path);
void win32_slashes(char *path);

#endif /* WIN32_UTIL_H */
