/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "../Shared/automatic_turn_dispatch_contracts.hpp"
#include "Scenarios/atd_a1_basic_terminator_scenario.hpp"
#include "Scenarios/atd_a2_await_terminator_scenario.hpp"
#include "Scenarios/atd_a3_continuation_context_scenario.hpp"
#include "Scenarios/atd_a4_worker_await_scenario.hpp"
#include "Scenarios/atd_b1_other_actor_progress_scenario.hpp"
#include "Scenarios/atd_b2_same_actor_reentry_scenario.hpp"
#include "Scenarios/atd_b3_actor_join_await_scenario.hpp"
#include "Scenarios/atd_c1_timer_isolation_scenario.hpp"
#include "Scenarios/atd_c2_timer_reentry_scenario.hpp"
#include "Scenarios/atd_c3_actor_timer_isolation_scenario.hpp"
#include "Scenarios/atd_d2_remote_spot_await_scenario.hpp"
#include "Scenarios/atd_d3_route_bridge_await_scenario.hpp"
#include "Scenarios/atd_d4_session_relay_actor_await_scenario.hpp"
#include "Scenarios/atd_e1_timeout_scenario.hpp"
#include "Scenarios/td_e2_user_to_user_spot_join_scenario.hpp"
#include "Scenarios/td_e3_opposite_spot_join_scenario.hpp"
#include "Scenarios/td_c1_http_yield_interleave_scenario.hpp"
#include "Scenarios/td_c2_http_async_exclusion_scenario.hpp"
#include "Scenarios/td_c3_io_worker_capacity_scenario.hpp"
#include "Scenarios/shutdown_await_scenario.hpp"
#include "Scenarios/await_actor_scenario_context.hpp"
#include "Support/client_options.hpp"
#include "Support/scenario_assert.hpp"

#include <zlink/stream_connector.hpp>

#include <chrono>
#include <iostream>
#include <string>

namespace yd = zlink::framework::e2e::automatic_turn_dispatch;
namespace atd_client = zlink::framework::e2e::automatic_turn_dispatch::client;

namespace
{

using atd_client::ensure;
using atd_client::ensure_result;
using atd_client::unique_id;

} // namespace

int main (int argc, char **argv)
{
    try {
        const auto client_options = atd_client::parse_client_options (argc, argv);
        ensure (!client_options.session_a_stream_endpoint.empty (),
                "sessionAStreamEndpoint is required");
        ensure (!client_options.session_b_stream_endpoint.empty (),
                "sessionBStreamEndpoint is required");
        const auto scenario =
          client_options.scenario.empty () ? std::string ("full") : client_options.scenario;
        if (scenario == "shutdown-wait") {
            ensure (!client_options.request_id.empty (),
                    "requestId is required for shutdown-wait");
            ensure (!client_options.spot_id.empty (),
                    "spotId is required for shutdown-wait");
            atd_client::run_shutdown_wait_scenario (client_options);
            return 0;
        }
        if (scenario == "shutdown-recovery") {
            ensure (!client_options.request_id.empty (),
                    "requestId is required for shutdown-recovery");
            ensure (!client_options.spot_id.empty (),
                    "spotId is required for shutdown-recovery");
            atd_client::run_shutdown_recovery_scenario (client_options);
            return 0;
        }
        auto wants = [&scenario] (const char *id) {
            return scenario == "all" || scenario == "full" || scenario == id;
        };
        ensure (scenario == "all" || scenario == "full" || scenario.rfind ("atd-", 0) == 0
                  || scenario.rfind ("td-", 0) == 0,
                "unknown AutomaticTurnDispatch client scenario: " + scenario);

        auto options = atd_client::make_connector_options (client_options);
        auto client = zlink::stream_connector::connector_factory_t::create (options);
        auto connected = client.connect ();
        ensure (static_cast<bool> (connected), "AutomaticTurnDispatch stream connect failed");
        auto observer = zlink::stream_connector::connector_factory_t::create (options);
        auto observer_connected = observer.connect ();
        ensure (static_cast<bool> (observer_connected),
                "AutomaticTurnDispatch observer stream connect failed");

        const auto spot_id = unique_id ("await-track-a");
        auto spot =
          client.request (yd::ensure_spot_req_t{.spot_id = spot_id})
            .packet_name (yd::ensure_spot_req_t::packet_name)
            .timeout (std::chrono::milliseconds (15000))
            .submit<yd::ensure_spot_res_t> ();
        ensure_result (spot, "ATD-A ensure spot request failed");
        ensure (spot.value ().spot_id == spot_id, "ATD-A ensure spot reply mismatch");
        const atd_client::await_actor_scenario_context_t actors{
          .spot_id = spot_id, .actor_a = unique_id ("actor-a"), .actor_b = unique_id ("actor-b")};
        auto bound_actors =
          client.request (yd::bind_await_actors_req_t{.spot_id = actors.spot_id,
                                                      .actor_ids = {actors.actor_a, actors.actor_b}})
            .packet_name (yd::bind_await_actors_req_t::packet_name)
            .timeout (std::chrono::milliseconds (15000))
            .submit<yd::bind_await_actors_res_t> ();
        ensure (static_cast<bool> (bound_actors), "ATD-B bind actors failed");
        ensure (bound_actors.value ().actors.size () == 2, "ATD-B bind actor count mismatch");
        auto observer_bound_actors =
          observer.request (
                    yd::bind_await_actors_req_t{.spot_id = actors.spot_id,
                                                .actor_ids = {actors.actor_a, actors.actor_b}})
            .packet_name (yd::bind_await_actors_req_t::packet_name)
            .timeout (std::chrono::milliseconds (15000))
            .submit<yd::bind_await_actors_res_t> ();
        ensure (static_cast<bool> (observer_bound_actors),
                "ATD-B observer bind actors failed");
        if (wants ("atd-a1")) {
            atd_client::run_atd_a1_basic_terminator_scenario (client, spot_id);
        }
        if (wants ("atd-a2")) {
            atd_client::run_atd_a2_await_terminator_scenario (client, observer, spot_id);
        }
        if (wants ("atd-a3")) {
            atd_client::run_atd_a3_continuation_context_scenario (client, observer, spot_id);
        }
        if (wants ("atd-a4")) {
            atd_client::run_atd_a4_worker_await_scenario (client, observer, spot_id);
        }

        if (wants ("atd-b1")) {
            atd_client::run_atd_b1_other_actor_progress_scenario (client, actors);
        }
        if (wants ("atd-b2")) {
            atd_client::run_atd_b2_same_actor_reentry_scenario (client, observer, actors);
        }
        if (wants ("atd-b3")) {
            atd_client::run_atd_b3_actor_join_await_scenario (client, actors);
        }
        if (wants ("td-e2")) {
            atd_client::run_td_e2_user_to_user_spot_join_scenario (client, actors);
        }
        if (wants ("td-e3")) {
            atd_client::run_td_e3_opposite_spot_join_scenario (client, actors);
        }
        auto rebound_actors =
          client.request (yd::bind_await_actors_req_t{.spot_id = actors.spot_id,
                                                      .actor_ids = {actors.actor_a, actors.actor_b}})
            .packet_name (yd::bind_await_actors_req_t::packet_name)
            .timeout (std::chrono::milliseconds (15000))
            .submit<yd::bind_await_actors_res_t> ();
        ensure (static_cast<bool> (rebound_actors), "ATD-C actor rebind failed");

        const auto timer_spot_id = unique_id ("await-timer");
        auto timer_spot =
          client.request (yd::ensure_spot_req_t{.spot_id = timer_spot_id})
            .packet_name (yd::ensure_spot_req_t::packet_name)
            .timeout (std::chrono::milliseconds (15000))
            .submit<yd::ensure_spot_res_t> ();
        ensure (static_cast<bool> (timer_spot), "ATD-C ensure timer spot request failed");
        ensure (timer_spot.value ().spot_id == timer_spot_id,
                "ATD-C ensure timer spot reply mismatch");

        if (wants ("atd-c1")) {
            atd_client::run_atd_c1_timer_isolation_scenario (client, observer, timer_spot_id);
        }
        if (wants ("atd-c2")) {
            atd_client::run_atd_c2_timer_reentry_scenario (client, observer, timer_spot_id);
        }
        if (wants ("atd-c3")) {
            atd_client::run_atd_c3_actor_timer_isolation_scenario (client, actors);
        }
        if (wants ("td-c1")) {
            atd_client::run_td_c1_http_yield_interleave_scenario (
              client, observer, timer_spot_id);
        }
        if (wants ("td-c2")) {
            atd_client::run_td_c2_http_async_exclusion_scenario (
              client, observer, timer_spot_id);
        }
        if (wants ("td-c3")) {
            atd_client::run_td_c3_io_worker_capacity_scenario (
              client, observer, timer_spot_id);
        }

        if (wants ("atd-d2")) {
            atd_client::run_atd_d2_remote_spot_await_scenario (client);
        }
        if (wants ("atd-d3")) {
            atd_client::run_atd_d3_route_bridge_await_scenario (client);
        }
        if (wants ("atd-d4")) {
            atd_client::run_atd_d4_session_relay_actor_await_scenario (
              client, client_options.session_b_stream_endpoint, actors);
        }

        if (wants ("atd-e1")) {
            atd_client::run_atd_e1_timeout_scenario (client);
        }

        (void) observer.close ();
        (void) client.close ();
        std::cout << "automatic-turn-dispatch track-a-e1 result=passed\n";
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << "automatic-turn-dispatch client failed: " << error.what () << "\n";
        return 1;
    }
}
