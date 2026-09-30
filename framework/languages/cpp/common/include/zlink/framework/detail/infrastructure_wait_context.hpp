/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#ifdef ZLINK_FRAMEWORK_DEBUG_WAIT_GUARD
#include <cstdio>
#include <cstdlib>

namespace zlink::framework::detail
{
inline thread_local const void *current_infrastructure_wait_owner = nullptr;

[[noreturn]] inline void fail_infrastructure_wait (const char *site) noexcept
{
    std::fprintf (stderr, "infrastructure wait guard: %s\n", site);
    std::fflush (stderr);
    std::abort ();
}

inline void check_infrastructure_wait (bool ready, const char *site) noexcept
{
    if (current_infrastructure_wait_owner && !ready)
        fail_infrastructure_wait (site);
}
} // namespace zlink::framework::detail
#endif
