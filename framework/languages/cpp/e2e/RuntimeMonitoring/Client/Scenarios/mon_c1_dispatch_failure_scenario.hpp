/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../Support/client_support.hpp"
#include "../../Shared/runtime_monitoring_contracts.hpp"

#include <zlink/http_client.hpp>

#include <chrono>
#include <cstdint>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace zlink::framework::e2e::runtime_monitoring::client
{

inline std::uint64_t evidence_sequence (const std::string &line)
{
    const auto marker = line.find ("|sequence=");
    if (marker == std::string::npos)
        return 0;
    return std::stoull (line.substr (marker + 10));
}

inline void run_mon_c1_dispatch_failure_scenario (const client_options_t &options)
{
    auto http = zlink::http_client::client_t::create ()
                  .base_url (options.filtered_service_url)
                  .timeout (std::chrono::seconds (15))
                  .build ();
    ensure (http.post ("/runtime/observe").submit_raw ().result ().value ().status
              < 400,
            "MON-C1 normal observer did not start");
    ensure (
      http.post ("/runtime/observe-isolation").submit_raw ().result ().value ().status
        < 400,
      "MON-C1 isolated observers did not start");
    ensure (
      http.post ("/admin/application-gate/arm")
          .submit_raw ()
          .result ()
          .value ()
          .status
        < 400,
      "MON-C1 application gate was not armed");

    const auto release_gate = [&] {
        (void) http.post ("/admin/application-gate/release")
          .submit_raw ()
          .result ();
    };
    auto gated_request = std::async (
      std::launch::async, [&options] {
          auto gate_http = zlink::http_client::client_t::create ()
                             .base_url (options.service_url)
                             .timeout (std::chrono::seconds (20))
                             .build ();
          return gate_http
            .post ("/mesh/application-gate/request?targetRid=svc-b")
            .body (application_gate_req_t{.marker = "mon-c1-application-gate"})
            .submit<application_gate_res_t> ()
            .result ()
            .value ()
            .body;
      });
    try {
        ensure (
          http.post ("/admin/application-gate/wait")
              .submit_raw ()
              .result ()
              .value ()
              .status
            < 400,
          "MON-C1 application callback did not enter its gate");

        nlohmann::json gated_snapshot;
        const auto claim_deadline =
          std::chrono::steady_clock::now () + std::chrono::seconds (5);
        do {
            gated_snapshot = runtime_snapshot (options.filtered_service_url);
            if (gated_snapshot.at ("claims")
                  .at ("applicationActive")
                  .get<bool> ())
                break;
            std::this_thread::sleep_for (std::chrono::milliseconds (20));
        } while (std::chrono::steady_clock::now () < claim_deadline);
        ensure (
          gated_snapshot.at ("claims").at ("applicationActive").get<bool> (),
          "MON-C1 snapshot did not expose the active application callback");

        const auto request_reply =
          http.post ("/mesh/profile/request?targetRid=svc-throw")
            .body (profile_req_t{
              .value = "claim-progress", .marker = "mon-c1-infrastructure"})
            .submit<profile_res_t> ()
            .result ()
            .value ()
            .body;
        ensure (
          request_reply.provider_rid == "svc-throw"
            && request_reply.value == "profile:claim-progress",
          "MON-C1 MeshNode request did not complete while application was gated");

        for (int index = 0; index < 12; ++index) {
            const int weight = index % 2 == 0 ? 0 : 100;
            const auto changed =
              http.post (
                    "/admin/mesh-weight?weight=" + std::to_string (weight))
                .submit_raw ()
                .result ();
            ensure (changed && changed.value ().status < 400,
                    "MON-C1 channel pressure transition failed");
        }

        wait_for_new_evidence (
          options.filtered_service_url,
          "identifier=zlink.runtime.mesh_node.claim_changed",
          0, std::chrono::seconds (10));
        ensure (
          gated_request.wait_for (std::chrono::milliseconds (0))
            != std::future_status::ready,
          "MON-C1 gated application request completed before release");
        release_gate ();
        const auto gate_reply = gated_request.get ();
        ensure (
          gate_reply.provider_rid == "svc-b"
            && gate_reply.marker == "mon-c1-application-gate",
          "MON-C1 gated application request did not complete after release");
        wait_evidence_contains (
          options.filtered_service_url,
          "claim-domain=application|reason=released",
          std::chrono::seconds (10));

        const auto evidence = fetch_evidence (options.filtered_service_url);
        ensure (
          any_contains (
            evidence,
            "identifier=zlink.runtime.mesh_node.claim_changed")
            && any_contains (evidence, "claim-domain=application|reason=active"),
          "MON-C1 normal observer missed application claim progress");
        ensure (count_contains (evidence, "mesh-runtime-throwing|") == 1,
                "MON-C1 failing observer was not isolated after its exception");
        ensure (
          count_contains (
            evidence,
            "mesh-runtime-event|mesh=runtime.monitoring.mesh|identifier=zlink.runtime.mesh_node.channel_changed")
            >= 2,
          "MON-C1 normal observer did not progress under queue pressure");

        std::vector<std::uint64_t> slow_sequences;
        for (const auto &line : evidence) {
            if (contains (line, "mesh-runtime-slow|"))
                slow_sequences.push_back (evidence_sequence (line));
        }
        bool sequence_gap = false;
        for (std::size_t index = 1; index < slow_sequences.size (); ++index)
            sequence_gap =
              sequence_gap
              || slow_sequences[index] > slow_sequences[index - 1] + 1;
        ensure (sequence_gap,
                "MON-C1 slow observer did not expose a coalesced sequence gap");

        const auto recovered = runtime_snapshot (options.filtered_service_url);
        ensure (
          !recovered.at ("claims").at ("applicationActive").get<bool> (),
          "MON-C1 application claim did not release");
        ensure (
          recovered.at ("sequence").get<std::uint64_t> ()
            > slow_sequences.back (),
          "MON-C1 snapshot did not resynchronize after the observer gap");
    }
    catch (...) {
        release_gate ();
        throw;
    }

    std::cout << "scenario MON-C1 passed\n";
}

} // namespace zlink::framework::e2e::runtime_monitoring::client
