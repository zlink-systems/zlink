#ifndef PERF_MULTI_ENTRY_HPP
#define PERF_MULTI_ENTRY_HPP

#include <cstdlib>

namespace perf
{
namespace multi
{

inline void set_perf_pattern_env (const char *pattern)
{
    if (!pattern || !*pattern)
        return;
#if defined(_WIN32)
    _putenv_s ("PERF_PATTERN", pattern);
    _putenv_s ("PERF_MULTI_PATTERN", pattern);
#else
    setenv ("PERF_PATTERN", pattern, 1);
    setenv ("PERF_MULTI_PATTERN", pattern, 1);
#endif
}

} // namespace multi
} // namespace perf

#endif
