/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_runner_support.hpp"

namespace
{

/* ST-F6: this file owns the scenario orchestration and its public assertions. */
inline void scenario_runner_t::run_st_f6_scenario ()
{
    in_flight_request_correlation ();
    in_flight_request_timeout ();
}

} // namespace
