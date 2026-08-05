/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_runner_support.hpp"

namespace
{

/* ST-C3: this file owns the scenario orchestration and its public assertions. */
inline void scenario_runner_t::run_st_c3_scenario ()
{
    transfer_out_failure ();
    source_leave_failure ();
    transfer_in_failure ();
    joined_failure ();
}

} // namespace
