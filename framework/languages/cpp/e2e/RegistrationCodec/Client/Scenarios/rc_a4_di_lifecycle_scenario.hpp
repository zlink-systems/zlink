/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/client_support.hpp"

#include <iostream>

namespace zlink::framework::e2e::registration_codec::client
{

inline void run_di_lifecycle_scenario (const client_options_t &options)
{
    const auto reply = post_empty<lifecycle_scenario_res_t> (
      options.http_endpoint, "/registration/di-lifecycle");
    const auto first_reply = reply.first;
    const auto second_reply = reply.second;
    ensure (first_reply.scoped_id != second_reply.scoped_id,
            "RC-A4 scoped dependency was reused");
    ensure (first_reply.singleton_id == second_reply.singleton_id,
            "RC-A4 singleton dependency was recreated");
    ensure (second_reply.destroyed_before >= first_reply.destroyed_before + 1,
            "RC-A4 scoped dependency was not destroyed after request");
    ensure (reply.stats.destroyed_count >= 2, "RC-A4 scoped dependency destruction count mismatch");
    std::cout << "scenario RC-A4 passed\n";
}

} // namespace zlink::framework::e2e::registration_codec::client
