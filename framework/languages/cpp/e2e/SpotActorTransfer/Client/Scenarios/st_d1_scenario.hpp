/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_runner_support.hpp"

namespace
{

/* ST-D1: this file owns the scenario orchestration and its public assertions. */
inline void scenario_runner_t::run_st_d1_scenario ()
{
    local_location_commit_timing ();
    remote_location_commit_timing ();
}

} // namespace
