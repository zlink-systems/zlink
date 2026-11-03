/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "runtime/actors/actor_client.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/timers/async_delay.hpp"

#include <gtest/gtest.h>
#include <atomic>
#include <future>

namespace zlink::framework::runtime::mesh
{
struct raw_mesh_node_owner_test_access_t
{
    static void deliver (std::shared_ptr<foundation::operation_registry_t> registry,
                         foundation::call_id_t operation,
                         result_t<detail::backend::raw_request_completion_t> result,
                         raw_mesh_node_owner_t::physical_receiver_t receiver)
    {
        raw_mesh_node_owner_t::observe_request (std::move (registry), operation, 17,
                                                std::move (result), std::move (receiver));
    }
};
}

namespace zlink::framework::runtime::host
{
struct public_host_runtime_test_access_t
{
    static void complete (public_host_runtime_t &host,
                          const pending_operation_t &operation,
                          foundation::operation_terminal_t terminal,
                          result_t<zlink::message_t> payload)
    {
        auto record = host.decode_operation_record (operation.id, operation_kind_t::none, terminal,
                                                    payload.value ());
        operation.completion->complete (
          record ? result_t<operation_completion_t>::success ({record.value (), {}})
                 : detail::result_access_t::failure<operation_completion_t> (record.exception ()));
    }
    static void receive (public_host_runtime_t &host,
                         const pending_operation_t &operation,
                         foundation::operation_terminal_t terminal,
                         result_t<zlink::message_t> payload,
                         bool claimed)
    {
        operation.receive (host.decode_operation_completion (operation, operation_kind_t::none,
                                                             terminal, payload.value ()),
                           claimed);
    }
};
}

namespace
{
namespace fw = zlink::framework;
namespace rt = fw::runtime;
namespace fd = rt::foundation;
namespace wire = rt::protocol;
using namespace std::chrono_literals;
using access = rt::actor_client_test_access_t;
using terminal_t = fd::operation_terminal_t;

// The fake binding retains the same delivered-result callback as raw_route_port_t.
// Explicit expiration and delivery order the test without a timer or sleep.
class late_actor_fixture_t : public testing::Test
{
  protected:
    static fd::operation_registry_t::clock_t::time_point now () { return {}; }
    static rt::host::host_options_t host_options ()
    {
        rt::host::host_options_t options;
        options.mesh = fw::test::runtime_failure_options (options.mesh);
        options.mesh.descriptor = {"late-results",
                                   {'s'},
                                   1,
                                   1,
                                   "tcp://127.0.0.1:0",
                                   {{"actor", 100}},
                                   rt::mesh::service_node_state_t::preparing};
        options.mesh.descriptor.lifecycle_generation = 1;
        return options;
    }
    std::mutex delay_mutex;
    fw::detail::delay_owner_t delays{delay_mutex};
    rt::in_memory_location_repository_t store;
    rt::live_location_reader_t locations{store};
    fw::serializer_registry_t serializers;
    std::shared_ptr<fw::actor_client_t> client =
      rt::make_actor_client (locations, serializers, {}, {}, {}, delays);
    std::shared_ptr<fw::detail::mesh_node_runtime_t> runtime =
      std::make_shared<fw::detail::mesh_node_runtime_t> (
        std::make_shared<fw::detail::mesh_node_builder_state_t> ("mesh"));
    std::shared_ptr<rt::host::public_host_runtime_t> host =
      std::make_shared<rt::host::public_host_runtime_t> (host_options ());
    std::shared_ptr<fd::operation_registry_t> registry =
      std::make_shared<fd::operation_registry_t> (&now);
    wire::actor_route_fence_t fence{"actor", 3, {'n', 'o', 'd', 'e'}, 5, 7, 11};
    rt::host::pending_operation_t operation;
    rt::mesh::raw_mesh_node_owner_t::physical_receiver_t binding_receiver;
    std::atomic_int receiver_calls{0};
    std::atomic_int resource_releases{0};
    std::atomic_int payload_releases{0};
    std::promise<void> public_callback_entered;
    std::promise<void> received;
    std::future<void> received_future = received.get_future ();

    struct receiver_resource_t
    {
        std::atomic_int &releases;
        ~receiver_resource_t () { ++releases; }
    };

    static fw::request_task_t<fw::message_t>
    unwrap (fw::request_task_t<fw::result_t<fw::message_t>> selected)
    {
        auto reply = co_await fw::detail::request_submission (std::move (selected));
        co_return reply.value ();
    }

    fw::request_task_t<fw::message_t> prepare (std::shared_future<void> released = {})
    {
        access::cache (*client, fence);
        operation.id = {1, 17};
        auto selected =
          fw::detail::request_task_access_t::receive_public<fw::result_t<fw::message_t>> (
            [this] (auto receiver) {
                auto resource =
                  std::shared_ptr<receiver_resource_t> (new receiver_resource_t{resource_releases});
                auto observed = receiver.template project<fw::result_t<fw::message_t>> (
                  [this, resource = std::move (resource)] (const auto &reply) {
                      ++receiver_calls;
                      received.set_value ();
                      return reply;
                  });
                auto physical = access::prepare (*client, fence, std::move (observed));
                runtime->prepare_actor_request_reply (operation, physical);
                binding_receiver =
                  [host = host, operation = operation] (auto terminal, auto payload, bool claimed) {
                      rt::host::public_host_runtime_test_access_t::receive (
                        *host, operation, terminal, std::move (payload), claimed);
                  };
                physical.admitted->complete (fw::result_t<void>::success ());
            });
        auto public_operation = operation;
        public_operation.receiver = {};
        EXPECT_TRUE (registry->register_operation (
          operation.id, fd::operation_registry_t::clock_t::time_point{} + 1ms,
          [this, host = host, operation = std::move (public_operation), released] (auto terminal,
                                                                                   auto payload) {
              if (released.valid ()) {
                  public_callback_entered.set_value ();
                  released.wait ();
              }
              rt::host::public_host_runtime_test_access_t::complete (*host, operation, terminal,
                                                                     std::move (payload));
          }));
        // These copies are held by the binding and registry, as in production.
        operation.receiver = {};
        return unwrap (std::move (selected));
    }

    void expire ()
    {
        EXPECT_EQ (registry->expire (fd::operation_registry_t::clock_t::time_point{} + 1h)
                     .result ()
                     .value (),
                   1u);
    }

    void stale ()
    {
        struct payload_t
        {
            std::vector<std::uint8_t> bytes;
            std::atomic_int &released;
            ~payload_t () { ++released; }
        };
        auto payload = new payload_t{
          wire::encode_reply_header (
            17, static_cast<std::uint32_t> (wire::request_terminal_result::conflict),
            static_cast<std::uint32_t> (wire::framework_error_code::actorLocationStale)),
          payload_releases};
        auto header = zlink::advanced::external_message_t::from (
          std::span<std::uint8_t> (payload->bytes),
          [] (void *, void *hint) { delete static_cast<payload_t *> (hint); }, payload);
        fw::detail::backend::raw_request_completion_t completion{zlink::request_result_t::ok, {}};
        completion.parts.push_back (std::move (header));
        rt::mesh::raw_mesh_node_owner_test_access_t::deliver (
          registry, operation.id,
          fw::result_t<fw::detail::backend::raw_request_completion_t>::success (
            std::move (completion)),
          std::move (binding_receiver));
    }

    void expect_received ()
    {
        ASSERT_EQ (received_future.wait_for (2s), std::future_status::ready);
        EXPECT_EQ (receiver_calls, 1);
        EXPECT_EQ (resource_releases, 1);
        EXPECT_EQ (payload_releases, 1);
    }
};

class held_authority_store_t final : public rt::in_memory_location_repository_t
{
  public:
    fw::task_completion_source_t<fw::authority_read_result_t> authority;
    int reads = 0;

    fw::task_t<fw::authority_read_result_t> read_authority (fw::authority_key_t,
                                                            std::stop_token) override
    {
        ++reads;
        return authority.task ();
    }
};

TEST (ActorSendFence, MissingAuthorityInvalidatesOnlyTheUsedFence)
{
    // Hold the send's authority read after it selected its cached route, then
    // replace each fence component before allowing that send to fail.
    for (int changed = -1; changed != 5; ++changed) {
        SCOPED_TRACE (changed);
        std::mutex delay_mutex;
        fw::detail::delay_owner_t delays{delay_mutex};
        held_authority_store_t store;
        rt::live_location_reader_t locations{store};
        fw::serializer_registry_t serializers;
        auto runtime = std::make_shared<fw::detail::mesh_node_runtime_t> (
          std::make_shared<fw::detail::mesh_node_builder_state_t> ("mesh"));
        fw::location_options_t options;
        options.route_cache_max_age = 1h;
        auto client =
          rt::make_actor_client (locations, serializers, {runtime}, {}, options, delays);
        wire::actor_route_fence_t used{"actor", 3, {'n', 'o', 'd', 'e'}, 5, 7, 11};
        access::cache (*client, used);
        auto pending = client->send (fw::actor_id_t ("actor"), std::string ("command")).async ();
        ASSERT_EQ (store.reads, 1);
        ASSERT_FALSE (pending.await_ready ());
        auto current = used;
        switch (changed) {
            case 0:
                ++current.object_generation;
                break;
            case 1:
                current.target_node_routing_id.push_back ('2');
                break;
            case 2:
                ++current.target_node_generation;
                break;
            case 3:
                ++current.authority_owner_generation;
                break;
            case 4:
                ++current.owner_lease_generation;
                break;
        }
        access::cache (*client, current);
        store.authority.complete (
          fw::result_t<fw::authority_read_result_t>::success (fw::authority_missing_t{}));
        const auto result = pending.result ();
        ASSERT_FALSE (result);
        EXPECT_EQ (result.error_kind (), fw::framework_error_kind_t::not_found);
        EXPECT_EQ (access::cached (*client, used.actor_id), changed != -1);
    }
}

class selected_actor_client_t final : public fw::actor_client_t
{
  public:
    std::function<fw::request_task_t<fw::message_t> ()> select;
    fw::serializer_registry_t *registry;

  protected:
    fw::task_t<void> send_erased (fw::actor_id_t,
                                  std::string,
                                  fw::message_t,
                                  const fw::actor_send_call_t::metadata_map_t &) override
    {
        co_return;
    }
    fw::request_task_t<fw::message_t>
    request_erased (fw::actor_id_t,
                    std::string,
                    fw::message_t,
                    std::optional<std::chrono::milliseconds>,
                    const fw::actor_request_call_t::metadata_map_t &) override
    {
        return select ();
    }
    fw::serializer_registry_t &actor_client_serializers () override { return *registry; }
};

TEST_F (late_actor_fixture_t, LateSuccessDecodesWithOwnedCodecAfterPublicTimeout)
{
    auto codecs = std::make_unique<fw::serializer_registry_t> ();
    std::promise<void> decoded;
    auto decoded_future = decoded.get_future ();
    std::atomic_int decode_count{0};
    codecs->add<std::string> (
      [] (const std::string &text) { return fw::encoded_payload_t::from_string (text); },
      [&] (const fw::encoded_payload_t &payload) {
          ++decode_count;
          decoded.set_value ();
          return payload.to_string ();
      },
      "application/json");
    selected_actor_client_t facade;
    facade.select = [this] { return prepare (); };
    facade.registry = codecs.get ();
    auto pending =
      facade.request (fw::actor_id_t ("actor"), std::string ("request")).async<std::string> ();
    expire ();
    const auto before = pending.result ();
    ASSERT_FALSE (before);
    EXPECT_EQ (before.error_kind (), fw::framework_error_kind_t::deadline_exceeded);
    codecs.reset ();

    rt::messaging::envelope_header_t header;
    header.kind = rt::messaging::message_kind_t::response;
    header.content_type = "application/json";
    auto parts =
      fw::detail::encode_actor_reply (header, zlink::message_t::from (std::string ("\"late\"")));
    auto payload =
      wire::encode_application_payload (wire::application_payload_t::from_parts (parts.items ()));
    fw::detail::backend::raw_request_completion_t completion{zlink::request_result_t::ok, {}};
    completion.parts.push_back (zlink::message_t::from (wire::encode_reply_header (17, 0, 0)));
    completion.parts.push_back (zlink::message_t::from (payload));
    rt::mesh::raw_mesh_node_owner_test_access_t::deliver (
      registry, operation.id,
      fw::result_t<fw::detail::backend::raw_request_completion_t>::success (std::move (completion)),
      std::move (binding_receiver));
    ASSERT_EQ (decoded_future.wait_for (2s), std::future_status::ready);
    EXPECT_EQ (decode_count, 1);
    EXPECT_EQ (receiver_calls, 1);
    EXPECT_EQ (resource_releases, 1);
    EXPECT_TRUE (access::cached (*client, fence.actor_id));
    EXPECT_EQ (pending.result ().exception (), before.exception ());
}

TEST_F (late_actor_fixture_t, T1TimeoutThenDeliveredStaleInvalidatesMatchingFence)
{
    auto pending = prepare ();
    expire ();
    const auto before = pending.result ();
    ASSERT_FALSE (before);
    EXPECT_EQ (before.error_kind (), fw::framework_error_kind_t::deadline_exceeded);
    EXPECT_EQ (resource_releases, 0);
    EXPECT_TRUE (access::cached (*client, fence.actor_id));
    stale ();
    expect_received ();
    EXPECT_FALSE (access::cached (*client, fence.actor_id));
    EXPECT_EQ (pending.result ().exception (), before.exception ());
}

TEST_F (late_actor_fixture_t, PreparedReceiverRetainsClientUntilLateDeliveryFinishes)
{
    auto pending = prepare ();
    std::weak_ptr<fw::actor_client_t> retained = client;
    expire ();
    const auto before = pending.result ();
    ASSERT_FALSE (before);
    client.reset ();
    EXPECT_FALSE (retained.expired ());
    stale ();
    expect_received ();
    EXPECT_TRUE (retained.expired ());
    EXPECT_EQ (pending.result ().exception (), before.exception ());
}

TEST_F (late_actor_fixture_t, ClaimedTimeoutWinsBeforeItsPublicCallbackPublishes)
{
    std::promise<void> release;
    struct release_guard_t
    {
        std::promise<void> &release;
        ~release_guard_t ()
        {
            try {
                release.set_value ();
            }
            catch (const std::future_error &) {
            }
        }
    } guard{release};
    auto entered = public_callback_entered.get_future ();
    auto pending = prepare (release.get_future ().share ());
    expire ();
    ASSERT_EQ (entered.wait_for (2s), std::future_status::ready);
    stale ();
    EXPECT_FALSE (pending.await_ready ());
    EXPECT_EQ (receiver_calls, 0);
    EXPECT_TRUE (access::cached (*client, fence.actor_id));
    release.set_value ();
    const auto result = pending.result ();
    ASSERT_FALSE (result);
    EXPECT_EQ (result.error_kind (), fw::framework_error_kind_t::deadline_exceeded);
    ASSERT_EQ (received_future.wait_for (2s), std::future_status::ready);
    registry->shutdown ();
    expect_received ();
    EXPECT_FALSE (access::cached (*client, fence.actor_id));
}

TEST_F (late_actor_fixture_t, T2TimeoutThenDeliveredStalePreservesEveryNewerFence)
{
    // Every component of the request's route fence is significant.
    for (int changed = 0; changed != 5; ++changed) {
        SCOPED_TRACE (changed);
        if (changed != 0) {
            received = std::promise<void> ();
            received_future = received.get_future ();
            receiver_calls = 0;
            resource_releases = 0;
            payload_releases = 0;
            operation = rt::host::pending_operation_t{};
        }
        auto pending = prepare ();
        expire ();
        const auto before = pending.result ();
        ASSERT_FALSE (before);
        EXPECT_EQ (before.error_kind (), fw::framework_error_kind_t::deadline_exceeded);
        auto newer = fence;
        switch (changed) {
            case 0:
                ++newer.object_generation;
                break;
            case 1:
                newer.target_node_routing_id.push_back ('2');
                break;
            case 2:
                ++newer.target_node_generation;
                break;
            case 3:
                ++newer.authority_owner_generation;
                break;
            case 4:
                ++newer.owner_lease_generation;
                break;
        }
        access::cache (*client, newer);
        stale ();
        expect_received ();
        EXPECT_TRUE (access::cached (*client, fence.actor_id));
        EXPECT_EQ (pending.result ().exception (), before.exception ());
    }
}

TEST_F (late_actor_fixture_t, T3OrdinaryStaleInvalidatesOnce)
{
    auto pending = prepare ();
    stale ();
    const auto result = pending.result ();
    ASSERT_FALSE (result);
    EXPECT_EQ (result.error_kind (), fw::framework_error_kind_t::unavailable);
    registry->shutdown (); // Drain the public callback's payload ownership before checking release.
    expect_received ();
    EXPECT_FALSE (access::cached (*client, fence.actor_id));
    // The ordinary registry terminal must not apply the physical decision again.
    access::cache (*client, fence);
    EXPECT_EQ (
      registry->expire (fd::operation_registry_t::clock_t::time_point{} + 1h).result ().value (),
      0u);
    EXPECT_TRUE (access::cached (*client, fence.actor_id));
}
fw::request_task_t<int> map_selected_failure (fw::request_task_t<int> selected)
{
    try {
        co_return co_await fw::detail::request_submission (std::move (selected));
    }
    catch (const fw::framework_exception_t &) {
        co_return fw::result_t<int>::failure (fw::framework_error_kind_t::not_found, "mapped");
    }
}

TEST (ActorLatePublicResult, PhysicalFailureKeepsTheCallersErrorMapping)
{
    auto selected = fw::detail::request_task_access_t::receive_public<int> ([] (auto receiver) {
        receiver.complete (
          fw::result_t<int>::failure (fw::framework_error_kind_t::unavailable, "physical"));
    });
    auto pending = map_selected_failure (std::move (selected));
    EXPECT_EQ (pending.result ().error_kind (), fw::framework_error_kind_t::not_found);
}

TEST (ActorLatePublicResult, PublicFailureWinsEvenWhenPhysicalIsReadyBeforeObservation)
{
    auto selected = fw::detail::request_task_access_t::receive_public<int> ([] (auto receiver) {
        const auto timeout =
          fw::detail::boundary_failure<int> (fw::detail::boundary_error_t::timed_out, "timeout");
        receiver.fail_public (timeout.exception ());
        receiver.complete (
          fw::result_t<int>::failure (fw::framework_error_kind_t::unavailable, "late physical"));
    });
    auto pending = map_selected_failure (std::move (selected));
    EXPECT_EQ (pending.result ().error_kind (), fw::framework_error_kind_t::deadline_exceeded);
}

}
