/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_UTILS_DEBUG_LOG_HPP_INCLUDED__
#define __ZLINK_UTILS_DEBUG_LOG_HPP_INCLUDED__

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace zlink
{
inline bool debug_env_enabled (const char *name_)
{
    return name_ && std::getenv (name_) != NULL;
}

inline void debug_vfprintf (const char *env_, const char *prefix_, const char *fmt_, va_list args_)
{
    if (env_ && !debug_env_enabled (env_))
        return;
    if (prefix_)
        std::fprintf (stderr, "%s", prefix_);
    std::vfprintf (stderr, fmt_, args_);
    std::fprintf (stderr, "\n");
    std::fflush (stderr);
}

inline void debug_vfprintf_file (const char *path_, const char *fmt_, va_list args_)
{
    if (!path_ || path_[0] == '\0')
        return;

    FILE *fp = std::fopen (path_, "a");
    if (!fp)
        return;

    va_list file_args;
    va_copy (file_args, args_);
    std::vfprintf (fp, fmt_, file_args);
    std::fprintf (fp, "\n");
    va_end (file_args);
    std::fclose (fp);
}

inline void debug_vfprintf_with_file (
  const char *env_, const char *prefix_, const char *path_, const char *fmt_, va_list args_)
{
    if (env_ && !debug_env_enabled (env_))
        return;

    va_list stderr_args;
    va_copy (stderr_args, args_);
    if (prefix_)
        std::fprintf (stderr, "%s", prefix_);
    std::vfprintf (stderr, fmt_, stderr_args);
    std::fprintf (stderr, "\n");
    std::fflush (stderr);
    va_end (stderr_args);

    debug_vfprintf_file (path_, fmt_, args_);
}

inline void debug_fprintf (const char *env_, const char *prefix_, const char *fmt_, ...)
{
    va_list args;
    va_start (args, fmt_);
    debug_vfprintf (env_, prefix_, fmt_, args);
    va_end (args);
}

inline void debug_fprintf_with_file (
  const char *env_, const char *prefix_, const char *path_, const char *fmt_, ...)
{
    va_list args;
    va_start (args, fmt_);
    debug_vfprintf_with_file (env_, prefix_, path_, fmt_, args);
    va_end (args);
}
}

#endif
