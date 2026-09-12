/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "metric_test_reader.hpp"

#include "runtime/diagnostics/mesh_request_metrics.hpp"
#include "runtime/foundation/operation_registry.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include "runtime/stateful/public_host_runtime.hpp"

#include <zlink/Contracts/Core/context.hpp>
#include <zlink/Contracts/Core/routing_id.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <future>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace mesh = zlink::framework::runtime::mesh;
namespace foundation = zlink::framework::runtime::foundation;
namespace host = zlink::framework::runtime::host;
namespace protocol = zlink::framework::runtime::protocol;
namespace runtime = zlink::framework::runtime;
using namespace std::chrono_literals;

namespace
{

using labels_t = std::map<std::string, std::string>;

struct metric_sample_t
{
    std::string name;
    std::string unit;
    std::string kind;
    labels_t labels;
    double value = 0;
    double histogram_sum = 0;
};

void require (bool condition, std::string message)
{
    if (!condition)
        throw std::runtime_error (std::move (message));
}

template <typename T>
T await_task (zlink::framework::task_t<T> task)
{
    return std::move (task).result ().value ();
}

void await_task (zlink::framework::task_t<void> task)
{
    std::move (task).result ().value ();
}

std::vector<std::uint8_t> bytes (std::string_view value)
{
    return {value.begin (), value.end ()};
}

labels_t labels (const metric_test::sdk::PointAttributes &attributes)
{
    labels_t result;
    for (const auto &[key, value] : attributes)
        result.emplace (key, opentelemetry::nostd::get<std::string> (value));
    return result;
}

std::string describe_labels (const labels_t &value)
{
    std::string result = "{";
    for (const auto &[key, field] : value) {
        if (result.size () != 1)
            result += ", ";
        result += key + "=" + field;
    }
    return result + "}";
}

std::vector<metric_sample_t> samples (metric_test::provider_t &provider)
{
    std::vector<metric_sample_t> result;
    for (const auto &metric : provider.collect ()) {
        for (const auto &point : metric.point_data_attr_) {
            metric_sample_t sample{
              metric.instrument_descriptor.name_,
              metric.instrument_descriptor.unit_,
              {},
              labels (point.attributes),
              0,
              0};
            if (const auto *sum =
                  opentelemetry::nostd::get_if<metric_test::sdk::SumPointData> (
                    &point.point_data)) {
                sample.kind = sum->is_monotonic_ ? "counter" : "updown";
                sample.value = opentelemetry::nostd::get<double> (sum->value_);
            }
            else if (const auto *gauge =
                       opentelemetry::nostd::get_if<
                         metric_test::sdk::LastValuePointData> (
                         &point.point_data)) {
                sample.kind = "gauge";
                sample.value =
                  opentelemetry::nostd::get<double> (gauge->value_);
            }
            else if (const auto *histogram =
                       opentelemetry::nostd::get_if<
                         metric_test::sdk::HistogramPointData> (
                         &point.point_data)) {
                sample.kind = "histogram";
                sample.value = static_cast<double> (histogram->count_);
                sample.histogram_sum =
                  opentelemetry::nostd::get<double> (histogram->sum_);
            }
            else {
                continue;
            }
            result.push_back (std::move (sample));
        }
    }
    return result;
}

std::optional<metric_sample_t>
find_sample (metric_test::provider_t &provider,
             std::string_view name,
             const labels_t &expected_labels)
{
    const auto collected = samples (provider);
    const auto found = std::find_if (
      collected.begin (), collected.end (),
      [&] (const auto &sample) {
          return sample.name == name && sample.labels == expected_labels;
      });
    return found == collected.end () ? std::nullopt
                                     : std::make_optional (*found);
}

void require_positive_duration (metric_test::provider_t &provider,
                                labels_t expected_labels)
{
    const auto sample = find_sample (
      provider, "zlink.mesh_node.request.duration", expected_labels);
    const auto identity = std::string ("zlink.mesh_node.request.duration ")
                          + describe_labels (expected_labels);
    require (sample.has_value (), "missing metric " + identity);
    require (sample->kind == "histogram" && sample->unit == "s",
             "unexpected duration descriptor for " + identity);
    require (std::isfinite (sample->histogram_sum)
               && sample->histogram_sum > 0,
             "duration sum was not finite and positive for " + identity
               + ": actual="
               + std::to_string (sample->histogram_sum));
}

void require_metric (metric_test::provider_t &provider,
                     std::string_view name,
                     std::string_view kind,
                     std::string_view unit,
                     labels_t expected_labels,
                     double expected_value)
{
    const auto sample = find_sample (provider, name, expected_labels);
    const auto identity = std::string (name) + " "
                          + describe_labels (expected_labels);
    require (sample.has_value (), "missing metric " + identity);
    require (sample->kind == kind,
             "unexpected instrument kind for " + identity
               + ": expected=" + std::string (kind)
               + " actual=" + sample->kind);
    require (sample->unit == unit,
             "unexpected instrument unit for " + identity
               + ": expected=" + std::string (unit)
               + " actual=" + sample->unit);
    require (sample->value == expected_value,
             "unexpected metric value for " + identity
               + ": expected=" + std::to_string (expected_value)
               + " actual=" + std::to_string (sample->value));
}

mesh::service_node_descriptor_t descriptor (
  std::string mesh_name,
  std::string routing_id,
  std::vector<mesh::service_channel_descriptor_t> channels = {})
{
    std::sort (
      channels.begin (), channels.end (),
      [] (const auto &left, const auto &right) {
          return left.name < right.name;
      });
    mesh::service_node_descriptor_t result;
    result.mesh_name = std::move (mesh_name);
    result.node_routing_id = bytes (routing_id);
    result.lifecycle_generation = 1;
    result.descriptor_revision = 1;
    result.advertised_endpoint = "tcp://127.0.0.1:0";
    result.channels = std::move (channels);
    return result;
}

bool pump_until (
  mesh::raw_mesh_node_owner_t &first,
  mesh::raw_mesh_node_owner_t &second,
  const std::function<bool ()> &ready,
  std::chrono::milliseconds timeout = 2s)
{
    const auto deadline = std::chrono::steady_clock::now () + timeout;
    while (!ready () && std::chrono::steady_clock::now () < deadline) {
        const auto now = mesh::service_liveness_registry_t::clock_t::now ();
        (void) await_task (first.drain_monitor_events (now));
        (void) await_task (second.drain_monitor_events (now));
        (void) await_task (first.pump_one (now));
        (void) await_task (second.pump_one (now));
        std::this_thread::yield ();
    }
    return ready ();
}

void admit_pair (mesh::raw_mesh_node_owner_t &source,
                 mesh::raw_mesh_node_owner_t &target)
{
    const auto target_descriptor = target.topology ().local_descriptor ();
    source.expect_peer (target_descriptor);
    require (source.connect_peer (target.endpoint (), target_descriptor),
             "raw MeshNode connect failed");
    require (
      pump_until (
        source, target,
        [&] {
            return source.topology ().peer (
                     target_descriptor.node_routing_id)
                     .has_value ();
        }),
      "raw MeshNode admission timed out");
}

void verify_topology_metrics_and_selection_reasons ()
{
    constexpr std::string_view mesh_name = "metrics-topology";
    mesh::raw_mesh_node_owner_t target (
      {descriptor (std::string (mesh_name), "metrics-topology-target",
                   {{"alpha", 100}, {"zero", 0}})});
    target.start ();

    metric_test::provider_t provider;
    auto source_options = mesh::raw_mesh_node_options_t{
      descriptor (std::string (mesh_name), "metrics-topology-source",
                  {{"server-self", 100}})};
    source_options.metric_channel_names = {"alpha", "empty", "zero"};
    source_options.metric_source = "manual_and_redis";
    mesh::raw_mesh_node_owner_t source (std::move (source_options));
    source.start ();

    const labels_t peer_labels{
      {"mesh_name", std::string (mesh_name)},
      {"source", "manual_and_redis"}};
    require_metric (provider, "zlink.mesh_node.peers.configured", "gauge",
                    "{peer}", peer_labels, 0);
    require_metric (provider, "zlink.mesh_node.peers.connected", "gauge",
                    "{peer}", peer_labels, 0);
    require_metric (provider, "zlink.mesh_node.peers.ready", "gauge",
                    "{peer}", peer_labels, 0);

    source.expect_peer (source.topology ().local_descriptor ());
    require_metric (provider, "zlink.mesh_node.peers.configured", "gauge",
                    "{peer}", peer_labels, 0);

    const auto target_descriptor = target.topology ().local_descriptor ();
    source.expect_peer (target_descriptor);
    require_metric (provider, "zlink.mesh_node.peers.configured", "gauge",
                    "{peer}", peer_labels, 1);
    require (source.connect_peer (target.endpoint (), target_descriptor),
             "topology metric connection failed");

    const auto connected_deadline = std::chrono::steady_clock::now () + 2s;
    while (std::chrono::steady_clock::now () < connected_deadline) {
        const auto now = mesh::service_liveness_registry_t::clock_t::now ();
        (void) await_task (source.drain_monitor_events (now));
        (void) await_task (target.drain_monitor_events (now));
        const auto connected = find_sample (
          provider, "zlink.mesh_node.peers.connected", peer_labels);
        if (connected && connected->value == 1)
            break;
        std::this_thread::yield ();
    }
    require_metric (provider, "zlink.mesh_node.peers.connected", "gauge",
                    "{peer}", peer_labels, 1);
    require_metric (provider, "zlink.mesh_node.peers.ready", "gauge",
                    "{peer}", peer_labels, 0);

    require (
      pump_until (
        source, target,
        [&] {
            return source.topology ().peer (
                     target_descriptor.node_routing_id)
                     .has_value ();
        }),
      "topology metric admission timed out");
    require_metric (provider, "zlink.mesh_node.peers.configured", "gauge",
                    "{peer}", peer_labels, 1);
    require_metric (provider, "zlink.mesh_node.peers.connected", "gauge",
                    "{peer}", peer_labels, 1);
    require_metric (provider, "zlink.mesh_node.peers.ready", "gauge",
                    "{peer}", peer_labels, 1);

    const auto channel_labels = [&] (std::string channel_name) {
        return labels_t{{"channel_name", std::move (channel_name)},
                        {"mesh_name", std::string (mesh_name)}};
    };
    require_metric (provider, "zlink.mesh_node.channels.ready_members",
                    "gauge", "{member}", channel_labels ("alpha"), 1);
    for (const auto channel : {"empty", "server-self", "zero"})
        require_metric (provider, "zlink.mesh_node.channels.ready_members",
                        "gauge", "{member}", channel_labels (channel), 0);

    const protocol::application_payload_t payload{
      "MetricProbe", "application/json", bytes ("payload")};
    require (!await_task (source.send_to_channel ("empty", payload)),
             "empty channel unexpectedly selected a member");
    require (!await_task (source.request_to_channel (
               "zero", payload, 2s, [] (auto, auto) {})),
             "zero-weight channel unexpectedly started a request");
    require (!await_task (
               source.send_to_channel ("not-registered", payload)),
             "unregistered channel unexpectedly selected a member");
    require_metric (
      provider, "zlink.mesh_node.channel.selection_failures", "counter",
      "{failure}",
      {{"channel_name", "empty"},
       {"mesh_name", std::string (mesh_name)},
       {"reason", "no_member"}},
      1);
    require (
      !find_sample (
        provider, "zlink.mesh_node.requests.inflight",
        {{"mesh_name", std::string (mesh_name)},
         {"surface", "channel"}}),
      "rejected channel selection emitted an inflight request");
    require (
      !find_sample (
        provider, "zlink.mesh_node.request.duration",
        {{"mesh_name", std::string (mesh_name)},
         {"outcome", "failed"},
         {"surface", "channel"}}),
      "rejected channel selection emitted request duration");
    require (
      !find_sample (
        provider, "zlink.mesh_node.request.timeouts",
        {{"mesh_name", std::string (mesh_name)},
         {"surface", "channel"}}),
      "selection failure emitted a timeout counter");
    require_metric (
      provider, "zlink.mesh_node.channel.selection_failures", "counter",
      "{failure}",
      {{"channel_name", "zero"},
       {"mesh_name", std::string (mesh_name)},
       {"reason", "not_ready"}},
      1);
    require (
      !find_sample (
        provider, "zlink.mesh_node.channel.selection_failures",
        {{"channel_name", "not-registered"},
         {"mesh_name", std::string (mesh_name)},
         {"reason", "no_member"}}),
      "unregistered channel emitted a selection failure");

    await_task (target.publish_draining ());
    require (
      pump_until (
        source, target,
        [&] {
            const auto peer = source.topology ().peer (
              target_descriptor.node_routing_id);
            return peer
                   && peer->descriptor.state
                        == mesh::service_node_state_t::draining;
        }),
      "draining descriptor update timed out");
    require_metric (provider, "zlink.mesh_node.peers.ready", "gauge",
                    "{peer}", peer_labels, 0);
    require_metric (provider, "zlink.mesh_node.channels.ready_members",
                    "gauge", "{member}", channel_labels ("alpha"), 0);
    require (!await_task (source.send_to_channel ("alpha", payload)),
             "draining channel unexpectedly selected a member");
    require_metric (
      provider, "zlink.mesh_node.channel.selection_failures", "counter",
      "{failure}",
      {{"channel_name", "alpha"},
       {"mesh_name", std::string (mesh_name)},
       {"reason", "draining"}},
      1);

    require (
      !source.disconnect_peer (
        target_descriptor.node_routing_id, target.endpoint ()),
      "sole-peer endpoint was unexpectedly retained after disconnect");
    require_metric (provider, "zlink.mesh_node.peers.connected", "gauge",
                    "{peer}", peer_labels, 0);
    require_metric (provider, "zlink.mesh_node.peers.ready", "gauge",
                    "{peer}", peer_labels, 0);
    require_metric (provider, "zlink.mesh_node.peers.configured", "gauge",
                    "{peer}", peer_labels, 1);
    source.forget_peer (
      target_descriptor.node_routing_id, target.endpoint ());
    require_metric (provider, "zlink.mesh_node.peers.configured", "gauge",
                    "{peer}", peer_labels, 0);
}

void verify_not_required_peer_counts_as_configured ()
{
    auto target_descriptor = descriptor (
      "metrics-not-required", "metrics-client-target");
    target_descriptor.object_role = mesh::service_object_role_t::client;
    mesh::raw_mesh_node_owner_t target ({std::move (target_descriptor)});
    target.start ();

    metric_test::provider_t provider;
    auto source_descriptor = descriptor (
      "metrics-not-required", "metrics-client-source");
    source_descriptor.object_role = mesh::service_object_role_t::client;
    mesh::raw_mesh_node_owner_t source ({std::move (source_descriptor)});
    source.start ();
    const auto remote = target.topology ().local_descriptor ();
    require (
      source.admit_peer (
        remote, bytes ("not-required-connection"),
        mesh::service_liveness_registry_t::clock_t::now ())
        == mesh::peer_admission_result_t::not_required,
      "client-only peer did not enter not-required topology state");
    const labels_t peer_labels{
      {"mesh_name", "metrics-not-required"}, {"source", "manual"}};
    require_metric (provider, "zlink.mesh_node.peers.configured", "gauge",
                    "{peer}", peer_labels, 1);
    require_metric (provider, "zlink.mesh_node.peers.connected", "gauge",
                    "{peer}", peer_labels, 0);
    require_metric (provider, "zlink.mesh_node.peers.ready", "gauge",
                    "{peer}", peer_labels, 0);
}

std::optional<mesh::service_mailbox_claim_t>
claim_request (mesh::raw_mesh_node_owner_t &target,
               mesh::service_mailbox_domain_t domain)
{
    const auto deadline = std::chrono::steady_clock::now () + 2s;
    while (std::chrono::steady_clock::now () < deadline) {
        auto claim = target.mailbox ().try_claim (domain, 1, 64u * 1024u);
        if (claim)
            return claim;
        const auto pumped = await_task (target.pump_one (
          mesh::service_liveness_registry_t::clock_t::now ()));
        require (pumped != mesh::raw_mesh_pump_result_t::protocol_error,
                 "raw request pump reported a protocol error");
        std::this_thread::yield ();
    }
    return std::nullopt;
}

using request_result_t =
  std::pair<foundation::operation_terminal_t, std::vector<std::uint8_t>>;

template <typename Submit, typename Reply>
void complete_remote_request (
  mesh::raw_mesh_node_owner_t &source,
  mesh::raw_mesh_node_owner_t &target,
  metric_test::provider_t &provider,
  std::string surface,
  mesh::service_mailbox_domain_t domain,
  Submit submit,
  Reply reply)
{
    std::promise<request_result_t> promise;
    auto completion = promise.get_future ();
    require (
      await_task (submit (
        [&promise] (foundation::operation_terminal_t terminal,
                    std::vector<std::uint8_t> payload) mutable {
            promise.set_value ({terminal, std::move (payload)});
        })),
      surface + " request was not submitted");
    auto claim = claim_request (target, domain);
    require (claim && claim->records.size () == 1,
             surface + " request was not delivered");

    const labels_t inflight_labels{
      {"mesh_name", "metrics-requests"}, {"surface", surface}};
    require_metric (provider, "zlink.mesh_node.requests.inflight", "updown",
                    "{request}", inflight_labels, 1);
    require (reply (claim->records.front ()),
             surface + " request reply failed");
    require (target.mailbox ().release (*claim),
             surface + " request mailbox release failed");

    const auto deadline = std::chrono::steady_clock::now () + 2s;
    while (completion.wait_for (0ms) != std::future_status::ready
           && std::chrono::steady_clock::now () < deadline) {
        const auto pumped = await_task (source.pump_one (
          mesh::service_liveness_registry_t::clock_t::now (), false));
        require (pumped != mesh::raw_mesh_pump_result_t::protocol_error,
                 surface + " reply pump reported a protocol error");
        std::this_thread::yield ();
    }
    require (completion.wait_for (0ms) == std::future_status::ready,
             surface + " request completion timed out");
    require (completion.get ().first
               == foundation::operation_terminal_t::completed,
             surface + " request did not complete successfully");
    require_metric (provider, "zlink.mesh_node.requests.inflight", "updown",
                    "{request}", inflight_labels, 0);
    require_metric (
      provider, "zlink.mesh_node.request.duration", "histogram", "s",
      {{"mesh_name", "metrics-requests"},
       {"outcome", "completed"},
       {"surface", surface}},
      1);
    require_positive_duration (
      provider,
      {{"mesh_name", "metrics-requests"},
       {"outcome", "completed"},
       {"surface", surface}});
    require (
      !find_sample (
        provider, "zlink.mesh_node.request.timeouts",
        {{"mesh_name", "metrics-requests"}, {"surface", surface}}),
      surface + " completed request emitted a timeout counter");
}

void verify_request_metrics_all_surfaces_and_late_reply ()
{
    mesh::raw_mesh_node_owner_t target (
      {descriptor ("metrics-requests", "metrics-request-target",
                   {{"work", 100}})});
    target.start ();

    metric_test::provider_t provider;
    auto source_options = mesh::raw_mesh_node_options_t{
      descriptor ("metrics-requests", "metrics-request-source")};
    source_options.metric_channel_names = {"work"};
    mesh::raw_mesh_node_owner_t source (std::move (source_options));
    source.start ();
    admit_pair (source, target);
    const auto local = source.topology ().local_descriptor ();
    const auto remote = target.topology ().local_descriptor ();
    const protocol::application_payload_t request_payload{
      "MetricRequest", "application/json", bytes ("request")};
    const protocol::application_payload_t reply_payload{
      "MetricReply", "application/json", bytes ("reply")};

    complete_remote_request (
      source, target, provider, "node",
      mesh::service_mailbox_domain_t::application,
      [&] (auto callback) {
          return source.request_to_node (
            remote.node_routing_id, request_payload, 2s,
            std::move (callback));
      },
      [&] (const auto &record) {
          return target.reply (record, reply_payload);
      });
    complete_remote_request (
      source, target, provider, "channel",
      mesh::service_mailbox_domain_t::application,
      [&] (auto callback) {
          return source.request_to_channel (
            "work", request_payload, 2s, std::move (callback));
      },
      [&] (const auto &record) {
          return target.reply (record, reply_payload);
      });

    const protocol::spot_route_fence_t spot{
      "spot-1", 1, remote.node_routing_id,
      remote.lifecycle_generation, 1, 1};
    complete_remote_request (
      source, target, provider, "spot",
      mesh::service_mailbox_domain_t::application,
      [&] (auto callback) {
          return source.request_to_spot (
            remote.node_routing_id, "source-spot", spot,
            request_payload, 2s, std::move (callback));
      },
      [&] (const auto &record) {
          return target.reply (record, reply_payload);
      });

    const protocol::actor_route_fence_t actor{
      "actor-1", 1, remote.node_routing_id,
      remote.lifecycle_generation, 1, 1};
    complete_remote_request (
      source, target, provider, "actor",
      mesh::service_mailbox_domain_t::application,
      [&] (auto callback) {
          return source.request_to_actor (
            remote.node_routing_id, std::nullopt, actor,
            request_payload, 2s, std::move (callback));
      },
      [&] (const auto &record) {
          return target.reply (record, reply_payload);
      });

    const auto deadline_unix_ms = static_cast<std::uint64_t> (
      std::chrono::duration_cast<std::chrono::milliseconds> (
        std::chrono::system_clock::now ().time_since_epoch () + 2s)
        .count ());
    protocol::instance_spot_activation_header_t activation{
      {remote.node_routing_id, remote.lifecycle_generation, "instance-1",
       "metrics-requests", "room", "descriptor-1", deadline_unix_ms},
      local.lifecycle_generation,
      local.node_routing_id,
      std::string ("source-spot"),
      true,
      {local.lifecycle_generation, 91},
      0,
      false};
    complete_remote_request (
      source, target, provider, "instance_spot",
      mesh::service_mailbox_domain_t::infrastructure,
      [&] (auto callback) {
          return source.request_instance_spot_activation (
            remote.node_routing_id, activation, std::nullopt,
            request_payload, 2s, std::move (callback));
      },
      [&] (const auto &record) {
          const auto decoded =
            protocol::decode_instance_spot_activation_header (
              record.parts.front ());
          require (decoded.request && decoded.reply_route_id != 0,
                   "instance activation was not encoded as a request");
          return target.reply_instance_spot_activation (
            record, 0, 0, reply_payload);
      });

    std::promise<request_result_t> activation_failure_promise;
    auto activation_failure = activation_failure_promise.get_future ();
    auto failed_activation = activation;
    failed_activation.operation.low = 92;
    require (await_task (source.request_instance_spot_activation (
               remote.node_routing_id, std::move (failed_activation),
               std::nullopt, request_payload, 2s,
               [&activation_failure_promise] (
                 foundation::operation_terminal_t terminal,
                 std::vector<std::uint8_t> payload) mutable {
                   activation_failure_promise.set_value (
                     {terminal, std::move (payload)});
               })),
             "failed instance activation was not submitted");
    auto failed_activation_claim = claim_request (
      target, mesh::service_mailbox_domain_t::infrastructure);
    require (failed_activation_claim
               && failed_activation_claim->records.size () == 1,
             "failed instance activation was not delivered");
    require_metric (
      provider, "zlink.mesh_node.requests.inflight", "updown", "{request}",
      {{"mesh_name", "metrics-requests"}, {"surface", "instance_spot"}},
      1);
    require (target.reply_instance_spot_activation (
               failed_activation_claim->records.front (), 104, 16,
               std::nullopt),
             "failed instance activation reply could not be sent");
    require (target.mailbox ().release (*failed_activation_claim),
             "failed instance activation mailbox release failed");
    const auto activation_failure_deadline =
      std::chrono::steady_clock::now () + 2s;
    while (activation_failure.wait_for (0ms) != std::future_status::ready
           && std::chrono::steady_clock::now ()
                < activation_failure_deadline) {
        (void) await_task (source.pump_one (
          mesh::service_liveness_registry_t::clock_t::now (), false));
        std::this_thread::yield ();
    }
    require (activation_failure.wait_for (0ms)
               == std::future_status::ready,
             "failed instance activation terminal was not delivered");
    const auto failed_activation_result = activation_failure.get ();
    require (failed_activation_result.first
               == foundation::operation_terminal_t::completed,
             "instance activation reply changed its callback terminal");
    const auto failed_activation_parts =
      protocol::unpack_infrastructure_reply (
        failed_activation_result.second);
    require (failed_activation_parts.size () == 1,
             "failed instance activation reply carried an invalid payload");
    const auto failed_activation_header =
      protocol::decode_reply_header (failed_activation_parts.front ());
    require (failed_activation_header.terminal_result == 104
               && failed_activation_header.failure_code == 16,
             "failed instance activation reply lost its wire failure");
    require_metric (
      provider, "zlink.mesh_node.requests.inflight", "updown", "{request}",
      {{"mesh_name", "metrics-requests"}, {"surface", "instance_spot"}},
      0);
    require_metric (
      provider, "zlink.mesh_node.request.duration", "histogram", "s",
      {{"mesh_name", "metrics-requests"},
       {"outcome", "failed"},
       {"surface", "instance_spot"}},
      1);
    require_metric (
      provider, "zlink.mesh_node.request.duration", "histogram", "s",
      {{"mesh_name", "metrics-requests"},
       {"outcome", "completed"},
       {"surface", "instance_spot"}},
      1);
    require (
      !find_sample (
        provider, "zlink.mesh_node.request.timeouts",
        {{"mesh_name", "metrics-requests"},
         {"surface", "instance_spot"}}),
      "failed instance activation emitted a timeout counter");

    std::promise<request_result_t> failed_promise;
    auto failed = failed_promise.get_future ();
    require (await_task (source.request_to_node (
               remote.node_routing_id, request_payload, 2s,
               [&failed_promise] (
                 foundation::operation_terminal_t terminal,
                 std::vector<std::uint8_t> payload) mutable {
                   failed_promise.set_value (
                     {terminal, std::move (payload)});
               })),
             "failed-outcome request was not submitted");
    auto failed_claim = claim_request (
      target, mesh::service_mailbox_domain_t::application);
    require (failed_claim && failed_claim->records.size () == 1,
             "failed-outcome request was not delivered");
    require_metric (
      provider, "zlink.mesh_node.requests.inflight", "updown", "{request}",
      {{"mesh_name", "metrics-requests"}, {"surface", "node"}}, 1);
    require (target.reply_failure (
               failed_claim->records.front (),
               static_cast<std::uint32_t> (
                 protocol::request_terminal_result::internalError),
               static_cast<std::uint32_t> (
                 protocol::framework_error_code::requestFailed)),
             "failed request reply could not be sent");
    require (target.mailbox ().release (*failed_claim),
             "failed request mailbox release failed");
    const auto failed_deadline = std::chrono::steady_clock::now () + 2s;
    while (failed.wait_for (0ms) != std::future_status::ready
           && std::chrono::steady_clock::now () < failed_deadline) {
        (void) await_task (source.pump_one (
          mesh::service_liveness_registry_t::clock_t::now (), false));
        std::this_thread::yield ();
    }
    require (failed.wait_for (0ms) == std::future_status::ready,
             "failed request terminal was not delivered");
    require (failed.get ().first
               == foundation::operation_terminal_t::transport_failed,
             "failed request reported the wrong terminal");
    require_metric (
      provider, "zlink.mesh_node.requests.inflight", "updown", "{request}",
      {{"mesh_name", "metrics-requests"}, {"surface", "node"}}, 0);
    require_metric (
      provider, "zlink.mesh_node.request.duration", "histogram", "s",
      {{"mesh_name", "metrics-requests"},
       {"outcome", "failed"},
       {"surface", "node"}},
      1);
    require_positive_duration (
      provider,
      {{"mesh_name", "metrics-requests"},
       {"outcome", "failed"},
       {"surface", "node"}});
    require (
      !find_sample (
        provider, "zlink.mesh_node.request.timeouts",
        {{"mesh_name", "metrics-requests"}, {"surface", "node"}}),
      "failed request emitted a timeout counter");

    std::promise<request_result_t> timeout_promise;
    auto timeout = timeout_promise.get_future ();
    require (await_task (source.request_to_node (
               remote.node_routing_id, request_payload, 2s,
               [&timeout_promise] (
                 foundation::operation_terminal_t terminal,
                 std::vector<std::uint8_t> payload) mutable {
                   timeout_promise.set_value (
                     {terminal, std::move (payload)});
               })),
             "timeout request was not submitted");
    auto late_claim = claim_request (
      target, mesh::service_mailbox_domain_t::application);
    require (late_claim && late_claim->records.size () == 1,
             "timeout request was not parked at its target");
    require_metric (
      provider, "zlink.mesh_node.requests.inflight", "updown", "{request}",
      {{"mesh_name", "metrics-requests"}, {"surface", "node"}}, 1);
    require (source.expire_requests (
               foundation::operation_registry_t::clock_t::now () + 3s)
               == 1,
             "timeout request did not expire");
    require (timeout.wait_for (2s) == std::future_status::ready,
             "timeout callback was not delivered");
    require (timeout.get ().first
               == foundation::operation_terminal_t::timed_out,
             "timeout request reported the wrong terminal");
    require_metric (
      provider, "zlink.mesh_node.requests.inflight", "updown", "{request}",
      {{"mesh_name", "metrics-requests"}, {"surface", "node"}}, 0);
    require_metric (
      provider, "zlink.mesh_node.request.duration", "histogram", "s",
      {{"mesh_name", "metrics-requests"},
       {"outcome", "timed_out"},
       {"surface", "node"}},
      1);
    require_positive_duration (
      provider,
      {{"mesh_name", "metrics-requests"},
       {"outcome", "timed_out"},
       {"surface", "node"}});
    require_metric (
      provider, "zlink.mesh_node.request.timeouts", "counter", "{request}",
      {{"mesh_name", "metrics-requests"}, {"surface", "node"}}, 1);

    require (target.reply (late_claim->records.front (), reply_payload),
             "late reply could not be sent");
    require (target.mailbox ().release (*late_claim),
             "late request mailbox release failed");

    // The following terminal reply on the same route is the ordering barrier
    // for the asynchronously consumed late reply above.
    std::promise<request_result_t> wire_timeout_promise;
    auto wire_timeout = wire_timeout_promise.get_future ();
    require (await_task (source.request_to_node (
               remote.node_routing_id, request_payload, 2s,
               [&wire_timeout_promise] (
                 foundation::operation_terminal_t terminal,
                 std::vector<std::uint8_t> payload) mutable {
                   wire_timeout_promise.set_value (
                     {terminal, std::move (payload)});
               })),
             "wire timeout request was not submitted");
    auto wire_timeout_claim = claim_request (
      target, mesh::service_mailbox_domain_t::application);
    require (wire_timeout_claim && wire_timeout_claim->records.size () == 1,
             "wire timeout request was not delivered");
    require_metric (
      provider, "zlink.mesh_node.requests.inflight", "updown", "{request}",
      {{"mesh_name", "metrics-requests"}, {"surface", "node"}}, 1);
    require (target.reply_failure (
               wire_timeout_claim->records.front (), 101, 0),
             "wire timeout reply could not be sent");
    require (target.mailbox ().release (*wire_timeout_claim),
             "wire timeout mailbox release failed");
    const auto wire_timeout_deadline =
      std::chrono::steady_clock::now () + 2s;
    while (wire_timeout.wait_for (0ms) != std::future_status::ready
           && std::chrono::steady_clock::now () < wire_timeout_deadline) {
        (void) await_task (source.pump_one (
          mesh::service_liveness_registry_t::clock_t::now (), false));
        std::this_thread::yield ();
    }
    require (wire_timeout.wait_for (0ms) == std::future_status::ready,
             "wire timeout terminal was not delivered");
    const auto wire_timeout_result = wire_timeout.get ();
    require (wire_timeout_result.first
               == foundation::operation_terminal_t::transport_failed,
             "wire timeout changed its callback terminal");
    const auto wire_timeout_header =
      protocol::decode_reply_header (wire_timeout_result.second);
    require (wire_timeout_header.terminal_result == 101,
             "wire timeout reply lost its terminal result");
    require_metric (
      provider, "zlink.mesh_node.requests.inflight", "updown", "{request}",
      {{"mesh_name", "metrics-requests"}, {"surface", "node"}}, 0);
    require_metric (
      provider, "zlink.mesh_node.request.duration", "histogram", "s",
      {{"mesh_name", "metrics-requests"},
       {"outcome", "timed_out"},
       {"surface", "node"}},
      2);
    require_metric (
      provider, "zlink.mesh_node.request.timeouts", "counter", "{request}",
      {{"mesh_name", "metrics-requests"}, {"surface", "node"}}, 2);
    require_metric (
      provider, "zlink.mesh_node.request.duration", "histogram", "s",
      {{"mesh_name", "metrics-requests"},
       {"outcome", "completed"},
       {"surface", "node"}},
      1);
}

void verify_remaining_terminal_outcomes ()
{
    metric_test::provider_t provider;
    auto metrics =
      std::make_shared<runtime::mesh_request_metrics_t> ("metrics-terminals");
    foundation::operation_registry_t registry;
    const auto register_operation =
      [&] (std::uint64_t id, runtime::mesh_request_surface_t surface) {
          require (registry.register_operation (
                     {1, id},
                     foundation::operation_registry_t::clock_t::now () + 2s,
                     [] (auto, auto) {}, {},
                     runtime::mesh_request_metric_t (metrics, surface)),
                   "terminal metric operation registration failed");
      };

    register_operation (1, runtime::mesh_request_surface_t::node);
    require (registry.cancel ({1, 1}), "request cancellation failed");
    register_operation (2, runtime::mesh_request_surface_t::channel);
    require (registry.fail (
               {1, 2}, foundation::operation_terminal_t::shutdown),
             "request shutdown terminal failed");
    register_operation (3, runtime::mesh_request_surface_t::actor);
    require (registry.fail (
               {1, 3}, foundation::operation_terminal_t::transport_failed),
             "request failure terminal failed");

    for (const auto &[surface, outcome] :
         std::vector<std::pair<std::string, std::string>>{
           {"node", "cancelled"},
           {"channel", "shutdown"},
           {"actor", "failed"}}) {
        require_metric (
          provider, "zlink.mesh_node.requests.inflight", "updown",
          "{request}",
          {{"mesh_name", "metrics-terminals"}, {"surface", surface}}, 0);
        require_metric (
          provider, "zlink.mesh_node.request.duration", "histogram", "s",
          {{"mesh_name", "metrics-terminals"},
           {"outcome", outcome},
           {"surface", surface}},
          1);
        require (
          !find_sample (
            provider, "zlink.mesh_node.request.timeouts",
            {{"mesh_name", "metrics-terminals"}, {"surface", surface}}),
          outcome + " terminal emitted a timeout counter");
    }
}

std::shared_ptr<zlink::framework::detail::mesh_node_builder_state_t>
make_public_node_state ()
{
    auto state = std::make_shared<
      zlink::framework::detail::mesh_node_builder_state_t> (
      "metrics-local-public");
    state->core_context = std::make_shared<zlink::context_t> ();
    state->listen_endpoint = "tcp://127.0.0.1:0";
    state->listen_port.reset ();
    state->routing_id =
      zlink::routing_id_t::from (std::string ("metrics-local-node"));
    state->spot_state->snapshot.actor_types.emplace_back ("metric.actor");
    return state;
}

void verify_public_host_local_spot_and_actor_requests ()
{
    metric_test::provider_t provider;
    zlink::framework::detail::mesh_node_runtime_t node (
      make_public_node_state ());
    // This isolated runtime has no application Location Store. Resolve only
    // its actual local object and generation, with the initial owner lease
    // used by the minimal stateful runtime (as in mesh_node_vertical).
    node.configure_spot_route_fence_resolver (
      [&node] (const zlink::routing_id_t &target_node,
               std::string_view spot_id, std::uint64_t generation)
        -> std::optional<host::route_fence_t> {
          const auto local = node.status ().routing_id ();
          if (target_node.to_bytes () != local.to_bytes ())
              return std::nullopt;
          const auto object = node.native_node ().objects ().find (
            runtime::stateful::object_kind_t::user_spot, std::string (spot_id));
          if (!object || object->object_generation != generation
              || object->node_id != local.to_string ())
              return std::nullopt;
          return host::route_fence_t{object->authority_owner_generation, 1};
      }, 0ms);
    node.start ();
    const auto node_id = node.status ().routing_id ();
    const std::vector<zlink::message_t> request_parts{
      zlink::message_t::from (std::string ("request"))};
    const std::vector<zlink::message_t> reply_parts{
      zlink::message_t::from (std::string ("reply"))};

    auto source_spot = node.get_or_create_spot ("source-spot");
    auto target_spot = node.get_or_create_spot ("target-spot");
    host::pending_operation_t spot_operation;
    require (await_task (source_spot.request_to_spot (
               node_id, target_spot.spot_id (),
               target_spot.status ().lifecycle_generation (), request_parts,
               spot_operation, zlink::send_flags_t::none, 2s))
               == zlink::submit_result_t::ok,
             "local public Spot request was not submitted");
    require_metric (
      provider, "zlink.mesh_node.requests.inflight", "updown", "{request}",
      {{"mesh_name", "metrics-local-public"}, {"surface", "spot"}}, 1);
    bool spot_replied = false;
    (void) await_task (node.dispatch_ready (
      [&] (const host::ready_record_t &,
           const host::receive_record_t &record,
           std::vector<zlink::message_t>) {
          if (record.kind == host::record_kind_t::spot_request) {
              spot_replied =
                host::reply (record.reply_token, reply_parts)
                == zlink::submit_result_t::ok;
          }
      }));
    require (spot_replied, "local public Spot request was not replied");
    require (node.wait_for_completion (spot_operation, 2s).has_value (),
             "local public Spot completion was not delivered");
    require_metric (
      provider, "zlink.mesh_node.request.duration", "histogram", "s",
      {{"mesh_name", "metrics-local-public"},
       {"outcome", "completed"},
       {"surface", "spot"}},
      1);
    require_positive_duration (
      provider,
      {{"mesh_name", "metrics-local-public"},
       {"outcome", "completed"},
       {"surface", "spot"}});
    require (
      !find_sample (
        provider, "zlink.mesh_node.request.timeouts",
        {{"mesh_name", "metrics-local-public"}, {"surface", "spot"}}),
      "local public Spot completion emitted a timeout counter");

    auto source_actor = node.create_actor (
      "metric.actor", "source-actor", {}, 2s);
    auto target_actor = node.create_actor (
      "metric.actor", "target-actor", {}, 2s);
    host::pending_operation_t actor_operation;
    require (await_task (source_actor.request_to (
               target_actor.ref (), request_parts, actor_operation,
               zlink::send_flags_t::none, 2s))
               == zlink::submit_result_t::ok,
             "local public Actor request was not submitted");
    require_metric (
      provider, "zlink.mesh_node.requests.inflight", "updown", "{request}",
      {{"mesh_name", "metrics-local-public"}, {"surface", "actor"}}, 1);
    bool actor_replied = false;
    (void) await_task (node.dispatch_ready (
      [&] (const host::ready_record_t &,
           const host::receive_record_t &record,
           std::vector<zlink::message_t>) {
          if (record.kind == host::record_kind_t::actor_request) {
              actor_replied =
                host::reply (record.reply_token, reply_parts)
                == zlink::submit_result_t::ok;
          }
      }));
    require (actor_replied, "local public Actor request was not replied");
    require (node.wait_for_completion (actor_operation, 2s).has_value (),
             "local public Actor completion was not delivered");
    require_metric (
      provider, "zlink.mesh_node.request.duration", "histogram", "s",
      {{"mesh_name", "metrics-local-public"},
       {"outcome", "completed"},
       {"surface", "actor"}},
      1);
    require_positive_duration (
      provider,
      {{"mesh_name", "metrics-local-public"},
       {"outcome", "completed"},
       {"surface", "actor"}});
    require (
      !find_sample (
        provider, "zlink.mesh_node.request.timeouts",
        {{"mesh_name", "metrics-local-public"}, {"surface", "actor"}}),
      "local public Actor completion emitted a timeout counter");
    node.stop ();
}

} // namespace

int main ()
{
    try {
        verify_topology_metrics_and_selection_reasons ();
        verify_not_required_peer_counts_as_configured ();
        verify_request_metrics_all_surfaces_and_late_reply ();
        verify_remaining_terminal_outcomes ();
        verify_public_host_local_spot_and_actor_requests ();
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << "mesh metric regression error: " << error.what () << '\n';
        return 1;
    }
}
