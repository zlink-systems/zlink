/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <gtest/gtest.h>
#include "allocation_counter.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/messaging/client_call_codec.hpp"
#include "runtime/locations/spot_address_resolvers.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include <zlink/framework.hpp>
#include <thread>
#include <atomic>
#include "runtime_failure_fixture.hpp"
#include <array>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>

namespace
{
namespace fw = zlink::framework;
using parts_t = fw::runtime::messaging::message_parts_t;
using transport_reply_t = fw::result_t<parts_t>;

struct payload_t
{
    static constexpr const char *packet_name = "p";
    int value = 7;
};

void add_payload (fw::serializer_registry_t &serializers)
{
    serializers.add<payload_t> (
      [] (const payload_t &payload) {
          return fw::encoded_payload_t::from_string (std::to_string (payload.value));
      },
      [] (const fw::encoded_payload_t &payload) {
          return payload_t{std::stoi (payload.to_string ())};
      },
      "x/y");
}

struct allocation_sample_t
{
    std::array<fw::tests::allocation_event_t, 256> events{};
    std::size_t count = 0;
    std::size_t total = 0;
    std::string diagnostics;
};

template <typename TAction> allocation_sample_t sample_allocations (TAction &&action)
{
    allocation_sample_t sample;
    fw::tests::allocation_event_log_t log{sample.events.data (), sample.events.size ()};
    {
        fw::tests::allocation_event_scope_t scope (log);
        sample.total = fw::tests::count_allocations (std::forward<TAction> (action));
    }
    EXPECT_FALSE (log.overflow);
    sample.count = log.count;
    sample.diagnostics = fw::tests::describe_allocations (log);
    return sample;
}

void print_allocation_sizes (const char *label, const allocation_sample_t &sample)
{
    std::printf ("%s: total=%zu sizes=", label, sample.total);
    for (std::size_t i = 0; i < sample.count; ++i)
        std::printf ("%s%zu", i == 0 ? "" : ",", sample.events[i].size);
    std::printf ("\n");
}

// Locate a live result in the last recorded allocation containing its address.
// Earlier temporary allocations may have reused the same storage. No object
// representation or allocator size class is used to identify the reply.
std::optional<std::size_t> allocation_containing (const allocation_sample_t &sample,
                                                  const void *address)
{
    const std::less<const void *> before;
    for (std::size_t i = sample.count; i > 0; --i) {
        const auto &event = sample.events[i - 1];
        const auto *end = static_cast<const unsigned char *> (event.memory) + event.size;
        if (!before (address, event.memory) && before (address, end))
            return i - 1;
    }
    return std::nullopt;
}

void expect_public_and_physical_reply_allocations (const allocation_sample_t &sample,
                                                   const fw::request_task_t<payload_t> &pending)
{
    auto physical = fw::detail::request_task_access_t::physical_reply (pending);
    ASSERT_TRUE (physical.await_ready ());
    const auto public_allocation =
      allocation_containing (sample, std::addressof (pending.result ()));
    const auto physical_allocation =
      allocation_containing (sample, std::addressof (physical.result ()));
    ASSERT_TRUE (public_allocation.has_value ());
    ASSERT_TRUE (physical_allocation.has_value ());
    EXPECT_NE (*public_allocation, *physical_allocation);
}

using node_transport_t =
  std::variant<std::monostate,
               std::shared_ptr<fw::detail::route_channel_runtime_t>,
               fw::detail::channel_runtime_state_t::mesh_node_transport_ptr_t>;
using channel_transport_t =
  std::variant<fw::detail::channel_runtime_state_t::client_server_request_t,
               fw::detail::channel_runtime_state_t::mesh_channel_request_t>;

// Minimal references compose existing request primitives. Their value argument
// boundaries and capture types match the owning runtime layers so the current
// standard library measures its own map heads and std::function storage. They
// do not duplicate routing, decoding, admission or completion decisions.
void encode_request_primitive (fw::serializer_registry_t &serializers,
                               std::string channel_name,
                               std::string packet_name,
                               std::chrono::milliseconds timeout,
                               fw::channel_request_call_t::metadata_map_t metadata)
{
    fw::runtime::messaging::client_call_codec_t codec;
    auto header = codec.create_envelope (fw::runtime::messaging::message_kind_t::request,
                                         channel_name, packet_name, timeout);
    header.metadata = std::move (metadata);
    (void) codec.encode_payload_parts (
      std::move (header), serializers.get<payload_t> ().serialize_with_content_type (payload_t{}));
}

allocation_sample_t encode_allocations (fw::serializer_registry_t &serializers,
                                        const std::string &channel_name)
{
    const fw::channel_request_call_t::metadata_map_t metadata;
    const std::string packet_name = payload_t::packet_name;
    return sample_allocations ([&] {
        encode_request_primitive (serializers, channel_name, packet_name,
                                  fw::runtime::configuration::default_request_timeout, metadata);
    });
}

allocation_sample_t internal_projection_allocations (
  std::optional<channel_transport_t> channel_transport = std::nullopt)
{
    fw::task_completion_source_t<transport_reply_t> source;
    auto admitted = std::make_shared<fw::task_completion_source_t<void>> ();
    admitted->complete (fw::result_t<void>::success ());
    // The transport fixture has already registered its public-reply observer.
    // Preserve that vector population before measuring the next registration.
    fw::request_task_t<transport_reply_t> selected (source.task (), admitted);
    std::optional<fw::request_task_t<zlink::message_t>> projected;
    const std::shared_ptr<fw::detail::route_client_state_t> state;
    std::string packet_name = payload_t::packet_name;
    if (channel_transport) {
        std::string channel_name = "c";
        return sample_allocations ([&] {
            projected.emplace (fw::detail::project_request_reply<zlink::message_t> (
              selected,
              [state, transport = std::move (*channel_transport),
               packet_name = std::move (packet_name), channel_name = std::move (channel_name)] (
                const fw::result_t<transport_reply_t> &) -> fw::result_t<zlink::message_t> {
                  (void) state;
                  (void) transport;
                  (void) packet_name;
                  (void) channel_name;
                  return fw::result_t<zlink::message_t>::success (zlink::message_t{});
              }));
        });
    }
    node_transport_t transport;
    std::string router_channel_id = "m";
    std::shared_ptr<const zlink::routing_id_t> target_owner;
    return sample_allocations ([&] {
        projected.emplace (fw::detail::project_request_reply<zlink::message_t> (
          selected,
          [state, transport = std::move (transport), packet_name = std::move (packet_name),
           router_channel_id = std::move (router_channel_id),
           target_owner = std::move (target_owner)] (
            const fw::result_t<transport_reply_t> &) -> fw::result_t<zlink::message_t> {
              (void) state;
              (void) transport;
              (void) packet_name;
              (void) router_channel_id;
              (void) target_owner;
              return fw::result_t<zlink::message_t>::success (zlink::message_t{});
          }));
    });
}

allocation_sample_t public_projection_allocations (fw::serializer_registry_t &registry)
{
    fw::task_completion_source_t<zlink::message_t> source;
    auto admitted = std::make_shared<fw::task_completion_source_t<void>> ();
    admitted->complete (fw::result_t<void>::success ());
    auto selected =
      fw::detail::request_task_access_t::from_admitted_reply (source.task (), admitted);
    std::optional<fw::request_task_t<payload_t>> projected;
    return sample_allocations ([&] {
        projected.emplace (fw::detail::public_request_reply<payload_t> (
          selected,
          [serializers =
             &registry] (const fw::result_t<zlink::message_t> &) -> fw::result_t<payload_t> {
              (void) serializers;
              return fw::result_t<payload_t>::success (payload_t{});
          }));
    });
}

allocation_sample_t channel_selection_allocations (
  const fw::detail::channel_runtime_state_t::mesh_channel_request_t &requester)
{
    fw::runtime::offload_executor_t executor (1, "request-allocation-reference");
    fw::runtime::state_lane_t lane (executor);
    const std::string channel_name = "c";
    const std::map<std::string, fw::detail::channel_runtime_state_t::client_server_request_t>
      client_server_transports;
    const std::map<std::string, fw::detail::channel_runtime_state_t::mesh_channel_request_t>
      route_mesh_transports{{channel_name, requester}};
    return sample_allocations ([&] {
        (void) lane
          .run_checked ([&] () -> std::optional<channel_transport_t> {
              const auto client_server_transport = client_server_transports.find (channel_name);
              if (client_server_transport != client_server_transports.end ())
                  return channel_transport_t (std::in_place_index<0>,
                                              client_server_transport->second);
              const auto route_mesh_transport = route_mesh_transports.find (channel_name);
              if (route_mesh_transport != route_mesh_transports.end ())
                  return channel_transport_t (std::in_place_index<1>, route_mesh_transport->second);
              return std::nullopt;
          })
          .get ();
    });
}

parts_t reply_parts (int value)
{
    fw::runtime::messaging::client_call_codec_t codec;
    auto header = codec.create_envelope (fw::runtime::messaging::message_kind_t::response, "m", "p",
                                         std::chrono::milliseconds (1000));
    return codec.encode_payload_parts (
      std::move (header),
      fw::serialized_payload_t{fw::encoded_payload_t::from_string (std::to_string (value)), "x/y"});
}

/* Submit/completion §9 and hot path E4·E5: a request is one operation with one
 * terminal. Above the transport the call decodes that terminal into the
 * envelope body and then into the reply type, once each, and shares the
 * transport's admission source. The public reply adds its own state, which
 * ends at an admission failure before the physical terminal, and the
 * observer of that admission. No coroutine or task wraps a layer again. */
TEST (RequestCallAllocations, NodeRequestAddsOneReplyPerReplyTypeAboveTheTransport)
{
    fw::serializer_registry_t serializers;
    add_payload (serializers);
    auto builder = fw::test::runtime_failure_builder ();
    auto runtime = fw::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    fw::task_completion_source_t<transport_reply_t> physical;
    auto admitted = std::make_shared<fw::task_completion_source_t<void>> ();
    admitted->complete (fw::result_t<void>::success ());
    std::optional<fw::request_task_t<transport_reply_t>> prepared;
    unsigned submitted = 0;
    runtime.bind_mesh_node_transport ("m", {}, [&] (const auto &, auto, auto) {
        ++submitted;
        auto selected = std::move (*prepared);
        prepared.reset ();
        return selected;
    });
    auto client = builder.route_client (serializers);
    const auto first_encode = encode_allocations (serializers, "m").total;
    const auto encode = encode_allocations (serializers, "m");
    std::printf ("encode calibration: first=%zu steady=%zu\n", first_encode, encode.total);
    // A transport may borrow its target until the physical terminal.
    const auto target_owner = sample_allocations (
      [] { (void) std::make_shared<const zlink::routing_id_t> (zlink::routing_id_t::from ("n")); });
    const auto internal_projection = internal_projection_allocations ();
    const auto public_projection = public_projection_allocations (serializers);
    print_allocation_sizes ("node encode reference", encode);
    print_allocation_sizes ("node target reference", target_owner);
    print_allocation_sizes ("node internal projection reference", internal_projection);
    print_allocation_sizes ("node public projection reference", public_projection);
    SCOPED_TRACE ("node encode reference\n" + encode.diagnostics);
    SCOPED_TRACE ("node target reference\n" + target_owner.diagnostics);
    SCOPED_TRACE ("node internal projection reference\n" + internal_projection.diagnostics);
    SCOPED_TRACE ("node public projection reference\n" + public_projection.diagnostics);

    prepared.emplace (physical.task (), admitted);
    auto call = client.request_to_node ("m", zlink::routing_id_t::from ("n"), payload_t{});
    std::optional<fw::request_task_t<payload_t>> pending;
    const auto actual = sample_allocations ([&] { pending.emplace (call.async<payload_t> ()); });
    print_allocation_sizes ("node request above transport", actual);
    SCOPED_TRACE ("node request above transport\n" + actual.diagnostics);
    EXPECT_EQ (submitted, 1u);
    EXPECT_EQ (actual.total, encode.total + target_owner.total + internal_projection.total
                               + public_projection.total);

    ASSERT_TRUE (pending.has_value ());
    EXPECT_EQ (fw::detail::request_task_access_t::admission_source (*pending), admitted);
    EXPECT_FALSE (pending->await_ready ());
    EXPECT_TRUE (pending->admitted ().result ());
    EXPECT_TRUE (physical.complete (
      fw::result_t<transport_reply_t>::success (transport_reply_t::success (reply_parts (41)))));
    const auto &reply = pending->result ();
    ASSERT_TRUE (reply);
    EXPECT_EQ (reply.value ().value, 41);
    expect_public_and_physical_reply_allocations (actual, *pending);
}

TEST (RequestCallAllocations, ChannelRequestAddsOneReplyPerReplyTypeAboveTheTransport)
{
    fw::serializer_registry_t serializers;
    add_payload (serializers);
    auto builder = fw::test::runtime_failure_builder ();
    auto runtime = fw::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    fw::task_completion_source_t<transport_reply_t> physical;
    auto admitted = std::make_shared<fw::task_completion_source_t<void>> ();
    admitted->complete (fw::result_t<void>::success ());
    std::optional<fw::request_task_t<transport_reply_t>> prepared;
    fw::detail::channel_runtime_state_t::mesh_channel_request_t requester = [&] (auto, auto) {
        auto selected = std::move (*prepared);
        prepared.reset ();
        return selected;
    };
    runtime.bind_mesh_channel_transport ("c", {}, requester);
    auto client = builder.route_client (serializers);
    (void) encode_allocations (serializers, "c");
    const auto encode = encode_allocations (serializers, "c");
    const auto selection = channel_selection_allocations (requester);
    const auto internal_projection =
      internal_projection_allocations (channel_transport_t (std::in_place_index<1>, requester));
    const auto public_projection = public_projection_allocations (serializers);
    print_allocation_sizes ("channel encode reference", encode);
    print_allocation_sizes ("channel selection reference", selection);
    print_allocation_sizes ("channel internal projection reference", internal_projection);
    print_allocation_sizes ("channel public projection reference", public_projection);
    SCOPED_TRACE ("channel encode reference\n" + encode.diagnostics);
    SCOPED_TRACE ("channel selection reference\n" + selection.diagnostics);
    SCOPED_TRACE ("channel internal projection reference\n" + internal_projection.diagnostics);
    SCOPED_TRACE ("channel public projection reference\n" + public_projection.diagnostics);
    prepared.emplace (physical.task (), admitted);
    auto call = client.request_to_channel ("c", payload_t{});
    std::optional<fw::request_task_t<payload_t>> pending;
    const auto actual = sample_allocations ([&] { pending.emplace (call.async<payload_t> ()); });
    print_allocation_sizes ("channel request above transport", actual);
    SCOPED_TRACE ("channel request above transport\n" + actual.diagnostics);
    EXPECT_EQ (actual.total, encode.total + selection.total + internal_projection.total
                               + public_projection.total);

    ASSERT_TRUE (pending.has_value ());
    EXPECT_EQ (fw::detail::request_task_access_t::admission_source (*pending), admitted);
    EXPECT_TRUE (physical.complete (
      fw::result_t<transport_reply_t>::success (transport_reply_t::success (reply_parts (43)))));
    const auto &reply = pending->result ();
    ASSERT_TRUE (reply);
    EXPECT_EQ (reply.value ().value, 43);
    expect_public_and_physical_reply_allocations (actual, *pending);
}

class ready_spot_resolver_t final : public fw::runtime::spot_address_resolver_t
{
  public:
    fw::task_t<std::optional<fw::runtime::spot_address_t>>
    resolve_spot_address (std::string, std::string spot_id) override
    {
        fw::runtime::spot_address_t address;
        address.mesh_name = "s";
        address.node_rid = zlink::routing_id_t::from ("n");
        address.spot_id = std::move (spot_id);
        address.spot_generation = 1;
        return fw::task_t<std::optional<fw::runtime::spot_address_t>> (
          fw::result_t<std::optional<fw::runtime::spot_address_t>>::success (std::move (address)));
    }
    void invalidate_spot_address (std::string_view) override {}
    void invalidate_all_routes_after_store_recovery () override {}
};

// Measure the operation's promise, admission and selected-submission await with
// the same value boundaries as the Spot submit. The frame size is deliberately
// not compared: its compiler-owned locals differ from the real resolver path.
fw::request_task_t<zlink::message_t>
spot_operation_primitive (fw::route_client_t::payload_encoder_t encode_payload,
                          fw::channel_request_call_t::metadata_map_t metadata,
                          fw::request_task_t<zlink::message_t> selected,
                          fw::runtime::spot_address_resolver_t &resolver)
{
    (void) encode_payload;
    (void) metadata;
    (void) co_await resolver.resolve_spot_address ({}, "spot", false);
    co_return co_await fw::detail::request_submission (std::move (selected));
}

allocation_sample_t spot_operation_allocations (fw::runtime::spot_address_resolver_t &resolver)
{
    fw::task_completion_source_t<zlink::message_t> source;
    auto admitted = std::make_shared<fw::task_completion_source_t<void>> ();
    admitted->complete (fw::result_t<void>::success ());
    auto selected =
      fw::detail::request_task_access_t::from_admitted_reply (source.task (), admitted);
    auto request_value = std::make_shared<payload_t> ();
    const fw::channel_request_call_t::metadata_map_t metadata;
    std::optional<fw::request_task_t<zlink::message_t>> operation;
    const auto sample = sample_allocations ([&] {
        operation.emplace (spot_operation_primitive (
          [request_value] (fw::serializer_registry_t &serializers) {
              return serializers.get<payload_t> ().serialize_with_content_type (*request_value);
          },
          metadata, std::move (selected), resolver));
    });
    EXPECT_TRUE (source.complete (fw::result_t<zlink::message_t>::success (zlink::message_t{})));
    EXPECT_TRUE (operation->result ());
    return sample;
}

allocation_sample_t spot_selection_allocations (
  const fw::detail::channel_runtime_state_t::spot_mesh_request_t &requester)
{
    fw::runtime::offload_executor_t executor (1, "request-allocation-reference");
    fw::runtime::state_lane_t lane (executor);
    const std::string mesh_name = "s";
    const std::map<std::string, fw::detail::channel_runtime_state_t::spot_mesh_request_t>
      requesters{{mesh_name, requester}};
    return sample_allocations ([&] {
        (void) lane
          .run_checked ([&] {
              const auto found = requesters.find (mesh_name);
              return found == requesters.end ()
                       ? std::nullopt
                       : std::optional<fw::detail::channel_runtime_state_t::spot_mesh_request_t> (
                           found->second);
          })
          .get ();
    });
}

allocation_sample_t spot_projection_allocations (
  const fw::detail::channel_runtime_state_t::spot_mesh_request_t &requester)
{
    fw::task_completion_source_t<transport_reply_t> source;
    auto admitted = std::make_shared<fw::task_completion_source_t<void>> ();
    admitted->complete (fw::result_t<void>::success ());
    fw::request_task_t<transport_reply_t> selected (source.task (), admitted);
    std::optional<fw::request_task_t<zlink::message_t>> projected;
    const std::shared_ptr<fw::detail::route_client_state_t> state;
    std::optional<fw::detail::channel_runtime_state_t::spot_mesh_request_t> mesh_requester (
      requester);
    std::string packet_name = payload_t::packet_name;
    std::string router_channel_id = "s";
    const std::shared_ptr<const std::pair<zlink::routing_id_t, fw::spot_id_t>> target;
    const auto sample = sample_allocations ([&] {
        projected.emplace (fw::detail::project_request_reply<zlink::message_t> (
          selected,
          [state, requester = std::move (mesh_requester), packet_name = std::move (packet_name),
           router_channel_id = std::move (router_channel_id),
           target] (const fw::result_t<transport_reply_t> &) -> fw::result_t<zlink::message_t> {
              (void) state;
              (void) requester;
              (void) packet_name;
              (void) router_channel_id;
              (void) target;
              return fw::result_t<zlink::message_t>::success (zlink::message_t{});
          }));
    });
    EXPECT_EQ (fw::detail::request_task_access_t::admission_source (*projected), admitted);
    return sample;
}

/* A Spot request resolves its address in its own operation coroutine, which
 * owns the operation's admission source and message reply (Spot address
 * messaging: one resolve per operation). The selected mesh request below it
 * is projected once and shares the transport's admission. */
TEST (RequestCallAllocations, SpotRequestProjectsTheSelectedMeshRequestOnce)
{
    fw::serializer_registry_t serializers;
    add_payload (serializers);
    auto builder = fw::test::runtime_failure_builder ();
    auto runtime = fw::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    auto resolver = std::make_shared<ready_spot_resolver_t> ();
    runtime.bind_spot_address_resolver (resolver);
    fw::task_completion_source_t<transport_reply_t> physical;
    auto admitted = std::make_shared<fw::task_completion_source_t<void>> ();
    admitted->complete (fw::result_t<void>::success ());
    std::optional<fw::request_task_t<transport_reply_t>> prepared;
    unsigned submitted = 0;
    fw::detail::channel_runtime_state_t::spot_mesh_request_t requester =
      [&] (const zlink::routing_id_t &, const std::string &, std::uint64_t, auto, auto) {
          ++submitted;
          auto selected = std::move (*prepared);
          prepared.reset ();
          return selected;
      };
    runtime.bind_spot_mesh_transport ("s", {}, requester);
    auto client = builder.route_client (serializers);
    (void) encode_allocations (serializers, "s");
    const auto encode = encode_allocations (serializers, "s");
    const auto operation = spot_operation_allocations (*resolver);
    const auto selection = spot_selection_allocations (requester);
    const auto target_owner = sample_allocations ([] {
        (void) std::make_shared<const std::pair<zlink::routing_id_t, fw::spot_id_t>> (
          zlink::routing_id_t::from ("n"), "spot");
    });
    const auto projection = spot_projection_allocations (requester);
    const auto public_projection = public_projection_allocations (serializers);
    print_allocation_sizes ("spot encode reference", encode);
    print_allocation_sizes ("spot operation reference", operation);
    print_allocation_sizes ("spot selection reference", selection);
    print_allocation_sizes ("spot target reference", target_owner);
    print_allocation_sizes ("spot internal projection reference", projection);
    print_allocation_sizes ("spot public projection reference", public_projection);
    SCOPED_TRACE ("spot encode reference\n" + encode.diagnostics);
    SCOPED_TRACE ("spot operation reference\n" + operation.diagnostics);
    SCOPED_TRACE ("spot selection reference\n" + selection.diagnostics);
    SCOPED_TRACE ("spot target reference\n" + target_owner.diagnostics);
    SCOPED_TRACE ("spot internal projection reference\n" + projection.diagnostics);
    SCOPED_TRACE ("spot public projection reference\n" + public_projection.diagnostics);
    prepared.emplace (physical.task (), admitted);
    auto call = client.request_to_spot (fw::spot_id_t ("spot"), payload_t{});
    std::optional<fw::request_task_t<payload_t>> pending;
    const auto actual = sample_allocations ([&] { pending.emplace (call.async<payload_t> ()); });
    print_allocation_sizes ("spot request above transport", actual);
    SCOPED_TRACE (actual.diagnostics);
    EXPECT_EQ (submitted, 1u);
    EXPECT_EQ (actual.total, encode.total + operation.total + selection.total + target_owner.total
                               + projection.total + public_projection.total);

    ASSERT_TRUE (pending.has_value ());
    const auto operation_admission = fw::detail::request_task_access_t::admission_source (*pending);
    ASSERT_NE (operation_admission, admitted);
    EXPECT_TRUE (allocation_containing (actual, operation_admission.get ()).has_value ());
    EXPECT_TRUE (
      allocation_containing (actual, std::addressof (operation_admission->task ().result ()))
        .has_value ());
    EXPECT_TRUE (pending->admitted ().result ());
    EXPECT_FALSE (pending->await_ready ());
    EXPECT_TRUE (physical.complete (
      fw::result_t<transport_reply_t>::success (transport_reply_t::success (reply_parts (47)))));
    const auto &reply = pending->result ();
    ASSERT_TRUE (reply);
    EXPECT_EQ (reply.value ().value, 47);
    expect_public_and_physical_reply_allocations (actual, *pending);
}

/* Hot path E4: the MeshNode request submit of a target the topology already
 * decides is the host runtime's own task. No coroutine wraps it to forward the
 * same result; a failure before submit is that task's result, as before. */
TEST (RequestCallAllocations, MeshNodeRequestSubmitDoesNotWrapTheHostTask)
{
    auto state =
      fw::test::runtime_failure_fixture<fw::detail::mesh_node_builder_state_t> ("request-call");
    fw::detail::mesh_node_runtime_t runtime (state);
    const auto failed_task = fw::tests::count_allocations ([] {
        try {
            throw fw::framework_exception_t (fw::framework_error_kind_t::protocol_error,
                                             "MeshNode has not started");
        }
        catch (...) {
            (void) fw::task_t<zlink::submit_result_t> (
              fw::detail::current_exception_result<zlink::submit_result_t> ());
        }
    });
    fw::detail::host::pending_operation_t operation;
    std::optional<fw::task_t<zlink::submit_result_t>> submitted;
    const auto target = zlink::routing_id_t::from ("n");
    const auto measured = fw::tests::count_allocations ([&] {
        submitted.emplace (runtime.request_to_node (target, std::vector<zlink::message_t>{},
                                                    operation, std::chrono::milliseconds (1000),
                                                    std::vector<std::uint8_t>{}));
    });
    std::printf ("mesh node request submit: measured=%zu failed_task=%zu\n", measured, failed_task);
    EXPECT_EQ (measured, failed_task);
    ASSERT_TRUE (submitted.has_value ());
    ASSERT_TRUE (submitted->await_ready ());
    EXPECT_EQ (submitted->result ().error_kind (), fw::framework_error_kind_t::protocol_error);
    ASSERT_NE (submitted->result ().error (), nullptr);
    EXPECT_STREQ (submitted->result ().error ()->what (), "MeshNode has not started");
}

/* Submit/completion §9: an admission failure is the caller's terminal even
 * while the transport has not reached its physical terminal; the later
 * physical terminal does not replace it. */
TEST (RequestCallAllocations, AdmissionFailureEndsThePublicReplyBeforeThePhysicalTerminal)
{
    for (const bool channel_request : {false, true}) {
        fw::serializer_registry_t serializers;
        add_payload (serializers);
        auto builder = fw::test::runtime_failure_builder ();
        auto runtime = fw::detail::channel_runtime_t::from (builder.message_bus ());
        runtime.bind_serializers (serializers);
        fw::task_completion_source_t<transport_reply_t> physical;
        auto admitted = std::make_shared<fw::task_completion_source_t<void>> ();
        const auto request = [&] (auto, auto) {
            return fw::request_task_t<transport_reply_t> (physical.task (), admitted);
        };
        runtime.bind_mesh_channel_transport ("c", {}, request);
        runtime.bind_mesh_node_transport ("m", {}, [&] (const auto &, auto parts, auto timeout) {
            return request (std::move (parts), timeout);
        });
        auto client = builder.route_client (serializers);
        auto pending =
          (channel_request
             ? client.request_to_channel ("c", payload_t{})
             : client.request_to_node ("m", zlink::routing_id_t::from ("n"), payload_t{}))
            .async<payload_t> ();
        auto admission = pending.admitted ();
        EXPECT_FALSE (pending.await_ready ());
        EXPECT_FALSE (admission.await_ready ());
        EXPECT_TRUE (admitted->complete (fw::result_t<void>::failure (
          fw::framework_error_kind_t::unavailable, "admission refused")));
        ASSERT_TRUE (pending.await_ready ());
        EXPECT_EQ (pending.result ().error_kind (), fw::framework_error_kind_t::unavailable);
        ASSERT_NE (pending.result ().error (), nullptr);
        EXPECT_STREQ (pending.result ().error ()->what (), "admission refused");
        EXPECT_EQ (admission.result ().error_kind (), fw::framework_error_kind_t::unavailable);
        EXPECT_TRUE (physical.complete (
          fw::result_t<transport_reply_t>::success (transport_reply_t::success (reply_parts (5)))));
        EXPECT_EQ (pending.result ().error_kind (), fw::framework_error_kind_t::unavailable);
    }
}
// A rejected raw request has no pending admission and returns only its
// ready result state. Neither the node entry nor header submission wraps it.
fw::task_t<bool> one_ready_admission_result ()
{
    return fw::task_t<bool> (fw::result_t<bool>::success (false));
}

TEST (RequestCallAllocations, RawNodeRequestDoesNotWrapItsAdmissionTask)
{
    fw::runtime::mesh::raw_mesh_node_options_t options;
    options.descriptor.mesh_name = "allocation-node";
    options.descriptor.node_routing_id = {'a'};
    options.descriptor.lifecycle_generation = options.descriptor.descriptor_revision = 1;
    options.descriptor.advertised_endpoint = "tcp://127.0.0.1:0";
    fw::runtime::mesh::raw_mesh_node_owner_t node (
      fw::test::runtime_failure_options (std::move (options)));
    fw::runtime::protocol::application_payload_t payload;
    std::vector<std::uint8_t> target{'b'};
    std::optional<fw::task_t<bool>> pending;
    const auto baseline =
      fw::tests::count_allocations ([&] { pending.emplace (one_ready_admission_result ()); });
    pending.reset ();
    const auto measured = fw::tests::count_allocations ([&] {
        pending.emplace (node.request_to_node (target, payload, std::chrono::seconds (1), {}));
    });
    std::printf ("raw node request: measured=%zu ready_admission=%zu\n", measured, baseline);
    EXPECT_EQ (measured, baseline);
    ASSERT_TRUE (pending->await_ready ());
    ASSERT_TRUE (pending->result ());
    EXPECT_FALSE (pending->result ().value ());
}
TEST (RequestCallAllocations, RawNodeRequestKeepsInvalidTimeoutInTheReturnedTask)
{
    fw::runtime::mesh::raw_mesh_node_options_t options;
    options.descriptor.mesh_name = "invalid-timeout-node";
    options.descriptor.node_routing_id = {'a'};
    options.descriptor.lifecycle_generation = options.descriptor.descriptor_revision = 1;
    options.descriptor.advertised_endpoint = "tcp://127.0.0.1:0";
    fw::runtime::mesh::raw_mesh_node_owner_t node (
      fw::test::runtime_failure_options (std::move (options)));
    fw::runtime::protocol::application_payload_t payload;
    std::optional<fw::task_t<bool>> pending;
    EXPECT_NO_THROW (pending.emplace (
      node.request_to_node ({'b'}, payload, std::chrono::milliseconds::zero (), {})));
    ASSERT_TRUE (pending.has_value ());
    ASSERT_TRUE (pending->await_ready ());
    const auto &result = pending->result ();
    ASSERT_FALSE (result);
    try {
        (void) result.value ();
        FAIL () << "invalid timeout did not remain a task failure";
    }
    catch (const std::invalid_argument &error) {
        EXPECT_STREQ (error.what (), "raw mesh request timeout must be positive");
    }
}

TEST (RequestCallAllocations, HostNodeRequestAddsNoCoroutineAboveTheRawTask)
{
    fw::completion_owner_t completion_owner;
    auto state =
      fw::test::runtime_failure_fixture<fw::detail::mesh_node_builder_state_t> ("host-request");
    state->core_context = std::make_shared<zlink::context_t> ();
    state->listen_endpoint = "tcp://127.0.0.1:0";
    state->routing_id = zlink::routing_id_t::from ("host-source");
    state->object_role = fw::object_role_t::none;
    fw::detail::mesh_node_runtime_t runtime (state);
    runtime.start ();
    struct stop_t
    {
        fw::detail::mesh_node_runtime_t &runtime;
        ~stop_t () { runtime.stop (); }
    } stop{runtime};
    auto host = runtime.native_node ().shared_from_this ();
    const auto target = zlink::routing_id_t::from ("missing-host-target");
    const auto target_bytes = target.to_bytes ();
    fw::runtime::host::pending_operation_t operation;
    fw::runtime::protocol::application_payload_t payload;
    std::optional<fw::task_t<bool>> raw;
    const auto raw_count = fw::tests::count_allocations ([&] {
        raw.emplace (
          host->transport ().request_to_node (target_bytes, payload, std::chrono::seconds (1), {}));
    });
    const auto target_count = fw::tests::count_allocations ([&] { (void) target.to_bytes (); });
    const auto callback_count = fw::tests::count_allocations ([&] {
        fw::runtime::foundation::operation_registry_t::callback_t callback =
          [host, operation] (fw::runtime::foundation::operation_terminal_t, zlink::message_t) {
              (void) host;
              (void) operation;
          };
    });
    const auto result_count = fw::tests::count_allocations ([] {
        (void) fw::task_t<zlink::submit_result_t> (
          fw::result_t<zlink::submit_result_t>::success (zlink::submit_result_t::not_found));
    });
    std::vector<zlink::message_t> calibrated_parts;
    calibrated_parts.push_back (zlink::message_t::from (std::string ("request")));
    const auto payload_count = fw::tests::count_allocations ([&] {
        (void) fw::runtime::protocol::application_payload_t::from_parts (
          std::move (calibrated_parts));
    });
    std::vector<zlink::message_t> parts;
    parts.push_back (zlink::message_t::from (std::string ("request")));
    std::optional<fw::task_t<zlink::submit_result_t>> pending;
    const auto measured = fw::tests::count_allocations ([&] {
        pending.emplace (
          host->request_to_node (target, std::move (parts), operation, std::chrono::seconds (1)));
    });
    const auto baseline = raw_count + target_count + callback_count + result_count + payload_count;
    std::printf (
      "host request: measured=%zu baseline=%zu raw=%zu target=%zu callback=%zu result=%zu\n",
      measured, baseline, raw_count, target_count, callback_count, result_count);
    EXPECT_EQ (measured, baseline);
    ASSERT_TRUE (pending->result ());
    EXPECT_EQ (pending->result ().value (), zlink::submit_result_t::not_found);
}

struct allocation_codec_t
{
    void register_framework_codecs (fw::codec_registration_context_t &context) const
    {
        context.add_serializer<payload_t> (
          [] (const payload_t &payload) {
              return fw::encoded_payload_t::from_string (std::to_string (payload.value));
          },
          [] (const fw::encoded_payload_t &payload) {
              return payload_t{std::stoi (payload.to_string ())};
          },
          "x/y");
    }
};

struct allocation_host_capture_t final : fw::hosted_service_t
{
    fw::route_client_t *client = nullptr;
    fw::route_mesh_runtime_t *mesh = nullptr;
    fw::framework_runtime_t *runtime = nullptr;
    fw::task_t<void> start (fw::service_provider_t &services) override
    {
        client = &services.get_required<fw::route_client_t> ();
        mesh = &services.get_required<fw::route_mesh_runtime_t> ();
        runtime = &services.get_required<fw::framework_runtime_t> ();
        co_return;
    }
    void stop () noexcept override {}
};

// The real app transport must not allocate an operation or a coroutine when
// topology has already classified an Object Client as an invalid Node target.
// Compare the same route layer over an immediate transport, plus the real
// topology classifier's routing-id byte vector. No allocation size identifies
// a reply state or an admission source.
TEST (RequestCallAllocations, AppClassifiedNodeFailureAddsNoTransportReplyState)
{
    struct app_host_t
    {
        fw::app_t app = fw::app_t::create ();
        allocation_host_capture_t *capture;
        std::thread runner;
        std::atomic<bool> exited{false};
        app_host_t ()
        {
            auto service = std::make_unique<allocation_host_capture_t> ();
            capture = service.get ();
            app.add_hosted_service (std::move (service));
        }
        void start ()
        {
            runner = std::thread ([this] {
                app.run (0, nullptr);
                exited = true;
            });
            const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);
            while (!app.is_ready () && !exited.load ()
                   && std::chrono::steady_clock::now () < deadline)
                std::this_thread::yield ();
            if (!app.is_ready ())
                throw std::runtime_error ("allocation host failed to start");
        }
        ~app_host_t ()
        {
            if (runner.joinable ()) {
                app.request_stop ();
                runner.join ();
            }
        }
    } target_host, source_host;
    const auto target = zlink::routing_id_t::from ("allocation-client");
    auto &target_options = target_host.app.add_zlink_framework ();
    target_options.add_location_store (
      std::make_shared<fw::runtime::in_memory_location_store_t> ());
    target_options.add_route_mesh ("m")
      .set_object_role (fw::object_role_t::client)
      .listen ("tcp://127.0.0.1:0")
      .set_routing_id (target);
    target_host.start ();
    const auto endpoint =
      target_host.capture->runtime->listener_status (fw::listener_kind_t::route_mesh, "m").endpoint;
    auto &options = source_host.app.add_zlink_framework ();
    options.codecs ().use (allocation_codec_t{});
    options.add_route_mesh ("m")
      .set_object_role (fw::object_role_t::none)
      .listen ("tcp://127.0.0.1:0")
      .set_routing_id (zlink::routing_id_t::from ("allocation-source"))
      .peer_connections ()
      .connect (target, endpoint);
    source_host.start ();
    bool known_client = false;
    const auto peer_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);
    while (std::chrono::steady_clock::now () < peer_deadline) {
        const auto snapshot = source_host.capture->mesh->snapshot ("m");
        if (std::any_of (snapshot.peers.begin (), snapshot.peers.end (), [&] (const auto &peer) {
                return peer.node_rid == target && peer.state == fw::peer_state_t::ready;
            })) {
            known_client = true;
            break;
        }
        std::this_thread::yield ();
    }
    ASSERT_TRUE (known_client);
    auto *host = source_host.capture;
    fw::serializer_registry_t serializers;
    add_payload (serializers);
    auto builder = fw::test::runtime_failure_builder ();
    auto channel = fw::detail::channel_runtime_t::from (builder.message_bus ());
    channel.bind_serializers (serializers);
    channel.bind_mesh_node_transport ("m", {}, [] (const auto &, auto, auto) {
        return fw::request_task_t<transport_reply_t> (
          fw::result_t<transport_reply_t>::success (transport_reply_t::failure (
            fw::framework_error_kind_t::not_found, "MeshNode request target was not found")));
    });
    auto reference = builder.route_client (serializers);
    const auto target_bytes = sample_allocations ([&] { (void) target.to_bytes (); });
    std::optional<fw::request_task_t<payload_t>> pending;
    auto measure = [&] (fw::route_client_t &client) {
        auto call = client.request_to_node ("m", target, payload_t{});
        auto sample = sample_allocations ([&] { pending.emplace (call.async<payload_t> ()); });
        EXPECT_EQ (pending->result ().error_kind (), fw::framework_error_kind_t::not_found);
        expect_public_and_physical_reply_allocations (sample, *pending);
        const auto admission = fw::detail::request_task_access_t::admission_source (*pending);
        EXPECT_TRUE (allocation_containing (sample, admission.get ()).has_value ());
        EXPECT_TRUE (allocation_containing (sample, std::addressof (admission->task ().result ()))
                       .has_value ());
        return sample;
    };
    (void) measure (reference);
    (void) measure (*host->client);
    const auto baseline = measure (reference);
    const auto actual = measure (*host->client);
    SCOPED_TRACE ("app classified reference\n" + baseline.diagnostics);
    SCOPED_TRACE ("app classified actual\n" + actual.diagnostics);
    print_allocation_sizes ("app classified reference", baseline);
    print_allocation_sizes ("app classified target bytes reference", target_bytes);
    print_allocation_sizes ("app classified actual", actual);
    EXPECT_EQ (actual.total, baseline.total + target_bytes.total);
}
} // namespace
