/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../../Shared/resilience_lifecycle_contracts.hpp"
#include "client_options.hpp"
#include "lifecycle_api_result.hpp"
#include "scenario_assert.hpp"
#include "topology_entry_result.hpp"

namespace zlink::framework::e2e::resilience_lifecycle::client
{

using resilience_lifecycle::api_channel;
using resilience_lifecycle::dealer_channel;
using resilience_lifecycle::evidence_entry_t;
using resilience_lifecycle::evidence_snapshot_t;
using resilience_lifecycle::handler_group;
using resilience_lifecycle::payload_req_t;
using resilience_lifecycle::payload_res_t;
using resilience_lifecycle::profile_msg_t;
using resilience_lifecycle::profile_req_t;
using resilience_lifecycle::profile_res_t;
using resilience_lifecycle::route_channel;
using resilience_lifecycle::scenario_route_req_t;
using resilience_lifecycle::scenario_route_res_t;

using resilience_lifecycle::client::any_provider_evidence_contains;
using resilience_lifecycle::client::client_options_t;
using resilience_lifecycle::client::ensure;
using resilience_lifecycle::client::evidence_contains;
using resilience_lifecycle::client::fetch_evidence;
using resilience_lifecycle::client::post_consumer_command;
using resilience_lifecycle::client::post_consumer_missing;
using resilience_lifecycle::client::post_consumer_profile;
using resilience_lifecycle::client::post_consumer_profile_raw;
using resilience_lifecycle::client::read_client_options;
using resilience_lifecycle::client::topology_entry_result_t;
using resilience_lifecycle::client::touch_file;
using resilience_lifecycle::client::wait_for_file;
using resilience_lifecycle::client::wait_provider_evidence_contains;

} // namespace zlink::framework::e2e::resilience_lifecycle::client
