/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/automatic_turn_dispatch_contracts.hpp"
#include "../Support/scenario_assert.hpp"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace zlink::framework::e2e::automatic_turn_dispatch::client
{

template <typename TConnector>
void run_td_c3_io_worker_capacity_scenario (TConnector &connector,
                                            TConnector &observer,
                                            const std::string &spot_id)
{
    constexpr int operation_count = 32;
    const auto request_id = unique_id ("TD-C3");
    auto send_operation = [&] (int index) {
        connector.send (io_worker_await_msg_t{.request_id = request_id,
                                              .operation_id = "io-" + std::to_string (index),
                                              .delay_ms = 300})
          .packet_name (io_worker_await_msg_t::packet_name)
          .metadata (spot_id_metadata, spot_id)
          .submit ();
    };
    send_operation (0);
    auto released =
      observer.request (await_evidence_wait_req_t{.request_id = request_id,
                                                   .marker = "io-worker-released",
                                                   .timeout_milliseconds = 3000})
        .packet_name (await_evidence_wait_req_t::packet_name)
        .metadata (target_node_rid_metadata, "play-a")
        .timeout (std::chrono::seconds (10))
        .template submit<await_evidence_res_t> ();
    ensure (static_cast<bool> (released), "TD-C3 release marker wait failed");
    ensure (contains_request_marker (released.value ().evidence, request_id,
                                     "io-worker-released"),
            "TD-C3 release marker was not observed");
    observer.send (timer_start_msg_t{.request_id = request_id,
                                     .timer_name = request_id + "-timer",
                                     .mode = "fast",
                                     .period_ms = 20,
                                     .delay_ms = 0})
      .packet_name (timer_start_msg_t::packet_name)
      .metadata (spot_id_metadata, spot_id)
      .submit ();
    for (int index = 1; index < operation_count; ++index) {
        send_operation (index);
    }
    observer.send (probe_msg_t{.request_id = request_id, .marker = "io-worker-probe"})
      .packet_name (probe_msg_t::packet_name)
      .metadata (spot_id_metadata, spot_id)
      .submit ();

    await_evidence_res_t evidence;
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (10);
    for (;;) {
        auto snapshot =
          observer.request (await_evidence_req_t{.request_id = request_id})
            .packet_name (await_evidence_req_t::packet_name)
            .metadata (target_node_rid_metadata, "play-a")
            .timeout (std::chrono::seconds (5))
            .template submit<await_evidence_res_t> ();
        ensure (static_cast<bool> (snapshot), "TD-C3 evidence request failed");
        evidence = std::move (snapshot.value ());
        int completed = 0;
        for (const auto &line : evidence.evidence) {
            completed += line.find ("io-worker-completed") != std::string::npos ? 1 : 0;
        }
        if (completed == operation_count) {
            break;
        }
        ensure (std::chrono::steady_clock::now () < deadline,
                "TD-C3 did not complete all I/O workers");
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
    }
    for (const auto &line : evidence.evidence) {
        ensure (line.find ("WorkerQueueFull") == std::string::npos,
                "TD-C3 exhausted the CPU worker queue");
    }
    ensure (contains_in_order (evidence.evidence, request_id,
                               {"io-worker-released", "probe-started",
                                "probe-completed", "io-worker-completed"}),
            "TD-C3 probe did not progress while I/O workers yielded");
    ensure (contains_in_order (evidence.evidence, request_id,
                               {"io-worker-released", "timer-fast-completed",
                                "io-worker-completed"}),
            "TD-C3 timer did not progress while I/O workers yielded");
    observer.send (timer_stop_msg_t{.request_id = request_id})
      .packet_name (timer_stop_msg_t::packet_name)
      .metadata (spot_id_metadata, spot_id)
      .submit ();
    std::cout << "scenario TD-C3 passed\n";
}

} // namespace zlink::framework::e2e::automatic_turn_dispatch::client
