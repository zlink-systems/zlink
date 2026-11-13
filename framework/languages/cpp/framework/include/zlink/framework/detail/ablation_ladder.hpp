/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

// #4350 experiment only. This opt-in and its callers stay on this branch.
#if defined(ZLINK_CPP_ABLATION_LADDER)
#include <cstdlib>
#include <stdexcept>
namespace zlink::framework::detail::ablation
{
// Restoration order: permit, envelope, service framing, typed codec,
// owner queue/worker, activation/scope, registry/completion/deadline.
// L0/L1 and the unchanged main path are executable in this job.
inline int level ()
{
    static const int value = [] {
        const auto *input = std::getenv ("ZLINK_CPP_LADDER_LEVEL");
        const int parsed = input ? std::atoi (input) : 8;
        if (parsed != 0 && parsed != 1 && parsed != 8)
            throw std::invalid_argument ("this step implements only L0 (0), L1 (1) and main (8)");
        return parsed;
    } ();
    return value;
}
}
#endif
