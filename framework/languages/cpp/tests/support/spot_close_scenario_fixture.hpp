/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include <nlohmann/json_fwd.hpp>
#include <set>
#include <string>

namespace zlink::framework::test::spot_close
{
const nlohmann::json &spot_close_fixture ();
const std::set<std::string> &known_scenarios ();
nlohmann::json run_scenario (const nlohmann::json &scenario, std::string &failure);
void run_and_check (const nlohmann::json &scenario);
} // namespace zlink::framework::test::spot_close
