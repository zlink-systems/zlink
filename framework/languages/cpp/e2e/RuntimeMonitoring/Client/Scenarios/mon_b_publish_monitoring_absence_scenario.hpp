/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../Support/client_support.hpp"

#include <nlohmann/json.hpp>
#include <zlink/framework.hpp>

#include <array>
#include <iostream>
#include <string_view>

namespace zlink::framework::e2e::runtime_monitoring::client
{

template <typename T>
concept has_multicast_member = requires (const T &value) {
    value.multicast;
};

static_assert (!has_multicast_member<zlink::framework::mesh_node_snapshot_t>);

inline nlohmann::json publish_monitoring_runtime_snapshot (
  const std::string &base_url)
{
    auto http = zlink::http_client::client_t::create ()
                  .base_url (base_url)
                  .timeout (std::chrono::milliseconds (3000))
                  .build ();
    return http.get ("/runtime/snapshot")
      .submit<nlohmann::json> ()
      .result ()
      .value ()
      .body;
}

inline constexpr std::array<std::string_view, 13> forbidden_publish_monitoring_names{
  "multicast",
  "remoteSnapshotCount",
  "remoteAdmittedCount",
  "remoteDroppedCount",
  "remoteUnreachableCount",
  "localSnapshotCount",
  "localAdmittedCount",
  "localDroppedCount",
  "zlink.mesh_node.multicast.submits",
  "zlink.mesh_node.multicast.targets",
  "zlink.mesh_node.multicast.pending",
  "zlink.runtime.mesh_node.multicast_backpressured",
  "zlink.runtime.mesh_node.multicast_dropped"};

inline void assert_publish_monitoring_absent (const std::string &value,
                                              std::string_view source)
{
    for (const auto forbidden : forbidden_publish_monitoring_names) {
        ensure (!contains (value, std::string (forbidden)),
                std::string (source) + " contains removed Publish monitoring name '"
                  + std::string (forbidden) + "'");
    }
}

inline void run_mon_b_publish_monitoring_absence_scenario (
  const client_options_t &options,
  std::string_view scenario,
  std::string topic,
  bool create_subject)
{
    auto http = zlink::http_client::client_t::create ()
                  .base_url (options.service_url)
                  .timeout (std::chrono::milliseconds (5000))
                  .build ();
    const auto evidence_start = fetch_evidence (options.service_url).size ();
    if (create_subject) {
        const auto created =
          http.post ("/admin/subject/create?spotId=monitoring-subject-b2")
            .submit_raw ()
            .result ();
        ensure (created && created.value ().status < 400,
                "MON-B2 local subscriber creation failed");
    }
    const auto published =
      http.post ("/runtime/publish?topic=" + topic).submit_raw ().result ();
    ensure (published && published.value ().status < 400,
            std::string (scenario) + " publish did not start");

    const auto snapshot =
      publish_monitoring_runtime_snapshot (options.service_url);
    ensure (snapshot.contains ("peers") && snapshot.contains ("channels")
              && snapshot.contains ("claims") && snapshot.contains ("drain"),
            std::string (scenario)
              + " generic RouteMesh monitoring fields are missing");
    assert_publish_monitoring_absent (
      snapshot.dump (), std::string (scenario) + " snapshot");

    const auto evidence = fetch_evidence (options.service_url);
    std::string appended;
    for (auto index = evidence_start; index < evidence.size (); ++index) {
        appended += evidence[index];
        appended.push_back ('\n');
    }
    assert_publish_monitoring_absent (
      appended, std::string (scenario) + " events");
    std::cout << "scenario " << scenario << " passed\n";
}

inline void run_mon_b1_publish_monitoring_absence_scenario (
  const client_options_t &options)
{
    run_mon_b_publish_monitoring_absence_scenario (
      options, "MON-B1", "monitoring.subject.none", false);
}

inline void run_mon_b2_publish_monitoring_absence_scenario (
  const client_options_t &options)
{
    run_mon_b_publish_monitoring_absence_scenario (
      options, "MON-B2", "monitoring.subject.trigger", true);
}

} // namespace zlink::framework::e2e::runtime_monitoring::client
