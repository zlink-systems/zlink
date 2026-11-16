/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "runtime/actors/actor_client.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/execution/serial_application_record.hpp"
#include "runtime/stateful/raw_stateful_dispatch.hpp"

#include <gtest/gtest.h>
#include "creation_counter.hpp"
#include <zlink/Contracts/Core/context.hpp>
#include <array>
#include <atomic>
#include <future>
#include <filesystem>
#include <fstream>
#include <thread>
#include <nlohmann/json.hpp>

namespace zlink::framework::runtime::mesh
{
namespace
{
foundation::operation_registry_t::clock_t::time_point injected_registry_now;

foundation::operation_registry_t::clock_t::time_point registry_now ()
{
    return injected_registry_now;
}
}

struct raw_mesh_node_owner_test_access_t
{
    static void close_registry (raw_mesh_node_owner_t &owner) { owner._operations->shutdown (); }
    static void observe_registration (raw_mesh_node_owner_t &owner, std::atomic_bool &registered)
    {
        owner._operations = std::make_shared<foundation::operation_registry_t> (
          owner._lane, &foundation::operation_registry_t::clock_t::now, [&owner, &registered] {
              if (owner._operations->size () != 0)
                  registered = true;
          });
    }
    static void fail_publication (raw_mesh_node_owner_t &owner, std::atomic_bool &registered)
    {
        owner._operations = std::make_shared<foundation::operation_registry_t> (
          &foundation::operation_registry_t::clock_t::now, [&owner, &registered] {
              if (owner._operations->size () != 0)
                  registered = true;
              throw std::bad_alloc ();
          });
    }

    static void inject_registry_clock (raw_mesh_node_owner_t &owner)
    {
        injected_registry_now =
          foundation::operation_registry_t::clock_t::now () + std::chrono::hours (1);
        owner._operations =
          std::make_shared<foundation::operation_registry_t> (owner._lane, &registry_now);
    }
    static void reset_registry_clock ()
    {
        injected_registry_now =
          foundation::operation_registry_t::clock_t::now () + std::chrono::hours (1);
    }
    static void schedule_registry (raw_mesh_node_owner_t &owner)
    {
        owner._operations = foundation::operation_registry_t::create_scheduled (owner._lane);
    }
    static task_t<bool>
    submit_unadmitted (raw_mesh_node_owner_t &owner,
                       std::shared_ptr<foundation::operation_registry_t> registry,
                       foundation::operation_registry_t::callback_t callback,
                       raw_mesh_node_owner_t::physical_receiver_t receiver,
                       bool expired_deadline)
    {
        owner._operations = std::move (registry);
        auto expected = owner._options.descriptor;
        expected.node_routing_id = {'n'};
        expected.advertised_endpoint = "tcp://127.0.0.1:1";
        owner.expect_peer (std::move (expected));
        const std::array<std::uint8_t, 1> target{'n'};
        return owner.request_with_header (
          target,
          [] (std::uint64_t correlation) {
              return protocol::encode_actor_message_header_message (
                protocol::command::actorRequest, {}, {"actor", 3, {'n'}, 5, 7, 11},
                {1, correlation}, correlation);
          },
          {"request", "application/json", {123, 125}},
          foundation::operation_registry_t::clock_t::now ()
            + (expired_deadline ? -std::chrono::hours (1) : std::chrono::hours (1)),
          std::move (callback), 17, true, {}, {}, {}, std::move (receiver));
    }
    static void end_local (raw_mesh_node_owner_t &owner,
                           foundation::call_id_t id,
                           foundation::operation_terminal_t terminal)
    {
        if (terminal == foundation::operation_terminal_t::cancelled)
            EXPECT_TRUE (owner._operations->cancel (id));
        else if (terminal == foundation::operation_terminal_t::timed_out) {
            auto indexed = owner._lane.run ([] {});
            ASSERT_EQ (indexed.wait_for (std::chrono::seconds (2)), std::future_status::ready);
            indexed.get ();
            const auto deadline = owner._operations->next_deadline ();
            ASSERT_TRUE (deadline);
            EXPECT_EQ (owner._operations->expire (*deadline).result ().value (), 1);
        } else
            EXPECT_TRUE (owner.fail_local_operation (id, terminal));
    }
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

namespace zlink::framework::detail::test_creation_probe
{
std::size_t actor_request_submission_count () noexcept;
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

class submitted_actor_receiver_t : public testing::Test
{
  protected:
    rt::in_memory_location_repository_t store;
    rt::live_location_reader_t locations{store};
    fw::serializer_registry_t serializers;
    std::shared_ptr<fw::detail::mesh_node_runtime_t> runtime;
    std::shared_ptr<fw::actor_client_t> client;
    wire::actor_route_fence_t fence{"actor", 3, {'n'}, 1, 7, 11};
    std::atomic_bool registered{false};

    void SetUp () override
    {
        auto state =
          fw::test::runtime_failure_fixture<fw::detail::mesh_node_builder_state_t> ("mesh");
        state->listen_endpoint = "tcp://127.0.0.1:0";
        state->routing_id = zlink::routing_id_t::from ("submitted-actor-source");
        state->core_context = std::make_shared<zlink::context_t> ();
        runtime = std::make_shared<fw::detail::mesh_node_runtime_t> (state);
        runtime->start ().result ().value ();
        rt::mesh::raw_mesh_node_owner_test_access_t::observe_registration (
          runtime->native_node ().transport (), registered);
        serializers.add<std::string> (
          [] (const std::string &value) { return fw::encoded_payload_t::from_string (value); },
          [] (const fw::encoded_payload_t &value) { return value.to_string (); },
          "application/json");
        client = rt::make_actor_client (locations, serializers, {runtime}, {}, {});
        access::cache (*client, fence);
    }
    void TearDown () override
    {
        runtime->stop ();
        client.reset ();
    }
    void peer (bool ready)
    {
        auto &raw = runtime->native_node ().transport ();
        auto descriptor = raw.topology ().local_descriptor ();
        descriptor.node_routing_id = fence.target_node_routing_id;
        descriptor.lifecycle_generation = fence.target_node_generation;
        descriptor.advertised_endpoint = "tcp://127.0.0.1:1";
        EXPECT_EQ (raw.topology ().admit (descriptor, descriptor.node_routing_id,
                                          std::chrono::steady_clock::now (),
                                          ready ? wire::command::admit : wire::command::hello),
                   rt::mesh::peer_admission_result_t::admitted);
        if (!ready)
            raw.expect_peer (descriptor);
    }
    void verify (terminal_t terminal,
                 bool expected_registered = true,
                 std::optional<fw::framework_error_kind_t> expected_terminal = {},
                 bool expected_bad_alloc = false,
                 bool fails_source_registration = false)
    {
        namespace probe = fw::detail::test_creation_probe;
        const auto deliveries = probe::request_deliveries.load ();
        const auto capture_bytes = probe::request_capture_bytes.load ();
        const auto capture_values = probe::request_capture_value_bytes.load ();
        const auto block_bytes = probe::request_block_bytes.load ();
        const auto owner_bytes = probe::request_callback_owner_bytes.load ();
        auto pending = client->request (fw::actor_id_t ("actor"), std::string ("request"))
                         .timeout (5s)
                         .async<std::string> ();
        auto physical = fw::detail::request_task_access_t::physical_reply (pending);
        std::atomic_uint public_results{0};
        std::promise<void> public_set;
        auto published = public_set.get_future ();
        auto public_reply = fw::detail::request_task_access_t::public_reply (pending);
        fw::detail::observe_task_terminal (public_reply, [&] (const auto &) {
            ++public_results;
            public_set.set_value ();
        });
        if (terminal == terminal_t::timed_out) {
            ASSERT_FALSE (pending.admitted ().await_ready ());
            EXPECT_EQ (runtime->native_node ()
                         .transport ()
                         .expire_requests (std::chrono::steady_clock::now () + 6s)
                         .result ()
                         .value (),
                       1u);
        } else if (terminal == terminal_t::shutdown && expected_registered) {
            ASSERT_FALSE (pending.admitted ().await_ready ());
            runtime->stop ();
        }
        const auto result = pending.result ();
        EXPECT_FALSE (result);
        const auto received = physical.result ();
        EXPECT_FALSE (received);
        const auto expected_kind =
          terminal == terminal_t::timed_out  ? fw::framework_error_kind_t::deadline_exceeded
          : terminal == terminal_t::shutdown ? fw::framework_error_kind_t::shutting_down
                                             : fw::framework_error_kind_t::internal_failure;
        if (expected_bad_alloc) {
            // The publication failure is the original exception, not a Framework error kind.
            EXPECT_EQ (result.error (), nullptr);
            EXPECT_THROW (std::rethrow_exception (result.exception ()), std::bad_alloc);
        } else {
            EXPECT_EQ (result.error_kind (), expected_terminal.value_or (expected_kind));
        }
        EXPECT_EQ (registered.load (), expected_registered);
        runtime->stop ();
        if (fails_source_registration) {
            EXPECT_EQ (probe::request_deliveries.load () - deliveries, 0u);
            EXPECT_EQ (probe::request_capture_bytes.load (), capture_bytes);
            EXPECT_EQ (probe::request_block_bytes.load (), block_bytes);
        } else
            EXPECT_EQ (probe::request_deliveries.load () - deliveries, 1u);
        ASSERT_EQ (published.wait_for (2s), std::future_status::ready);
        EXPECT_EQ (public_results.load (), 1u);
        std::printf ("actor receiver registered=%d captures=%zu values=%zu block=%zu "
                     "callback-owner=%zu deliveries=%zu\n",
                     registered.load (), probe::request_capture_bytes.load () - capture_bytes,
                     probe::request_capture_value_bytes.load () - capture_values,
                     probe::request_block_bytes.load () - block_bytes,
                     probe::request_callback_owner_bytes.load () - owner_bytes,
                     probe::request_deliveries.load () - deliveries);
    }
};

TEST_F (submitted_actor_receiver_t, NativeAdmissionRejectionDeliversOnce)
{
    peer (true);
    verify (terminal_t::transport_failed, true, fw::framework_error_kind_t::unavailable);
}

TEST_F (submitted_actor_receiver_t, TimeoutWhileAdmissionPendingDeliversOnce)
{
    peer (false);
    verify (terminal_t::timed_out);
}

TEST_F (submitted_actor_receiver_t, ShutdownWhileAdmissionPendingDeliversOnce)
{
    peer (false);
    verify (terminal_t::shutdown);
}

TEST_F (submitted_actor_receiver_t, SourcePublicationExceptionEndsOnceWithoutPhysicalHandoff)
{
    peer (true);
    rt::mesh::raw_mesh_node_owner_test_access_t::fail_publication (
      runtime->native_node ().transport (), registered);
    verify (terminal_t::transport_failed, true, {}, true, true);
}

TEST_F (submitted_actor_receiver_t, SourceRegistrationRejectionEndsOnceWithoutPhysicalHandoff)
{
    rt::mesh::raw_mesh_node_owner_test_access_t::close_registry (
      runtime->native_node ().transport ());
    verify (terminal_t::shutdown, false, {}, false, true);
}

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

    rt::in_memory_location_repository_t store;
    rt::live_location_reader_t locations{store};
    fw::serializer_registry_t serializers;
    std::shared_ptr<fw::actor_client_t> client =
      rt::make_actor_client (locations, serializers, {}, {}, {});
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
    std::optional<std::pair<terminal_t, bool>> physical_terminal;
    std::promise<void> public_callback_entered;
    std::promise<void> received;
    std::future<void> received_future = received.get_future ();

    struct receiver_resource_t
    {
        std::atomic_int &releases;
        ~receiver_resource_t () { ++releases; }
    };

    static fw::request_task_t<fw::message_t>
    unwrap (fw::request_task_t<fw::result_t<fw::message_t>> selected,
            std::shared_ptr<void> lifetime = {})
    {
        auto reply = co_await fw::detail::request_submission (std::move (selected));
        co_return reply.value ();
    }

    fw::request_task_t<fw::message_t>
    prepare (std::shared_future<void> released = {},
             rt::mesh::raw_mesh_node_owner_t *unadmitted = nullptr,
             std::shared_ptr<void> lifetime = {},
             bool expired_deadline = false)
    {
        access::cache (*client, fence);
        operation.id = {1, 17};
        auto selected =
          fw::detail::request_task_access_t::receive_public<fw::result_t<fw::message_t>> (
            [this, unadmitted] (auto receiver) {
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
                binding_receiver = [this, host = host, operation = operation] (
                                     auto terminal, auto payload, bool claimed) {
                    physical_terminal = std::pair{terminal, claimed};
                    if (claimed)
                        EXPECT_FALSE (operation.completion->task ().await_ready ());
                    rt::host::public_host_runtime_test_access_t::receive (
                      *host, operation, terminal, std::move (payload), claimed);
                };
                if (!unadmitted)
                    physical.admitted->complete (fw::result_t<void>::success ());
            });
        auto public_operation = operation;
        public_operation.receiver = {};
        auto complete = [this, host = host, operation = std::move (public_operation),
                         released] (auto terminal, auto payload) {
            if (released.valid ()) {
                public_callback_entered.set_value ();
                released.wait ();
            }
            rt::host::public_host_runtime_test_access_t::complete (*host, operation, terminal,
                                                                   std::move (payload));
        };
        if (unadmitted) {
            auto submitting = rt::mesh::raw_mesh_node_owner_test_access_t::submit_unadmitted (
              *unadmitted, registry, std::move (complete), std::move (binding_receiver),
              expired_deadline);
            if (expired_deadline) {
                // Admission failure is published by the completion dispatcher.
                EXPECT_FALSE (submitting.result ());
            } else if (submitting.await_ready ()) {
                const auto result = submitting.result ();
                if (!result) {
                    try {
                        std::rethrow_exception (result.exception ());
                    }
                    catch (const std::exception &error) {
                        ADD_FAILURE () << error.what ();
                    }
                }
                EXPECT_FALSE (submitting.await_ready ());
            }
        } else {
            EXPECT_TRUE (registry->register_operation (
              operation.id, fd::operation_registry_t::clock_t::time_point{} + 1ms,
              std::move (complete)));
        }
        // These copies are held by the binding and registry, as in production.
        operation.receiver = {};
        return unwrap (std::move (selected), std::move (lifetime));
    }

    void expect_unadmitted_terminal (terminal_t terminal, bool expired_deadline = false)
    {
        rt::mesh::raw_mesh_node_owner_t owner (host_options ().mesh);
        owner.start ();
        auto frame_released = std::make_shared<std::promise<void>> ();
        auto released = frame_released->get_future ();
        auto lifetime =
          std::shared_ptr<void> (client.get (), [client = client, frame_released] (void *) mutable {
              client.reset ();
              frame_released->set_value ();
          });
        auto pending = prepare ({}, &owner, std::move (lifetime), expired_deadline);
        std::weak_ptr<fw::actor_client_t> retained = client;
        client.reset ();
        if (!expired_deadline)
            EXPECT_FALSE (retained.expired ());
        if (expired_deadline)
            EXPECT_FALSE (registry->contains (operation.id));
        else if (terminal == terminal_t::timed_out)
            ASSERT_EQ (
              registry->expire (fd::operation_registry_t::clock_t::now () + 2h).result ().value (),
              1u);
        else if (terminal == terminal_t::cancelled)
            ASSERT_TRUE (registry->cancel (operation.id));
        else if (terminal == terminal_t::shutdown)
            ASSERT_EQ (registry->shutdown (), 1u);
        else
            ASSERT_EQ (registry->fail_target ({'n'}, terminal), 1u);
        const auto failure = pending.result ();
        ASSERT_FALSE (failure);
        if (terminal == terminal_t::timed_out)
            EXPECT_EQ (failure.error_kind (), fw::framework_error_kind_t::deadline_exceeded);
        registry->shutdown ();
        // Task result publication precedes coroutine-frame destruction. This owned
        // deleter proves the physical await finished and released its frame state.
        ASSERT_EQ (released.wait_for (2s), std::future_status::ready);
        EXPECT_EQ (receiver_calls, 1);
        ASSERT_TRUE (physical_terminal);
        EXPECT_EQ (physical_terminal->first, terminal);
        EXPECT_EQ (physical_terminal->second, expired_deadline);
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

    fw::task_t<fw::authority_read_result_t> read_authority_core (fw::authority_key_t,
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

        held_authority_store_t store;
        rt::live_location_reader_t locations{store};
        fw::serializer_registry_t serializers;
        auto runtime = std::make_shared<fw::detail::mesh_node_runtime_t> (
          std::make_shared<fw::detail::mesh_node_builder_state_t> ("mesh"));
        fw::location_options_t options;
        options.route_cache_max_age = 1h;
        auto client = rt::make_actor_client (locations, serializers, {runtime}, {}, options);
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

class counting_actor_authority_store_t final : public rt::in_memory_location_repository_t
{
  public:
    std::atomic_int reads{0};
    fw::task_t<fw::authority_read_result_t>
    read_authority_core (fw::authority_key_t key, std::stop_token cancellation) override
    {
        ++reads;
        return rt::in_memory_location_repository_t::read_authority_core (std::move (key),
                                                                         cancellation);
    }
};

// These requests use the production client, local host registration and dispatcher.
// Only the registry clock/cancellation is controlled by the fixture.
class local_actor_receiver_t : public testing::Test
{
  protected:
    counting_actor_authority_store_t store;
    rt::live_location_reader_t locations{store};
    fw::serializer_registry_t serializers;
    std::shared_ptr<fw::detail::mesh_node_runtime_t> runtime;
    std::shared_ptr<fw::actor_client_t> client;
    std::optional<rt::host::receive_record_t> request;
    std::function<void ()> before_submit;
    wire::actor_route_fence_t fence;

    void SetUp () override
    {
        auto state =
          fw::test::runtime_failure_fixture<fw::detail::mesh_node_builder_state_t> ("mesh");
        state->listen_endpoint = "tcp://127.0.0.1:0";
        state->routing_id = zlink::routing_id_t::from ("local-receiver");
        state->core_context = std::make_shared<zlink::context_t> ();
        state->spot_state->snapshot.actor_types.push_back ("late-result");
        runtime = std::make_shared<fw::detail::mesh_node_runtime_t> (state);
        runtime->configure_stateful_dispatch (
          [] (const rt::stateful::accepted_record_authority_query_t &query)
            -> fw::task_t<std::optional<rt::stateful::accepted_record_authority_t>> {
              co_return rt::stateful::accepted_record_authority_t{
                {"owner", 1, query.source_node_routing_id, query.source_node_generation}, 1};
          });
        runtime->start ().result ().value ();
        rt::mesh::raw_mesh_node_owner_test_access_t::inject_registry_clock (
          runtime->native_node ().transport ());
        const auto actor = runtime->create_actor ("late-result", "actor");
        fence = {"actor",
                 actor.ref ().object_generation (),
                 runtime->status ().routing_id ().to_bytes (),
                 runtime->status ().lifecycle_generation (),
                 1,
                 1};
        serializers.add<std::string> (
          [this] (const std::string &value) {
              if (before_submit)
                  before_submit ();
              return fw::encoded_payload_t::from_string (value);
          },
          [] (const fw::encoded_payload_t &value) { return value.to_string (); },
          "application/json");
        client = rt::make_actor_client (locations, serializers, {runtime}, {}, {});
        access::cache (*client, fence);
    }

    void TearDown () override
    {
        request.reset ();
        client.reset ();
        runtime->stop ();
    }

    void dispatch_request ()
    {
        const auto deadline = std::chrono::steady_clock::now () + 2s;
        while (!request && std::chrono::steady_clock::now () < deadline) {
            const auto dispatched = runtime
                                      ->dispatch_ready ([this] (const auto &, const auto &record,
                                                                auto, auto) { request = record; })
                                      .result ();
            ASSERT_TRUE (dispatched);
            std::this_thread::yield ();
        }
        ASSERT_TRUE (request);
    }

    void end (terminal_t terminal)
    {
        rt::mesh::raw_mesh_node_owner_test_access_t::end_local (
          runtime->native_node ().transport (), request->operation_id, terminal);
    }

    void expect_released (const std::weak_ptr<fw::actor_client_t> &retained)
    {
        client.reset ();
        const auto deadline = std::chrono::steady_clock::now () + 2s;
        while (!retained.expired () && std::chrono::steady_clock::now () < deadline)
            std::this_thread::yield ();
        EXPECT_TRUE (retained.expired ());
    }
};

TEST_F (local_actor_receiver_t, ExpiredLocalContinuationPreservesTheRegisteredDeadlineTerminal)
{
    for (int iteration = 0; iteration != 32; ++iteration) {
        SCOPED_TRACE (iteration);
        auto pending = client->request (fw::actor_id_t ("actor"), std::string ("request"))
                         .timeout (5s)
                         .async<std::string> ();
        const auto result = pending.result ();
        ASSERT_FALSE (result);
        EXPECT_EQ (result.error_kind (), fw::framework_error_kind_t::deadline_exceeded);
        auto physical = fw::detail::request_task_access_t::physical_reply (pending);
        const auto physical_result = physical.result ();
        EXPECT_FALSE (physical_result);
        EXPECT_EQ (physical_result.error_kind (), fw::framework_error_kind_t::deadline_exceeded);
        EXPECT_EQ (runtime->pending_transport_operations (), 0u);
    }
}

TEST_F (local_actor_receiver_t, RejectedLocalRegistrationDeliversPhysicalFailureOnce)
{
    namespace probe = fw::detail::test_creation_probe;
    const auto deliveries = probe::request_deliveries.load ();
    before_submit = [this] {
        rt::mesh::raw_mesh_node_owner_test_access_t::close_registry (
          runtime->native_node ().transport ());
    };
    auto pending = client->request (fw::actor_id_t ("actor"), std::string ("request"))
                     .timeout (5s)
                     .async<std::string> ();
    std::atomic_uint public_results{0};
    auto public_reply = fw::detail::request_task_access_t::public_reply (pending);
    fw::detail::observe_task_terminal (public_reply, [&] (const auto &) { ++public_results; });
    EXPECT_FALSE (pending.result ());
    auto physical = fw::detail::request_task_access_t::physical_reply (pending);
    ASSERT_TRUE (physical.await_ready ());
    EXPECT_FALSE (physical.result ());
    runtime->stop ();
    EXPECT_EQ (probe::request_deliveries.load () - deliveries, 1u);
    EXPECT_EQ (public_results.load (), 1u);
}

TEST_F (local_actor_receiver_t, LocalCompletionSinkConstructionFailureDeliversPhysicalFailureOnce)
{
    namespace probe = fw::detail::test_creation_probe;
    const auto deliveries = probe::request_deliveries.load ();
    struct clear_fault_t
    {
        ~clear_fault_t ()
        {
            zlink::framework::detail::test_creation_probe::local_completion_sink_fault.store (
              nullptr);
        }
    } clear_fault;
    probe::local_completion_sink_fault.store (+[] { throw std::bad_alloc (); });

    auto pending = client->request (fw::actor_id_t ("actor"), std::string ("request"))
                     .timeout (5s)
                     .async<std::string> ();
    auto physical = fw::detail::request_task_access_t::physical_reply (pending);
    const auto public_deadline = std::chrono::steady_clock::now () + 2s;
    while (!pending.await_ready () && std::chrono::steady_clock::now () < public_deadline)
        std::this_thread::yield ();
    ASSERT_TRUE (pending.await_ready ());
    EXPECT_FALSE (pending.result ());

    const auto physical_deadline = std::chrono::steady_clock::now () + 2s;
    while (!physical.await_ready () && std::chrono::steady_clock::now () < physical_deadline)
        std::this_thread::yield ();
    const auto physical_ready = physical.await_ready ();
    EXPECT_TRUE (physical_ready);
    if (physical_ready)
        EXPECT_FALSE (physical.result ());
    runtime->stop ();
    EXPECT_EQ (probe::request_deliveries.load () - deliveries, 1u);
    EXPECT_EQ (probe::local_completion_sink_fault.load (), nullptr);
}

// Each request selects the old cache entry before the serializer runs. Replacing
// that entry in the serializer gives a deterministic selection/submission boundary.
TEST_F (local_actor_receiver_t, MissingDescriptorTimeoutPreservesEveryCachedFence)
{
    // Wait for the registered deadline; a manual expiry would race the scheduler.
    // The expired-continuation contract above retains the fixture's future clock.
    rt::mesh::raw_mesh_node_owner_test_access_t::schedule_registry (
      runtime->native_node ().transport ());
    for (int changed = -1; changed != 5; ++changed) {
        SCOPED_TRACE (changed);
        auto used = fence;
        used.target_node_routing_id = {'m', 'i', 's', 's', 'i', 'n', 'g'};
        access::cache (*client, used);
        auto newer = used;
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
        before_submit = [&, newer] { access::cache (*client, newer); };
        store.reads = 0;
        auto pending = client->request (fw::actor_id_t ("actor"), std::string ("request"))
                         .timeout (5ms)
                         .async<std::string> ();
        const auto result = pending.result ();
        ASSERT_FALSE (result);
        EXPECT_EQ (result.error_kind (), fw::framework_error_kind_t::deadline_exceeded);
        EXPECT_EQ (store.reads, 0);
        EXPECT_TRUE (access::cached (*client, "actor"));
        before_submit = {};
    }
}

TEST_F (local_actor_receiver_t, SubmitFenceMismatchInvalidatesOnlyTheUsedFenceForTheNextRequest)
{
    auto &transport = runtime->native_node ().transport ();
    auto peer = transport.topology ().local_descriptor ();
    peer.node_routing_id = {'p', 'e', 'e', 'r'};
    peer.advertised_endpoint = "tcp://127.0.0.1:1";
    ASSERT_EQ (transport.topology ().admit (peer, {'c'}),
               rt::mesh::peer_admission_result_t::admitted);
    for (int changed = -1; changed != 5; ++changed) {
        SCOPED_TRACE (changed);
        auto used = fence;
        used.target_node_routing_id = peer.node_routing_id;
        // An authority fence without its lease is rejected before binding submit.
        used.owner_lease_generation = 0;
        access::cache (*client, used);
        auto newer = used;
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
        before_submit = [&, newer] { access::cache (*client, newer); };
        store.reads = 0;
        auto pending = client->request (fw::actor_id_t ("actor"), std::string ("request"))
                         .timeout (1h)
                         .async<std::string> ();
        const auto result = pending.result ();
        ASSERT_FALSE (result);
        EXPECT_EQ (result.error_kind (), fw::framework_error_kind_t::not_found);
        // Cache invalidation belongs to the physical result, which can follow C1 publication.
        auto physical = fw::detail::request_task_access_t::physical_reply (pending);
        EXPECT_FALSE (physical.result ());
        EXPECT_EQ (store.reads, 0);
        if (changed == -1) {
            // Actor model §9 resolves Ready authority once per logical request.
            // Only a new operation resolves the invalidated route again.
            before_submit = {};
            auto next = client->request (fw::actor_id_t ("actor"), std::string ("request"))
                          .timeout (1h)
                          .async<std::string> ();
            const auto next_result = next.result ();
            ASSERT_FALSE (next_result);
            EXPECT_EQ (next_result.error_kind (), fw::framework_error_kind_t::not_found);
            auto next_physical = fw::detail::request_task_access_t::physical_reply (next);
            EXPECT_FALSE (next_physical.result ());
        }
        EXPECT_EQ (store.reads, changed == -1 ? 1 : 0);
        EXPECT_EQ (access::cached (*client, "actor"), changed != -1);
        before_submit = {};
    }
}

TEST_F (local_actor_receiver_t, LocalStaleReplyInvalidatesOnlyTheUsedFence)
{
    for (int changed = -1; changed != 5; ++changed) {
        SCOPED_TRACE (changed);
        access::cache (*client, fence);
        auto pending = client->request (fw::actor_id_t ("actor"), std::string ("request"))
                         .timeout (1h)
                         .async<std::string> ();
        dispatch_request ();
        ASSERT_TRUE (request);
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
        rt::messaging::envelope_header_t header;
        header.kind = rt::messaging::message_kind_t::error;
        header.error_code = "unavailable";
        header.error_message = "old Actor route is unavailable";
        auto reply = fw::detail::encode_actor_reply (header, {});
        EXPECT_TRUE (runtime->native_node ().reply (request->reply_token, reply.items ()));
        EXPECT_FALSE (pending.result ());
        EXPECT_EQ (access::cached (*client, "actor"), changed != -1);
        request.reset ();
    }
}

TEST_F (local_actor_receiver_t, PublicRequestReleasesClientAfterNoReply)
{
    auto pending = client->request (fw::actor_id_t ("actor"), std::string ("request"))
                     .timeout (1h)
                     .async<std::string> ();
    dispatch_request ();
    ASSERT_TRUE (request);
    ASSERT_TRUE (request->application_record);
    ASSERT_TRUE (request->application_record->native_reply);
    EXPECT_TRUE (request->application_record->native_reply (std::nullopt));
    EXPECT_FALSE (pending.result ());
    EXPECT_TRUE (access::cached (*client, "actor"));
    expect_released (client);
}

TEST_F (local_actor_receiver_t, PublicRequestReleasesClientAfterTimeout)
{
    rt::mesh::raw_mesh_node_owner_test_access_t::reset_registry_clock ();
    auto pending = client->request (fw::actor_id_t ("actor"), std::string ("request"))
                     .timeout (1h)
                     .async<std::string> ();
    dispatch_request ();
    ASSERT_TRUE (request);
    end (terminal_t::timed_out);
    EXPECT_EQ (pending.result ().error_kind (), fw::framework_error_kind_t::deadline_exceeded);
    EXPECT_TRUE (access::cached (*client, "actor"));
    expect_released (client);
}

TEST_F (local_actor_receiver_t, PublicRequestReleasesClientAfterCancel)
{
    auto pending = client->request (fw::actor_id_t ("actor"), std::string ("request"))
                     .timeout (1h)
                     .async<std::string> ();
    dispatch_request ();
    ASSERT_TRUE (request);
    end (terminal_t::cancelled);
    EXPECT_FALSE (pending.result ());
    EXPECT_TRUE (access::cached (*client, "actor"));
    expect_released (client);
}

TEST_F (local_actor_receiver_t, PublicRequestReleasesClientAfterForcedStop)
{
    auto pending = client->request (fw::actor_id_t ("actor"), std::string ("request"))
                     .timeout (1h)
                     .async<std::string> ();
    dispatch_request ();
    ASSERT_TRUE (request);
    runtime->stop ();
    EXPECT_EQ (pending.result ().error_kind (), fw::framework_error_kind_t::shutting_down);
    expect_released (client);
}

TEST_F (local_actor_receiver_t, LocalRegistryTerminalsReachPreparedReceiverOnce)
{
    for (auto terminal : {terminal_t::transport_failed, terminal_t::timed_out,
                          terminal_t::cancelled, terminal_t::shutdown, terminal_t::completed}) {
        SCOPED_TRACE (static_cast<int> (terminal));
        rt::host::pending_operation_t operation;
        std::atomic_int calls{0};
        operation.receiver = [&] (const auto &) { ++calls; };
        auto actor = fw::detail::actor_ref_access_t::make (
          fw::node_rid_t::from_string (runtime->status ().routing_id ().to_string ()),
          "late-result", "actor", fence.object_generation);
        std::vector<zlink::message_t> parts{zlink::message_t::from (std::string ("request"))};
        rt::mesh::raw_mesh_node_owner_test_access_t::inject_registry_clock (
          runtime->native_node ().transport ());
        auto submitted = runtime->request_to_actor (actor, parts, operation, 1h);
        const auto submit_result = submitted.result_for (2s);
        ASSERT_TRUE (submit_result && *submit_result);
        ASSERT_EQ (submit_result->value (), zlink::submit_result_t::ok);
        dispatch_request ();
        ASSERT_TRUE (request);
        if (terminal == terminal_t::completed)
            EXPECT_TRUE (runtime->native_node ().reply (request->reply_token, parts));
        else if (terminal == terminal_t::transport_failed) {
            ASSERT_TRUE (request->application_record);
            EXPECT_TRUE (request->application_record->native_reply (std::nullopt));
        } else
            end (terminal);
        const auto completion_result = operation.completion->task ().result_for (2s);
        ASSERT_TRUE (completion_result);
        (void) *completion_result;
        EXPECT_EQ (calls, 1);
        EXPECT_FALSE (runtime->native_node ().transport ().fail_local_operation (
          operation.id, terminal_t::transport_failed));
        EXPECT_EQ (calls, 1);
        // End the held handler turn before submitting the next case. The
        // registry terminal is already fixed and this cannot publish another result.
        if (request->application_record)
            (void) request->application_record->native_reply (std::nullopt);
        request.reset ();
        EXPECT_EQ (calls, 1);
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

TEST_F (late_actor_fixture_t, UnadmittedRequestReleasesPhysicalReceiverAfterTimeout)
{
    expect_unadmitted_terminal (terminal_t::timed_out);
}

TEST_F (late_actor_fixture_t, UnadmittedRequestReleasesPhysicalReceiverAfterCancellation)
{
    expect_unadmitted_terminal (terminal_t::cancelled);
}

TEST_F (late_actor_fixture_t, UnadmittedRequestReleasesPhysicalReceiverAfterShutdown)
{
    expect_unadmitted_terminal (terminal_t::shutdown);
}

TEST_F (late_actor_fixture_t, UnadmittedRequestReleasesPhysicalReceiverAfterRouteFailure)
{
    expect_unadmitted_terminal (terminal_t::route_unavailable);
}

TEST_F (late_actor_fixture_t, ExpiredAdmissionClaimsPhysicalReceiverBeforePublicDispatch)
{
    expect_unadmitted_terminal (terminal_t::timed_out, true);
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

namespace
{
class held_actor_authority_store_t final : public fw::location_store_t
{
  public:
    fw::task_t<fw::store_read_result_t> read (fw::store_key_t key) override
    {
        if (hold
            && key.value
                 == (std::string ("authority") + '\0' + "actor" + '\0' + "deadline-actor")) {
            entered.set_value ();
            co_await release.task ();
        }
        co_return co_await inner.read (std::move (key));
    }
    fw::task_t<fw::store_write_result_t> write (fw::store_write_request_t request) override
    {
        return inner.write (std::move (request));
    }
    fw::task_t<fw::store_scan_result_t> scan (fw::store_scan_request_t request) override
    {
        return inner.scan (std::move (request));
    }
    rt::in_memory_location_store_t inner;
    bool hold = false;
    std::promise<void> entered;
    fw::task_completion_source_t<void> release;
};

TEST (ActorRequestLookupDeadline, DeadlineIncludesPendingAuthorityReadAndPreventsLateSubmit)
{
    auto root = std::filesystem::path (__FILE__).parent_path ();
    const auto relative =
      std::filesystem::path ("runtime/conformance/actor-request-lookup-deadline-v1.json");
    while (!root.empty () && !std::filesystem::is_regular_file (root / relative)) {
        const auto parent = root.parent_path ();
        if (parent == root)
            root.clear ();
        else
            root = parent;
    }
    ASSERT_FALSE (root.empty ());
    std::ifstream input (root / relative);
    const auto fixture = nlohmann::json::parse (input);
    held_actor_authority_store_t opaque;
    rt::provider_location_repository_t repository (opaque);
    auto owner = std::get<fw::owner_lease_claimed_t> (
                   repository.claim_owner_lease ("deadline-owner", 60s).result ().value ())
                   .token;
    fw::mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = "mesh";
    descriptor.rid = zlink::routing_id_t::from ("deadline-target");
    descriptor.lifecycle_generation = 1;
    descriptor.descriptor_revision = 1;
    descriptor.endpoint = "tcp://127.0.0.1:1";
    descriptor.owner_id = owner.owner_id;
    descriptor.lease_generation = owner.lease_generation;
    descriptor.object_role = fw::object_role_t::server;
    descriptor.state = fw::framework_runtime_state_t::serving;
    descriptor.application_version = 1;
    descriptor.security_identity = "deadline-target";
    descriptor.entry_spot_id = "deadline-entry";
    descriptor.activation_concurrency.limit = 1;
    descriptor.object_capabilities.push_back ({fw::placement_object_kind_t::actor, "player",
                                               fw::maintenance_policy_kind_t::disabled, false, 0});
    descriptor.capacity.actors.limit = 1;
    ASSERT_TRUE (
      repository.update_mesh_node (descriptor, fw::location_write_intent_t::new_claim).result ());
    fw::object_reserve_request_t request;
    request.key = {fw::placement_object_kind_t::actor, "deadline-actor"};
    request.intent.stable_type = "player";
    request.target = {"mesh", fw::node_rid_t::from_string ("deadline-target"), 1, owner};
    request.capacity_bundle.actor_slots = 1;
    auto reserved =
      std::get<fw::object_reserved_t> (repository.reserve (request).result ().value ());
    auto ready = rt::encode_actor_authority_payload (rt::actor_authority_payload_t{
      .state = rt::actor_authority_state_t::ready,
      .stable_type = "player",
      .actor_id = "deadline-actor",
      .current_spot_id = "deadline-entry",
      .current_spot_generation = 1,
      .current_spot_kind = rt::actor_authority_spot_kind_t::entry,
      .owner_id = owner.owner_id,
      .owner_lease_generation = static_cast<std::uint64_t> (owner.lease_generation),
      .mesh_name = "mesh",
      .node_rid = request.target.node_rid,
      .node_generation = 1});
    ASSERT_TRUE (repository.commit ({request.key, reserved.fence, ready}).result ());
    rt::live_location_reader_t locations (repository);

    fw::serializer_registry_t serializers;
    auto state = fw::test::runtime_failure_fixture<fw::detail::mesh_node_builder_state_t> ("mesh");
    state->listen_endpoint = "tcp://127.0.0.1:0";
    state->routing_id = zlink::routing_id_t::from ("deadline-source");
    state->core_context = std::make_shared<zlink::context_t> ();
    auto runtime = std::make_shared<fw::detail::mesh_node_runtime_t> (state);
    runtime->start ().result ().value ();
    struct stop_t
    {
        std::shared_ptr<fw::detail::mesh_node_runtime_t> runtime;
        ~stop_t () { runtime->stop (); }
    } stop{runtime};
    fw::location_options_t options;
    options.route_cache_max_age = 0ms;
    auto client = rt::make_actor_client (locations, serializers, {runtime}, {}, options);
    const auto submissions = fw::detail::test_creation_probe::actor_request_submission_count ();
    opaque.hold = true;
    auto entered = opaque.entered.get_future ();
    auto call = client->request (fw::actor_id_t ("deadline-actor"), std::string ("ping"))
                  .timeout (std::chrono::milliseconds (fixture.at ("timeoutMs").get<int> ()))
                  .async<std::string> ();
    auto public_reply = fw::detail::request_task_access_t::public_reply (call);
    std::promise<fw::result_t<std::string>> terminal;
    auto terminal_result = terminal.get_future ();
    fw::detail::observe_task_terminal (
      public_reply, [&terminal] (const auto &value) { terminal.set_value (value); });
    EXPECT_EQ (entered.wait_for (5s), std::future_status::ready);
    const auto status = terminal_result.wait_for (
      std::chrono::milliseconds (fixture.at ("observationMs").get<int> ()));
    EXPECT_EQ (status, std::future_status::ready)
      << "request must end before Store read is released";
    EXPECT_EQ (fw::detail::test_creation_probe::actor_request_submission_count (), submissions);
    opaque.release.complete (fw::result_t<void>::success ());
    const auto result = terminal_result.get ();
    EXPECT_FALSE (result);
    EXPECT_EQ (result.error_kind (), fw::framework_error_kind_t::deadline_exceeded);
    auto physical = fw::detail::request_task_access_t::physical_reply (call);
    (void) physical.result ();
    EXPECT_EQ (fw::detail::test_creation_probe::actor_request_submission_count (), submissions);
    EXPECT_EQ (runtime->pending_transport_operations (), 0u);
}
}

namespace
{
TEST (ActorRequestOperationOwner, CanonicalReplyKeepsTheOriginalRegistryAcrossPhysicalMeshes)
{
    auto original_options = fw::test::runtime_failure_options (rt::mesh::raw_mesh_node_options_t{});
    original_options.descriptor = {"lookup-original",
                                   {'o'},
                                   41,
                                   1,
                                   "tcp://127.0.0.1:0",
                                   {{"actor", 100}},
                                   rt::mesh::service_node_state_t::preparing};
    auto physical_options = original_options;
    physical_options.descriptor.mesh_name = "lookup-physical";
    physical_options.descriptor.node_routing_id = {'p'};
    physical_options.descriptor.lifecycle_generation = 43;
    rt::mesh::raw_mesh_node_owner_t original (original_options);
    rt::mesh::raw_mesh_node_owner_t physical (physical_options);
    auto lease = std::make_shared<fw::location_owner_token_t> ();
    lease->owner_id = "lookup-process";
    lease->lease_generation = 1;
    original.configure_request_sources (nullptr, [lease] { return lease; });
    physical.configure_request_sources (nullptr, [lease] { return lease; });
    original.start ();
    physical.start ();
    const auto reservation = original.prepare_local_operation_async ().result ().value ();
    ASSERT_TRUE (reservation);
    auto terminal = std::make_shared<std::promise<terminal_t>> ();
    auto received = terminal->get_future ();
    auto calls = std::make_shared<std::atomic_uint> (0);
    const auto deadline = fd::operation_registry_t::clock_t::now () + 30s;
    ASSERT_TRUE (original.register_prepared_local_operation (
      *reservation, deadline,
      [terminal, calls] (auto ended, auto) {
          ++*calls;
          terminal->set_value (ended);
      },
      rt::mesh_request_surface_t::actor));
    const auto id = reservation->first;
    const wire::wire_operation_id_t wire_id{id.high, id.low};
    EXPECT_FALSE (physical.relocated_request_source (wire_id, id.low));
    physical.set_operation_owner_resolver ([&] (auto operation, auto reply_route_id) {
        if (auto owner = original.local_operation_owner (operation, reply_route_id))
            return owner;
        return physical.local_operation_owner (operation, reply_route_id);
    });
    const auto source = physical.relocated_request_source (wire_id, id.low);
    ASSERT_TRUE (source);
    EXPECT_EQ (source->node_routing_id, physical_options.descriptor.node_routing_id);
    EXPECT_EQ (source->node_generation, physical_options.descriptor.lifecycle_generation);
    EXPECT_EQ (source->owner_id, lease->owner_id);
    EXPECT_EQ (original.pending_operation_count (), 1u);
    EXPECT_EQ (physical.pending_operation_count (), 0u);
    EXPECT_EQ (original.operation_registry ()->deadline (id), deadline);
    wire::reply_relay_t relay;
    relay.operation = wire_id;
    relay.reply_route_id = id.low;
    const wire::application_payload_t reply{"reply", "application/json", {123, 125}};
    auto wrong = relay;
    ++wrong.operation.high;
    EXPECT_FALSE (physical.complete_relocated_request (wrong, reply));
    ASSERT_TRUE (physical.complete_relocated_request (relay, reply));
    ASSERT_EQ (received.wait_for (5s), std::future_status::ready);
    EXPECT_EQ (received.get (), terminal_t::completed);
    EXPECT_EQ (calls->load (), 1u);
    EXPECT_EQ (original.pending_operation_count (), 0u);
    EXPECT_EQ (physical.pending_operation_count (), 0u);
    EXPECT_FALSE (physical.complete_relocated_request (relay, reply));
    EXPECT_EQ (original.expire_requests (deadline + 1s).result ().value (), 0u);
}

TEST (ActorRequestOperationOwner, ConcurrentRegistryLookupReleasesStoppedMesh)
{
    auto state = fw::test::runtime_failure_fixture<fw::detail::mesh_node_builder_state_t> ("mesh");
    state->listen_endpoint = "tcp://127.0.0.1:0";
    state->routing_id = zlink::routing_id_t::from ("lookup-lifecycle-source");
    state->core_context = std::make_shared<zlink::context_t> ();
    auto runtime = std::make_shared<fw::detail::mesh_node_runtime_t> (state);
    auto lease = std::make_shared<fw::location_owner_token_t> ();
    lease->owner_id = "lookup-lifecycle";
    lease->lease_generation = 1;
    runtime->configure_stateful_dispatch ([lease] { return lease; });
    runtime->start ().result ().value ();
    rt::host::pending_operation_t operation;
    auto admitted = std::make_shared<fw::task_completion_source_t<void>> ();
    runtime->register_actor_request (operation, 1h, admitted).result ().value ();
    ASSERT_TRUE (runtime->local_operation_owner (operation.id, operation.id.low));
    std::promise<void> entered;
    auto started = entered.get_future ();
    std::atomic_uint visits{0};
    std::jthread reader ([&] (std::stop_token stop) {
        do {
            if (auto owner = runtime->local_operation_owner (operation.id, operation.id.low)) {
                EXPECT_EQ (owner->first, operation.registry);
                EXPECT_EQ (owner->second, operation.id);
            }
            if (visits.fetch_add (1) == 0)
                entered.set_value ();
            std::this_thread::yield ();
        } while (!stop.stop_requested ());
    });
    started.get ();
    runtime->stop ();
    reader.request_stop ();
    reader.join ();
    EXPECT_GT (visits.load (), 0u);
    EXPECT_FALSE (runtime->local_operation_owner (operation.id, operation.id.low));
    EXPECT_FALSE (operation.registry->contains (operation.id));
}

}
