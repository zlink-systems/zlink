/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <gtest/gtest.h>
#include "allocation_counter.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/messaging/client_call_codec.hpp"
#include "runtime/locations/spot_address_resolvers.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime_failure_fixture.hpp"
#include <array>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>

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

// Allocation sizes of the task objects a request layer can add, calibrated on
// this build: a reply state per reply type and an admission source.
struct layer_sizes_t
{
    std::size_t payload_state = 0;
    std::size_t message_state = 0;
    std::size_t admission_state = 0;
};

std::size_t only_allocation_size (const auto &action)
{
    std::array<fw::tests::allocation_event_t, 4> events{};
    fw::tests::allocation_event_log_t log{events.data (), events.size ()};
    fw::tests::allocation_event_log = &log;
    const auto count = fw::tests::count_allocations (action);
    fw::tests::allocation_event_log = nullptr;
    EXPECT_GE (count, 1u);
    return count >= 1 ? events[count - 1].size : 0;
}

layer_sizes_t calibrate_layer_sizes ()
{
    layer_sizes_t sizes;
    sizes.payload_state =
      only_allocation_size ([] { (void) fw::task_completion_source_t<payload_t> (); });
    sizes.message_state =
      only_allocation_size ([] { (void) fw::task_completion_source_t<zlink::message_t> (); });
    sizes.admission_state =
      only_allocation_size ([] { (void) fw::task_completion_source_t<void> (); });
    return sizes;
}

struct layer_counts_t
{
    std::size_t total = 0;
    std::size_t payload_states = 0;
    std::size_t message_states = 0;
    std::size_t admission_sources = 0;
};

template <typename TAction>
layer_counts_t count_layers (const layer_sizes_t &sizes, TAction &&action)
{
    std::array<fw::tests::allocation_event_t, 256> events{};
    fw::tests::allocation_event_log_t log{events.data (), events.size ()};
    fw::tests::allocation_event_log = &log;
    layer_counts_t counts;
    counts.total = fw::tests::count_allocations (std::forward<TAction> (action));
    fw::tests::allocation_event_log = nullptr;
    EXPECT_FALSE (log.overflow);
    for (std::size_t i = 0; i < log.count; ++i) {
        counts.payload_states += events[i].size == sizes.payload_state;
        counts.message_states += events[i].size == sizes.message_state;
        counts.admission_sources += events[i].size == sizes.admission_state;
    }
    return counts;
}

// One layer that turns a pending reply into another reply type: its reply
// state, the terminal observer and that observer's registration.
std::size_t projection_allocations ()
{
    fw::task_completion_source_t<payload_t> source;
    auto input = source.task ();
    std::optional<fw::task_t<payload_t>> projected;
    return fw::tests::count_allocations ([&] {
        projected.emplace (fw::detail::project_task_terminal<payload_t> (
          input, [] (const fw::result_t<payload_t> &result) { return result; }));
    });
}

// The observer of an already-admitted request's admission source.
std::size_t ready_observer_allocations ()
{
    auto admitted = fw::task_t<void> (fw::result_t<void>::success ());
    auto reply = std::make_shared<int> ();
    return fw::tests::count_allocations ([&] {
        fw::detail::observe_task_terminal (admitted, [reply] (const fw::result_t<void> &) {});
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
    const auto sizes = calibrate_layer_sizes ();

    // The call's encode as the erased submit performs it, with the default timeout.
    const auto encode_once = [&] {
        return fw::tests::count_allocations ([&] {
            fw::runtime::messaging::client_call_codec_t codec;
            auto header =
              codec.create_envelope (fw::runtime::messaging::message_kind_t::request, "m", "p",
                                     fw::runtime::configuration::default_request_timeout);
            header.metadata = {};
            (void) codec.encode_payload_parts (
              std::move (header),
              serializers.get<payload_t> ().serialize_with_content_type (payload_t{}));
        });
    };
    const auto first_encode = encode_once ();
    const auto encode = encode_once ();
    std::printf ("encode calibration: first=%zu steady=%zu\n", first_encode, encode);
    // A transport may borrow its target until the physical terminal.
    const auto target_owner = fw::tests::count_allocations (
      [] { (void) std::make_shared<const zlink::routing_id_t> (zlink::routing_id_t::from ("n")); });
    const auto projection = projection_allocations ();
    const auto admission_end = ready_observer_allocations ();

    prepared.emplace (physical.task (), admitted);
    auto call = client.request_to_node ("m", zlink::routing_id_t::from ("n"), payload_t{});
    std::optional<fw::request_task_t<payload_t>> pending;
    const auto counts = count_layers (sizes, [&] { pending.emplace (call.async<payload_t> ()); });
    std::printf ("node request above transport: total=%zu payload_states=%zu message_states=%zu "
                 "admission_sources=%zu encode=%zu target_owner=%zu projection=%zu "
                 "admission_end=%zu\n",
                 counts.total, counts.payload_states, counts.message_states,
                 counts.admission_sources, encode, target_owner, projection, admission_end);
    EXPECT_EQ (submitted, 1u);
    // The node call closure has the size of a message reply state, so the
    // total below, not the size class, bounds the node message layer.
    // Payload states: the physical reply and the public reply.
    EXPECT_EQ (counts.payload_states, 2u);
    EXPECT_EQ (counts.admission_sources, 0u);
    EXPECT_EQ (counts.total, encode + target_owner + 2 * projection + 1 + admission_end);

    ASSERT_TRUE (pending.has_value ());
    EXPECT_FALSE (pending->await_ready ());
    EXPECT_TRUE (pending->admitted ().result ());
    EXPECT_TRUE (physical.complete (
      fw::result_t<transport_reply_t>::success (transport_reply_t::success (reply_parts (41)))));
    const auto &reply = pending->result ();
    ASSERT_TRUE (reply);
    EXPECT_EQ (reply.value ().value, 41);
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
    runtime.bind_mesh_channel_transport ("c", {}, [&] (auto, auto) {
        auto selected = std::move (*prepared);
        prepared.reset ();
        return selected;
    });
    auto client = builder.route_client (serializers);
    const auto sizes = calibrate_layer_sizes ();
    prepared.emplace (physical.task (), admitted);
    auto call = client.request_to_channel ("c", payload_t{});
    std::optional<fw::request_task_t<payload_t>> pending;
    const auto counts = count_layers (sizes, [&] { pending.emplace (call.async<payload_t> ()); });
    std::printf ("channel request above transport: total=%zu payload_states=%zu "
                 "message_states=%zu admission_sources=%zu\n",
                 counts.total, counts.payload_states, counts.message_states,
                 counts.admission_sources);
    EXPECT_EQ (counts.payload_states, 2u);
    EXPECT_EQ (counts.message_states, 1u);
    EXPECT_EQ (counts.admission_sources, 0u);

    ASSERT_TRUE (pending.has_value ());
    EXPECT_TRUE (physical.complete (
      fw::result_t<transport_reply_t>::success (transport_reply_t::success (reply_parts (43)))));
    const auto &reply = pending->result ();
    ASSERT_TRUE (reply);
    EXPECT_EQ (reply.value ().value, 43);
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
    runtime.bind_spot_address_resolver (std::make_shared<ready_spot_resolver_t> ());
    fw::task_completion_source_t<transport_reply_t> physical;
    auto admitted = std::make_shared<fw::task_completion_source_t<void>> ();
    admitted->complete (fw::result_t<void>::success ());
    std::optional<fw::request_task_t<transport_reply_t>> prepared;
    runtime.bind_spot_mesh_transport (
      "s", {}, [&] (const zlink::routing_id_t &, const std::string &, std::uint64_t, auto, auto) {
          auto selected = std::move (*prepared);
          prepared.reset ();
          return selected;
      });
    auto client = builder.route_client (serializers);
    const auto sizes = calibrate_layer_sizes ();
    prepared.emplace (physical.task (), admitted);
    auto call = client.request_to_spot (fw::spot_id_t ("spot"), payload_t{});
    std::optional<fw::request_task_t<payload_t>> pending;
    const auto counts = count_layers (sizes, [&] { pending.emplace (call.async<payload_t> ()); });
    std::printf ("spot request above transport: total=%zu payload_states=%zu message_states=%zu "
                 "admission_sources=%zu\n",
                 counts.total, counts.payload_states, counts.message_states,
                 counts.admission_sources);
    // Message states: the Spot operation coroutine's physical and public
    // replies, and one projection of the selected mesh request.
    EXPECT_EQ (counts.payload_states, 2u);
    EXPECT_EQ (counts.message_states, 3u);
    EXPECT_EQ (counts.admission_sources, 1u);

    ASSERT_TRUE (pending.has_value ());
    EXPECT_TRUE (physical.complete (
      fw::result_t<transport_reply_t>::success (transport_reply_t::success (reply_parts (47)))));
    const auto &reply = pending->result ();
    ASSERT_TRUE (reply);
    EXPECT_EQ (reply.value ().value, 47);
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

} // namespace
