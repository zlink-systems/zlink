/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/automatic_turn_dispatch_contracts.hpp"
#include "../Support/client_options.hpp"
#include "../Support/scenario_assert.hpp"

#include <zlink/stream_connector.hpp>

#include <chrono>
#include <iostream>
#include <string>

namespace zlink::framework::e2e::automatic_turn_dispatch::client
{

inline const char *shutdown_error_name (zlink::stream_connector::error_code_t code)
{
    switch (code) {
    case zlink::stream_connector::error_code_t::disconnected:
        return "disconnected";
    case zlink::stream_connector::error_code_t::request_timeout:
        return "request_timeout";
    case zlink::stream_connector::error_code_t::remote_error:
        return "remote_error";
    case zlink::stream_connector::error_code_t::closed:
        return "closed";
    case zlink::stream_connector::error_code_t::canceled:
        return "canceled";
    default:
        return "other";
    }
}

inline void run_shutdown_wait_scenario (const client_options_t &client_options)
{
    auto connector_options = make_connector_options (client_options);
    connector_options.request_timeout = std::chrono::milliseconds (3000);
    auto client = zlink::stream_connector::connector_factory_t::create (connector_options);
    auto connected = client.connect ();
    ensure (static_cast<bool> (connected), "ATD-E3 shutdown wait stream connect failed");

    auto result =
      client.request (await_shutdown_scenario_req_t{.request_id = client_options.request_id,
                                                    .spot_id = client_options.spot_id,
                                                    .delay_ms = 30000})
        .packet_name (await_shutdown_scenario_req_t::packet_name)
        .timeout (std::chrono::milliseconds (3000))
        .submit<await_scenario_res_t> ();
    (void) client.close ();
    ensure (!static_cast<bool> (result),
            "ATD-E3 expected play-a shutdown while await was pending");
    ensure (result.error_code () != zlink::stream_connector::error_code_t::request_timeout,
            "ATD-E3 shutdown wait must observe closed/cancelled error before request timeout");
    std::cout << "automatic-turn-dispatch shutdown wait result=passed error="
              << shutdown_error_name (result.error_code ()) << "\n";
}

inline void run_shutdown_recovery_scenario (const client_options_t &client_options)
{
    auto client = zlink::stream_connector::connector_factory_t::create (
      make_connector_options (client_options));
    auto connected = client.connect ();
    ensure (static_cast<bool> (connected), "ATD-E3 shutdown recovery stream connect failed");

    auto result =
      client.request (
              await_shutdown_recovery_req_t{.request_id = client_options.request_id,
                                            .spot_id = client_options.spot_id})
        .packet_name (await_shutdown_recovery_req_t::packet_name)
        .timeout (std::chrono::milliseconds (30000))
        .submit<await_scenario_res_t> ();
    (void) client.close ();
    ensure (static_cast<bool> (result), "ATD-E3 shutdown recovery request failed");
    ensure (result.value ().operation == "await.e3-shutdown-recovery",
            "ATD-E3 recovery operation mismatch");
    ensure (result.value ().spot_id == client_options.spot_id,
            "ATD-E3 recovery spot id mismatch");
    bool saw_probe = false;
    for (const auto &line : result.value ().evidence) {
        if (line.find ("request=" + client_options.request_id) != std::string::npos
            && line.find ("probe-completed") != std::string::npos
            && line.find ("marker=shutdown-recovery-probe") != std::string::npos) {
            saw_probe = true;
        }
    }
    ensure (saw_probe, "ATD-E3 recovery probe marker missing");
    std::cout << "automatic-turn-dispatch shutdown recovery result=passed\n";
}

} // namespace zlink::framework::e2e::automatic_turn_dispatch::client
