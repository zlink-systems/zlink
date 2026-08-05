/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/spot_service_contracts.hpp"

#include <zlink/stream_connector.hpp>
#include <zlink/stream_e2e_client.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace zlink::framework::e2e::spot_service::client::scenarios
{

template <typename TResult> std::string sm_g1_stream_error_text (const TResult &result)
{
    if (result.error ()) {
        return result.error ()->message;
    }
    return "unknown stream error";
}

inline zlink::stream_connector::connector_t sm_g1_make_stream_connector (
  const std::string &stream_endpoint)
{
    zlink::stream_connector::connector_options_t options;
    options.endpoint = stream_endpoint;
    options.connect_timeout = std::chrono::milliseconds (3000);
    options.request_timeout = std::chrono::milliseconds (3000);
    options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
    return zlink::stream_connector::connector_factory_t::create (options);
}

inline void sm_g1_write_signal_file (const std::string &path)
{
    if (path.empty ()) {
        throw std::runtime_error ("SM-G1 signal path is empty");
    }
    std::ofstream file (path);
    file << "ready\n";
}

inline void sm_g1_wait_signal_file (const std::string &path, const std::string &label)
{
    if (path.empty ()) {
        throw std::runtime_error ("SM-G1 " + label + " signal path is empty");
    }
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (3);
    while (std::chrono::steady_clock::now () < deadline) {
        if (std::filesystem::exists (path)) {
            return;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }
    throw std::runtime_error ("SM-G1 timed out waiting for " + label);
}

inline void run_sm_g1_crash_observation_scenario (const std::string &stream_endpoint,
                                                  const std::string &alternate_stream_endpoint,
                                                  const std::string &crash_ready_file,
                                                  const std::string &crash_go_file,
                                                  const std::string &crash_observed_file)
{
    if (stream_endpoint.empty () || alternate_stream_endpoint.empty ()) {
        throw std::runtime_error (
          "streamEndpoint and alternateStreamEndpoint are required for SM-G1");
    }
    const auto play_a_actor_id = std::string ("actor-sm-g1-crash");
    const auto play_b_actor_id = std::string ("actor-sm-g1-survivor");

    auto play_a_core = sm_g1_make_stream_connector (stream_endpoint);
    auto play_a_stream = zlink::stream_e2e_client::use (play_a_core);
    auto play_a_connected = play_a_stream.connect ().submit ();
    if (!static_cast<bool> (play_a_connected)) {
        throw std::runtime_error ("SM-G1 play-a stream connect failed");
    }
    auto play_b_core = sm_g1_make_stream_connector (alternate_stream_endpoint);
    auto play_b_stream = zlink::stream_e2e_client::use (play_b_core);
    auto play_b_connected = play_b_stream.connect ().submit ();
    if (!static_cast<bool> (play_b_connected)) {
        throw std::runtime_error ("SM-G1 play-b stream connect failed");
    }

    auto play_a_auth =
      play_a_stream.request (stream_ensure_auth_req_t{"play-a", play_a_actor_id, "crash-owner"})
        .packet_name ("StreamEnsureAuthReq")
        .timeout (std::chrono::milliseconds (3000))
        .async<stream_auth_res_t> ()
        .result ();
    if (!static_cast<bool> (play_a_auth)) {
        throw std::runtime_error ("SM-G1 play-a auth failed: "
                                  + sm_g1_stream_error_text (play_a_auth));
    }

    auto play_a_before_crash =
      play_a_stream.request (actor_ping_req_t{"before-crash"})
        .packet_name ("ActorPingReq")
        .timeout (std::chrono::milliseconds (3000))
        .async<actor_ping_res_t> ()
        .result ();
    if (!static_cast<bool> (play_a_before_crash)
        || play_a_before_crash.value ().node_rid != "play-a") {
        throw std::runtime_error ("SM-G1 play-a actor setup mismatch");
    }

    auto play_b_auth =
      play_b_stream.request (stream_ensure_auth_req_t{"play-b", play_b_actor_id, "survivor"})
        .packet_name ("StreamEnsureAuthReq")
        .timeout (std::chrono::milliseconds (3000))
        .async<stream_auth_res_t> ()
        .result ();
    if (!static_cast<bool> (play_b_auth)) {
        throw std::runtime_error ("SM-G1 play-b auth failed: "
                                  + sm_g1_stream_error_text (play_b_auth));
    }

    auto play_b_before_crash =
      play_b_stream.request (actor_ping_req_t{"before-crash"})
        .packet_name ("ActorPingReq")
        .timeout (std::chrono::milliseconds (3000))
        .async<actor_ping_res_t> ()
        .result ();
    if (!static_cast<bool> (play_b_before_crash)
        || play_b_before_crash.value ().node_rid != "play-b") {
        throw std::runtime_error ("SM-G1 play-b actor setup mismatch");
    }

    sm_g1_write_signal_file (crash_ready_file);
    sm_g1_wait_signal_file (crash_go_file, "play-a crash");

    auto play_b_after_crash =
      play_b_stream.request (actor_ping_req_t{"after-crash"})
        .packet_name ("ActorPingReq")
        .timeout (std::chrono::milliseconds (3000))
        .async<actor_ping_res_t> ()
        .result ();
    if (!static_cast<bool> (play_b_after_crash)
        || play_b_after_crash.value ().actor_id != play_b_actor_id
        || play_b_after_crash.value ().node_rid != "play-b") {
        throw std::runtime_error ("SM-G1 play-b state did not survive play-a crash");
    }

    auto failed_after_crash =
      play_a_stream.request (actor_ping_req_t{"after-crash"})
        .packet_name ("ActorPingReq")
        .timeout (std::chrono::milliseconds (1000))
        .async<actor_ping_res_t> ()
        .result ();
    if (static_cast<bool> (failed_after_crash)) {
        throw std::runtime_error ("SM-G1 play-a request unexpectedly succeeded after crash");
    }

    sm_g1_write_signal_file (crash_observed_file);

    (void) play_a_stream.close ().submit ();
    (void) play_b_stream.close ().submit ();
    std::cout << "scenario SM-G1 crash observed passed\n";
}

inline void run_sm_g1_crash_recovery_scenario (const std::string &stream_endpoint)
{
    if (stream_endpoint.empty ()) {
        throw std::runtime_error ("streamEndpoint is required for SM-G1 recovery");
    }

    auto recovered_core = sm_g1_make_stream_connector (stream_endpoint);
    auto recovered_stream = zlink::stream_e2e_client::use (recovered_core);
    auto recovered_connected = recovered_stream.connect ().submit ();
    if (!static_cast<bool> (recovered_connected)) {
        throw std::runtime_error ("SM-G1 recovered stream connect failed");
    }

    std::string last_recover_error = "unknown stream error";
    bool recovered_auth_accepted = false;
    const auto reclaim_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (15);
    while (std::chrono::steady_clock::now () < reclaim_deadline) {
        auto recovered_auth =
          recovered_stream.request (
                            stream_ensure_auth_req_t{"play-b", "actor-sm-g1-crash",
                                                     "recovered-on-play-b"})
            .packet_name ("StreamEnsureAuthReq")
            .timeout (std::chrono::milliseconds (3000))
            .async<stream_auth_res_t> ()
            .result ();
        if (static_cast<bool> (recovered_auth)) {
            recovered_auth_accepted = true;
            break;
        }
        last_recover_error = sm_g1_stream_error_text (recovered_auth);
        std::this_thread::sleep_for (std::chrono::milliseconds (500));
    }
    if (!recovered_auth_accepted) {
        throw std::runtime_error ("SM-G1 recovered auth failed: " + last_recover_error);
    }

    auto recovered_state =
      recovered_stream.request (actor_ping_req_t{"rebound"})
        .packet_name ("ActorPingReq")
        .timeout (std::chrono::milliseconds (3000))
        .async<actor_ping_res_t> ()
        .result ();
    if (!static_cast<bool> (recovered_state)
        || recovered_state.value ().actor_id != "actor-sm-g1-crash"
        || recovered_state.value ().node_rid != "play-b") {
        throw std::runtime_error ("SM-G1 recovered state mismatch");
    }

    (void) recovered_stream.close ().submit ();
    std::cout << "scenario SM-G1 passed\n";
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
