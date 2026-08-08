/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../Support/client_support.hpp"
#include "../../Shared/runtime_monitoring_contracts.hpp"

#include <zlink/http_client.hpp>

#include <algorithm>
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

inline std::string latest_evidence_field (const std::vector<std::string> &evidence,
                                          const std::string &marker,
                                          const std::string &field)
{
    const auto prefix = "|" + field + "=";
    for (auto line = evidence.rbegin (); line != evidence.rend (); ++line) {
        if (!contains (*line, marker))
            continue;
        const auto begin = line->find (prefix);
        if (begin == std::string::npos)
            continue;
        const auto value_begin = begin + prefix.size ();
        const auto end = line->find ('|', value_begin);
        return line->substr (value_begin, end - value_begin);
    }
    return {};
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

        wait_evidence_contains (
          options.filtered_service_url,
          "application-gate|state=entered",
          std::chrono::seconds (5));
        const auto first_gate_correlation = latest_evidence_field (
          fetch_evidence (options.filtered_service_url),
          "application-gate|state=entered", "corr");
        ensure (!first_gate_correlation.empty (),
                "MON-C1 first gated request has no correlation evidence");

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
          "MON-C1 MeshNode request did not survive the logging provider failure");
        wait_evidence_contains (
          options.throw_service_url,
          "message-flow-provider-throw|event=zlink.message_flow",
          std::chrono::seconds (5));
        wait_evidence_contains (
          options.filtered_service_url,
          "message-flow-provider|event=zlink.message_flow",
          std::chrono::seconds (5));

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

        ensure (
          gated_request.wait_for (std::chrono::milliseconds (0))
            != std::future_status::ready,
          "MON-C1 gated application request completed before release");
        ensure (
          http.post ("/admin/message-flow-mode?mode=off")
              .submit_raw ()
              .result ()
              .value ()
              .status
            < 400,
          "MON-C1 could not disable message-flow recording during a handler");
        release_gate ();
        const auto gate_reply = gated_request.get ();
        ensure (
          gate_reply.provider_rid == "svc-b"
            && gate_reply.marker == "mon-c1-application-gate",
          "MON-C1 gated application request did not complete after release");
        wait_evidence_contains (
          options.filtered_service_url,
          "application-gate|state=released",
          std::chrono::seconds (10));
        wait_evidence_contains (
          options.filtered_service_url,
          "phase=replied|surface=route_mesh_channel|corr="
            + first_gate_correlation,
          std::chrono::seconds (5));

        ensure (
          http.post ("/admin/application-gate/arm")
              .submit_raw ()
              .result ()
              .value ()
              .status
            < 400,
          "MON-C1 second application gate was not armed");
        auto off_gated_request = std::async (
          std::launch::async, [&options] {
              auto gate_http = zlink::http_client::client_t::create ()
                                 .base_url (options.service_url)
                                 .timeout (std::chrono::seconds (20))
                                 .build ();
              return gate_http
                .post ("/mesh/application-gate/request?targetRid=svc-b")
                .body (application_gate_req_t{
                  .marker = "mon-c1-off-entry-gate"})
                .submit<application_gate_res_t> ()
                .result ()
                .value ()
                .body;
          });
        ensure (
          http.post ("/admin/application-gate/wait")
              .submit_raw ()
              .result ()
              .value ()
              .status
            < 400,
          "MON-C1 off-entry callback did not enter its gate");
        const auto second_gate_correlation = latest_evidence_field (
          fetch_evidence (options.filtered_service_url),
          "application-gate|state=entered", "corr");
        ensure (!second_gate_correlation.empty ()
                  && second_gate_correlation != first_gate_correlation,
                "MON-C1 second gated request has no distinct correlation evidence");
        ensure (
          http.post ("/admin/message-flow-mode?mode=normal")
              .submit_raw ()
              .result ()
              .value ()
              .status
            < 400,
          "MON-C1 could not enable message-flow recording during a handler");
        release_gate ();
        const auto off_gate_reply = off_gated_request.get ();
        ensure (off_gate_reply.provider_rid == "svc-b"
                  && off_gate_reply.marker == "mon-c1-off-entry-gate",
                "MON-C1 off-entry gated request did not complete after release");
        std::this_thread::sleep_for (std::chrono::milliseconds (250));
        const auto snapshot_evidence = fetch_evidence (options.filtered_service_url);
        ensure (
          std::none_of (
            snapshot_evidence.begin (), snapshot_evidence.end (),
            [&] (const std::string &line) {
                return contains (
                         line,
                         "message-flow-provider|event=zlink.message_flow")
                       && contains (line, "|corr=" + second_gate_correlation);
            }),
          "MON-C1 off-entry request emitted a partial flow after enabling recording");

        const auto evidence = fetch_evidence (options.filtered_service_url);
        ensure (any_contains (evidence, "application-gate|state=entered")
                  && any_contains (evidence, "application-gate|state=released"),
                "MON-C1 application gate evidence did not converge");
        ensure (count_contains (evidence, "mesh-runtime-throwing|") == 1,
                "MON-C1 failing observer was not isolated after its exception");
        ensure (count_contains (evidence, "mesh-runtime-snapshot|mesh=runtime.monitoring.mesh|")
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
        ensure (recovered.at ("state").get<std::string> () != "degraded",
                "MON-C1 runtime did not remain available after release");
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
