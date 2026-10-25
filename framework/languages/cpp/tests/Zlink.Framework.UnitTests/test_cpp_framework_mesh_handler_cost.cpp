/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <zlink/framework.hpp>

#include "allocation_counter.hpp"
#include "runtime/channels/route_handler_registry.hpp"
#include "runtime/mesh/mesh_record_dispatcher.hpp"
#include "runtime/stateful/public_host_runtime.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace fw = zlink::framework;
namespace
{
using namespace std::chrono_literals;
constexpr std::array<const char *, 8> channel_names{"cost-s0s", "cost-s0a", "cost-s1s", "cost-s1a",
                                                    "cost-r0s", "cost-r0a", "cost-r1s", "cost-r1a"};
constexpr auto startup_timeout = 5s;
constexpr auto request_timeout = 1s;
constexpr int sample_rounds = 4;
std::atomic<int> selected{-1};
std::atomic<unsigned> observed{0};
std::atomic<unsigned> handler_calls{0};
std::atomic<bool> invalid_payload{false};
std::array<fw::tests::allocation_measurement_t, channel_names.size ()> costs{};
std::array<fw::tests::allocation_measurement_t, channel_names.size ()> complete_costs{};
std::array<bool, channel_names.size ()> dispatch_success{};
constexpr const char *pending_channel = "cost-pending";
std::shared_ptr<fw::task_completion_source_t<void>> pending_release;
std::atomic<bool> pending_entered{false};

struct owner_work_observation_t
{
    std::atomic<unsigned> completed{0};
    fw::tests::allocation_measurement_t dispatch_to_idle{}, suffix{};
    unsigned handlers = 0, claims = 0, ready_claims = 0, empty_claims = 0;
};
struct owner_work_scope_t
{
    unsigned handlers = 0, claims = 0, ready_claims = 0, empty_claims = 0;
    std::size_t last_ready_count = 0, last_ready_bytes = 0;
    bool previous_tracking = false;
    std::size_t previous_count = 0, previous_bytes = 0, previous_largest = 0, previous_size = 0;
    std::shared_ptr<owner_work_observation_t> observation;
};
std::atomic<std::shared_ptr<owner_work_observation_t>> selected_owner_work;
thread_local owner_work_scope_t owner_work_scope;
thread_local owner_work_scope_t *current_owner_work = nullptr;

void finish_owner_measurement () noexcept
{
    auto *scope = std::exchange (current_owner_work, nullptr);
    if (!scope)
        return;
    const fw::tests::allocation_measurement_t total{fw::tests::tracked_allocation_count,
                                                    fw::tests::tracked_allocation_bytes,
                                                    fw::tests::largest_allocation};
    const fw::tests::allocation_measurement_t suffix{total.count - scope->last_ready_count,
                                                     total.bytes - scope->last_ready_bytes, 0};
    fw::tests::tracking_allocations = scope->previous_tracking;
    fw::tests::tracked_allocation_count = scope->previous_count;
    fw::tests::tracked_allocation_bytes = scope->previous_bytes;
    fw::tests::largest_allocation = scope->previous_largest;
    fw::tests::tracked_allocation_size = scope->previous_size;
    auto observation = std::move (scope->observation);
    observation->dispatch_to_idle = total;
    observation->suffix = suffix;
    observation->handlers = scope->handlers;
    observation->claims = scope->claims;
    observation->ready_claims = scope->ready_claims;
    observation->empty_claims = scope->empty_claims;
    observation->completed.fetch_add (1, std::memory_order_release);
}

struct owner_cost_send_t
{
    void handle (const std::string &value)
    {
        if (value != "owner-cost" || !current_owner_work)
            throw std::runtime_error ("public local send bypassed the observed owner work");
        ++current_owner_work->handlers;
    }
};

void check_payload (const std::string &payload)
{
    ++handler_calls;
    if (payload == "fail")
        throw std::runtime_error ("mesh handler failure");
    if (payload == "framework")
        throw fw::framework_exception_t (fw::framework_error_kind_t::protocol_error,
                                         "mesh framework failure");
    if (payload == "nonstandard")
        throw 17;
    if (payload != "payload")
        invalid_payload = true;
}

template <bool Context> struct sync_send_t
{
    void handle (const std::string &value)
        requires (!Context)
    {
        check_payload (value);
    }
    void handle (const std::string &value, const fw::route_message_context_t &)
        requires Context
    {
        check_payload (value);
    }
};
template <bool Context> struct ready_send_t
{
    fw::task_t<void> handle (const std::string &value)
        requires (!Context)
    {
        check_payload (value);
        auto ready = fw::task_t<void> (fw::result_t<void>::success ());
        return ready;
    }
    fw::task_t<void> handle (const std::string &value, const fw::route_message_context_t &)
        requires Context
    {
        check_payload (value);
        auto ready = fw::task_t<void> (fw::result_t<void>::success ());
        return ready;
    }
};
template <bool Context> struct sync_request_t
{
    std::string handle (const std::string &value)
        requires (!Context)
    {
        check_payload (value);
        return value;
    }
    std::string handle (const std::string &value, const fw::route_message_context_t &)
        requires Context
    {
        check_payload (value);
        return value;
    }
};
template <bool Context> struct ready_request_t
{
    fw::task_t<std::string> handle (const std::string &value)
        requires (!Context)
    {
        check_payload (value);
        auto ready = fw::task_t<std::string> (fw::result_t<std::string>::success (value));
        return ready;
    }
    fw::task_t<std::string> handle (const std::string &value, const fw::route_message_context_t &)
        requires Context
    {
        check_payload (value);
        auto ready = fw::task_t<std::string> (fw::result_t<std::string>::success (value));
        return ready;
    }
};

struct pending_request_t
{
    fw::task_t<std::string> handle (const std::string &value,
                                    const fw::route_message_context_t &context)
    {
        const auto original = context;
        pending_entered.store (true, std::memory_order_release);
        co_await pending_release->task ();
        if (value != "pending payload survives suspension"
            || context.channel_name != original.channel_name
            || context.mesh_name != original.mesh_name
            || context.packet_name != original.packet_name
            || context.content_type != original.content_type
            || context.source_node_rid != original.source_node_rid)
            throw std::runtime_error ("pending handler lost payload or context");
        co_return value;
    }
};

class running_app_t
{
  public:
    explicit running_app_t (fw::app_t &app) :
        _app (app), _run (std::async (std::launch::async, [&app] { return app.run (0, nullptr); }))
    {
    }
    ~running_app_t ()
    {
        _app.request_stop ();
        EXPECT_EQ (_run.get (), 0);
    }
    bool ready ()
    {
        const auto deadline = std::chrono::steady_clock::now () + startup_timeout;
        while (!_app.is_ready () && std::chrono::steady_clock::now () < deadline) {
            if (_run.wait_for (0ms) == std::future_status::ready)
                return false;
            std::this_thread::yield ();
        }
        return _app.is_ready ();
    }

  private:
    fw::app_t &_app;
    std::future<int> _run;
};

template <typename T> std::size_t ready_handler_cost (fw::result_t<T> value)
{
    std::optional<fw::task_t<T>> task;
    const auto cost = fw::tests::count_allocations ([&] { task.emplace (std::move (value)); });
    EXPECT_TRUE (task->await_ready ());
    EXPECT_TRUE (task->result ());
    return cost;
}
} // namespace

// The first condition wait after synchronous dispatch is the worker's idle
// boundary. The last ready claim may be nonempty when the owner budget expires.
// Preserve every wait argument and result; only read counters.
extern "C" int __real_pthread_cond_wait (pthread_cond_t *, pthread_mutex_t *);
extern "C" int __wrap_pthread_cond_wait (pthread_cond_t *condition, pthread_mutex_t *mutex)
{
    finish_owner_measurement ();
    return __real_pthread_cond_wait (condition, mutex);
}
extern "C" int
__real_pthread_cond_timedwait (pthread_cond_t *, pthread_mutex_t *, const timespec *);
extern "C" int __wrap_pthread_cond_timedwait (pthread_cond_t *condition,
                                              pthread_mutex_t *mutex,
                                              const timespec *deadline)
{
    finish_owner_measurement ();
    return __real_pthread_cond_timedwait (condition, mutex, deadline);
}
#if defined(_GLIBCXX_USE_PTHREAD_COND_CLOCKWAIT)
extern "C" int
__real_pthread_cond_clockwait (pthread_cond_t *, pthread_mutex_t *, clockid_t, const timespec *);
extern "C" int __wrap_pthread_cond_clockwait (pthread_cond_t *condition,
                                              pthread_mutex_t *mutex,
                                              clockid_t clock,
                                              const timespec *deadline)
{
    finish_owner_measurement ();
    return __real_pthread_cond_clockwait (condition, mutex, clock, deadline);
}
#endif

using owner_dispatch_t = std::function<void (const fw::runtime::host::ready_record_t &,
                                             const fw::runtime::host::receive_record_t &,
                                             std::vector<zlink::message_t>)>;
#define OWNER_CLAIM_SYMBOL                                                                         \
    "_ZN5zlink9framework7runtime4host21public_host_runtime_t26dispatch_application_ownerENSt7__"   \
    "cxx1112basic_stringIcSt11char_traitsIcESaIcEEESt8functionIFvRKNS2_14ready_record_tERKNS2_"    \
    "16receive_record_tESt6vectorINS_9message_tESaISI_EEEESA_IFvvEESO_"
extern "C" fw::task_t<bool>
real_owner_claim (fw::runtime::host::public_host_runtime_t *,
                  std::string,
                  owner_dispatch_t,
                  std::function<void ()>,
                  std::function<void ()>) __asm__ ("__real_" OWNER_CLAIM_SYMBOL);
extern "C" fw::task_t<bool>
wrap_owner_claim (fw::runtime::host::public_host_runtime_t *,
                  std::string,
                  owner_dispatch_t,
                  std::function<void ()>,
                  std::function<void ()>) __asm__ ("__wrap_" OWNER_CLAIM_SYMBOL);
#undef OWNER_CLAIM_SYMBOL
extern "C" fw::task_t<bool> wrap_owner_claim (fw::runtime::host::public_host_runtime_t *host,
                                              std::string owner,
                                              owner_dispatch_t dispatch,
                                              std::function<void ()> started,
                                              std::function<void ()> rejected)
{
    if (!current_owner_work) {
        if (auto observation = selected_owner_work.load (std::memory_order_acquire)) {
            owner_work_scope = {};
            auto &scope = owner_work_scope;
            scope.observation = std::move (observation);
            scope.previous_tracking = std::exchange (fw::tests::tracking_allocations, true);
            scope.previous_count = std::exchange (fw::tests::tracked_allocation_count, 0);
            scope.previous_bytes = std::exchange (fw::tests::tracked_allocation_bytes, 0);
            scope.previous_largest = std::exchange (fw::tests::largest_allocation, 0);
            scope.previous_size = std::exchange (fw::tests::tracked_allocation_size, 0);
            current_owner_work = &scope;
        }
    }
    auto result = real_owner_claim (host, std::move (owner), std::move (dispatch),
                                    std::move (started), std::move (rejected));
    if (current_owner_work) {
        ++current_owner_work->claims;
        if (result.await_ready () && result.result ()) {
            ++current_owner_work->ready_claims;
            if (!result.result ().value ())
                ++current_owner_work->empty_claims;
            current_owner_work->last_ready_count = fw::tests::tracked_allocation_count;
            current_owner_work->last_ready_bytes = fw::tests::tracked_allocation_bytes;
        }
    }
    return result;
}

// Count the complete typed adapter call, including its returned task state.
// The surrounding hosted observer owns tracking; this scope only reads deltas.
extern "C" fw::task_t<zlink::message_t> real_registry_invoke (
  const fw::detail::route_handler_registry_t *,
  std::string_view,
  fw::runtime::messaging::message_kind_t,
  std::string_view,
  fw::service_provider_t &,
  fw::serializer_registry_t &,
  const zlink::message_t &,
  const fw::route_message_context_t
    &) __asm__ ("__real__ZNK5zlink9framework6detail24route_handler_registry_t12invoke_"
                "asyncESt17basic_string_viewIcSt11char_traitsIcEENS0_7runtime9messaging14message_"
                "kind_tES6_RNS0_18service_provider_tERNS0_21serializer_registry_tERKNS_9message_"
                "tERKNS0_23route_message_context_tE");
extern "C" fw::task_t<zlink::message_t> wrap_registry_invoke (
  const fw::detail::route_handler_registry_t *,
  std::string_view,
  fw::runtime::messaging::message_kind_t,
  std::string_view,
  fw::service_provider_t &,
  fw::serializer_registry_t &,
  const zlink::message_t &,
  const fw::route_message_context_t
    &) __asm__ ("__wrap__ZNK5zlink9framework6detail24route_handler_registry_t12invoke_"
                "asyncESt17basic_string_viewIcSt11char_traitsIcEENS0_7runtime9messaging14message_"
                "kind_tES6_RNS0_18service_provider_tERNS0_21serializer_registry_tERKNS_9message_"
                "tERKNS0_23route_message_context_tE");
extern "C" fw::task_t<zlink::message_t>
wrap_registry_invoke (const fw::detail::route_handler_registry_t *self,
                      std::string_view channel,
                      fw::runtime::messaging::message_kind_t kind,
                      std::string_view packet,
                      fw::service_provider_t &services,
                      fw::serializer_registry_t &serializers,
                      const zlink::message_t &message,
                      const fw::route_message_context_t &context)
{
    const int index = selected.load (std::memory_order_acquire);
    if (index < 0 || channel != channel_names[static_cast<std::size_t> (index)]
        || !fw::tests::tracking_allocations)
        return real_registry_invoke (self, channel, kind, packet, services, serializers, message,
                                     context);
    const auto before_count = fw::tests::tracked_allocation_count;
    const auto before_bytes = fw::tests::tracked_allocation_bytes;
    auto result =
      real_registry_invoke (self, channel, kind, packet, services, serializers, message, context);
    // A cumulative allocation peak has no delta; count and bytes cover this scope.
    costs[static_cast<std::size_t> (index)] = {fw::tests::tracked_allocation_count - before_count,
                                               fw::tests::tracked_allocation_bytes - before_bytes,
                                               0};
    return result;
}

// Observe the real hosted call. The test submits only through public app and
// route-client APIs; this wrapper preserves the dispatch arguments and result.
extern "C" fw::result_t<void> real_mesh_dispatch (
  const fw::detail::mesh_record_dispatcher_t *,
  const fw::runtime::host::receive_record_t &,
  std::vector<zlink::message_t>,
  fw::runtime::messaging::
    envelope_header_t) __asm__ ("__real__ZNK5zlink9framework6detail24mesh_record_dispatcher_"
                                "t8dispatchERKNS0_7runtime4host16receive_record_tESt6vectorINS_"
                                "9message_tESaIS9_EENS3_9messaging17envelope_header_tE");
extern "C" fw::result_t<void> wrap_mesh_dispatch (
  const fw::detail::mesh_record_dispatcher_t *,
  const fw::runtime::host::receive_record_t &,
  std::vector<zlink::message_t>,
  fw::runtime::messaging::
    envelope_header_t) __asm__ ("__wrap__ZNK5zlink9framework6detail24mesh_record_dispatcher_"
                                "t8dispatchERKNS0_7runtime4host16receive_record_tESt6vectorINS_"
                                "9message_tESaIS9_EENS3_9messaging17envelope_header_tE");
extern "C" fw::result_t<void> wrap_mesh_dispatch (const fw::detail::mesh_record_dispatcher_t *self,
                                                  const fw::runtime::host::receive_record_t &record,
                                                  std::vector<zlink::message_t> parts,
                                                  fw::runtime::messaging::envelope_header_t header)
{
    const int index = selected.load (std::memory_order_acquire);
    const auto &channel = record.channel_name.empty () ? header.channel_name : record.channel_name;
    if (index < 0 || channel != channel_names[static_cast<std::size_t> (index)])
        return real_mesh_dispatch (self, record, std::move (parts), std::move (header));
    std::optional<fw::result_t<void>> result;
    complete_costs[static_cast<std::size_t> (index)] = fw::tests::measure_allocations ([&] {
        result.emplace (real_mesh_dispatch (self, record, std::move (parts), std::move (header)));
    });
    dispatch_success[static_cast<std::size_t> (index)] = static_cast<bool> (*result);
    observed.fetch_add (1, std::memory_order_release);
    return std::move (*result);
}

TEST (MeshHandlerCost, PublicSyncHandlersDoNotOwnAnAdapterCoroutine)
{
    selected = -1;
    observed = 0;
    handler_calls = 0;
    invalid_payload = false;
    auto server = fw::app_t::create ();
    server.logging ().disable_record_capture ();
    server.add_zlink_framework ([] (fw::zlink_framework_options_t &options) {
        auto mesh = options.add_route_mesh ("handler-cost");
        mesh.set_object_role (fw::object_role_t::none)
          .set_routing_id (zlink::routing_id_t::from ("handler-cost-server"))
          .listen ("tcp://127.0.0.1:0");
        mesh.channel (channel_names[0])
          .server ()
          .add_send_handler<sync_send_t<false>, std::string> ();
        mesh.channel (channel_names[1])
          .server ()
          .add_send_handler<ready_send_t<false>, std::string> ();
        mesh.channel (channel_names[2])
          .server ()
          .add_send_handler<sync_send_t<true>, std::string> ();
        mesh.channel (channel_names[3])
          .server ()
          .add_send_handler<ready_send_t<true>, std::string> ();
        mesh.channel (channel_names[4])
          .server ()
          .add_request_handler<sync_request_t<false>, std::string, std::string> ();
        mesh.channel (channel_names[5])
          .server ()
          .add_request_handler<ready_request_t<false>, std::string, std::string> ();
        mesh.channel (channel_names[6])
          .server ()
          .add_request_handler<sync_request_t<true>, std::string, std::string> ();
        mesh.channel (channel_names[7])
          .server ()
          .add_request_handler<ready_request_t<true>, std::string, std::string> ();
        mesh.channel (pending_channel)
          .server ()
          .add_request_handler<pending_request_t, std::string, std::string> ();
    });
    auto server_provider = server.advanced ().services ().build_provider ();
    running_app_t running_server (server);
    ASSERT_TRUE (running_server.ready ());
    const auto endpoint = server_provider.get_required<fw::framework_runtime_t> ()
                            .listener_status (fw::listener_kind_t::route_mesh, "handler-cost")
                            .endpoint;
    ASSERT_FALSE (endpoint.empty ());
    auto client = fw::app_t::create ();
    client.logging ().disable_record_capture ();
    client.add_zlink_framework ([&] (fw::zlink_framework_options_t &options) {
        auto mesh = options.add_route_mesh ("handler-cost");
        mesh.set_object_role (fw::object_role_t::none)
          .set_routing_id (zlink::routing_id_t::from ("handler-cost-client"))
          .listen ("tcp://127.0.0.1:0");
        mesh.peer_connections ().connect (zlink::routing_id_t::from ("handler-cost-server"),
                                          endpoint);
        for (const auto *name : channel_names)
            mesh.channel (name).client ();
        mesh.channel (pending_channel).client ();
    });
    auto provider = client.advanced ().services ().build_provider ();
    running_app_t running_client (client);
    ASSERT_TRUE (running_client.ready ());
    auto &mesh = provider.get_required<fw::route_mesh_runtime_t> ();
    const auto deadline = std::chrono::steady_clock::now () + startup_timeout;
    while (mesh.snapshot ("handler-cost").ready_peer_count == 0
           && std::chrono::steady_clock::now () < deadline)
        std::this_thread::yield ();
    ASSERT_GT (mesh.snapshot ("handler-cost").ready_peer_count, 0u);
    auto &routes = provider.get_required<fw::route_client_t> ();
    const auto submit = [&] (int index, const std::string &payload) {
        const auto previous = observed.load (std::memory_order_acquire);
        selected.store (index, std::memory_order_release);
        if (index < 4) {
            const auto result =
              routes.send_to_channel (channel_names[index], payload).async ().result ();
            EXPECT_TRUE (result);
        } else {
            const auto result = routes.request_to_channel (channel_names[index], payload)
                                  .timeout (request_timeout)
                                  .async<std::string> ()
                                  .result ();
            if (payload == "fail" || payload == "nonstandard")
                EXPECT_EQ (result.error_kind (), fw::framework_error_kind_t::internal_failure);
            else if (payload == "framework")
                EXPECT_EQ (result.error_kind (), fw::framework_error_kind_t::protocol_error);
            else {
                EXPECT_TRUE (result);
                if (result)
                    EXPECT_EQ (result.value (), payload);
            }
        }
        const auto completed_by = std::chrono::steady_clock::now () + startup_timeout;
        while (observed.load (std::memory_order_acquire) == previous
               && std::chrono::steady_clock::now () < completed_by)
            std::this_thread::yield ();
        EXPECT_EQ (observed.load (std::memory_order_acquire), previous + 1);
        EXPECT_EQ (dispatch_success[index], index >= 4 || payload == "payload");
        selected.store (-1, std::memory_order_release);
    };
    for (int index = 0; index != static_cast<int> (channel_names.size ()); ++index)
        submit (index, "payload");
    const auto send_ready = ready_handler_cost (fw::result_t<void>::success ());
    const auto request_ready = ready_handler_cost (fw::result_t<std::string>::success ("payload"));
    EXPECT_GT (send_ready, 0u);
    EXPECT_GT (request_ready, 0u);
    for (int round = 0; round != sample_rounds; ++round) {
        for (int pair = 0; pair != 4; ++pair) {
            const int sync = pair * 2, async = sync + 1;
            submit (round % 2 ? async : sync, "payload");
            submit (round % 2 ? sync : async, "payload");
            const auto ready = pair < 2 ? send_ready : request_ready;
            std::printf ("mesh-handler round=%d pair=%d sync=%zu/%zu async=%zu/%zu ready=%zu "
                         "complete-sync=%zu complete-async=%zu\n",
                         round, pair, costs[sync].count, costs[sync].bytes, costs[async].count,
                         costs[async].bytes, ready, complete_costs[sync].count,
                         complete_costs[async].count);
            EXPECT_LT (costs[sync].count + ready, costs[async].count)
              << "pair=" << pair << " round=" << round;
        }
    }
    for (const std::string payload : {"fail", "framework", "nonstandard"})
        for (int index = 0; index != static_cast<int> (channel_names.size ()); ++index)
            submit (index, payload);
    EXPECT_EQ (handler_calls.load (), channel_names.size () * (sample_rounds + 4));
    EXPECT_FALSE (invalid_payload.load ());

    pending_release = std::make_shared<fw::task_completion_source_t<void>> ();
    pending_entered = false;
    struct release_guard_t
    {
        ~release_guard_t () { pending_release->complete (fw::result_t<void>::success ()); }
    } release_guard;
    auto pending =
      routes
        .request_to_channel (pending_channel, std::string ("pending payload survives suspension"))
        .timeout (request_timeout)
        .async<std::string> ();
    const auto entered_by = std::chrono::steady_clock::now () + startup_timeout;
    while (!pending_entered.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < entered_by)
        std::this_thread::yield ();
    ASSERT_TRUE (pending_entered.load (std::memory_order_acquire));
    EXPECT_FALSE (pending.await_ready ());
    EXPECT_TRUE (pending_release->complete (fw::result_t<void>::success ()));
    const auto reply = pending.result ();
    ASSERT_TRUE (reply) << (reply.error () ? reply.error ()->what () : "no error");
    EXPECT_EQ (reply.value (), "pending payload survives suspension");
}

TEST (MeshHandlerCost, PublicOwnerWorkDoesNotAllocateAnotherTaskOwner)
{
    auto app = fw::app_t::create ();
    app.logging ().disable_record_capture ();
    app.add_zlink_framework ([] (fw::zlink_framework_options_t &options) {
        options.add_route_mesh ("owner-cost")
          .set_object_role (fw::object_role_t::none)
          .set_routing_id (zlink::routing_id_t::from ("owner-cost-node"))
          .listen ("tcp://127.0.0.1:0")
          .add_route_send_handler<owner_cost_send_t, std::string> ();
    });
    auto provider = app.advanced ().services ().build_provider ();
    running_app_t running (app);
    ASSERT_TRUE (running.ready ());
    auto &routes = provider.get_required<fw::route_client_t> ();
    const auto anchor = std::make_shared<int> (0);
    std::function<void (const fw::result_t<void> &)> callback;
    const auto callback_cost = fw::tests::count_allocations ([&] {
        callback = [anchor] (const fw::result_t<void> &result) {
            result.value ();
            ++*anchor;
        };
    });
    callback (fw::result_t<void>::success ());
    EXPECT_EQ (*anchor, 1);
    for (int round = 0; round != sample_rounds; ++round) {
        auto observation = std::make_shared<owner_work_observation_t> ();
        selected_owner_work.store (observation, std::memory_order_release);
        const auto submitted =
          routes
            .send_to_node ("owner-cost", zlink::routing_id_t::from ("owner-cost-node"),
                           std::string ("owner-cost"))
            .async ()
            .result ();
        EXPECT_TRUE (submitted);
        const auto deadline = std::chrono::steady_clock::now () + startup_timeout;
        while (observation->completed.load (std::memory_order_acquire) == 0
               && std::chrono::steady_clock::now () < deadline)
            std::this_thread::yield ();
        selected_owner_work.store (nullptr, std::memory_order_release);
        ASSERT_EQ (observation->completed.load (std::memory_order_acquire), 1u);
        EXPECT_EQ (observation->handlers, 1u);
        EXPECT_GE (observation->claims, 1u);
        EXPECT_LE (observation->claims, 2u);
        EXPECT_EQ (observation->ready_claims, observation->claims);
        EXPECT_EQ (observation->empty_claims, observation->claims - 1);
        EXPECT_GT (observation->dispatch_to_idle.count, observation->suffix.count);
        EXPECT_GT (observation->dispatch_to_idle.bytes, observation->suffix.bytes);
        std::printf ("owner-work round=%d dispatch-to-idle=%zu/%zu suffix=%zu/%zu callback=%zu "
                     "handlers=%u claims=%u empty=%u\n",
                     round, observation->dispatch_to_idle.count,
                     observation->dispatch_to_idle.bytes, observation->suffix.count,
                     observation->suffix.bytes, callback_cost, observation->handlers,
                     observation->claims, observation->empty_claims);
        EXPECT_LE (observation->suffix.count, callback_cost) << "round=" << round;
    }
}
