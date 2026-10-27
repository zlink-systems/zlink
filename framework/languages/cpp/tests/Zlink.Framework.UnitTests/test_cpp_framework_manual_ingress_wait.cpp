/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "../support/runtime_failure_fixture.hpp"
#include "test_completion_poller_driver.hpp"
#include "runtime/channels/channel_host_service.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/dispatch/application_job_queue.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include <zlink/framework.hpp>
#include <zlink/Contracts/Core/context.hpp>
#include <zlink/Contracts/Messaging/operation_contracts.hpp>
#include <zlink/Contracts/Sockets/routed_socket_contracts.hpp>
#include <zlink.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <zlink/Contracts/Eventing/monitor.hpp>
#include <zlink/Contracts/Eventing/events.hpp>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <iostream>
#include <sstream>
#include <vector>

namespace
{
namespace fw = zlink::framework;
using namespace std::chrono_literals;
std::mutex observed_mutex;
std::condition_variable observed_changed;
void *source_poller = nullptr;
void *source_monitor = nullptr;
void *source_router = nullptr;
struct wait_observation_t
{
    long timeout;
    std::chrono::steady_clock::time_point started;
    std::optional<std::chrono::steady_clock::time_point> finished;
    int result = 0;
    std::vector<zlink_poller_event_t> events;
};
std::vector<wait_observation_t> observations;
std::vector<std::pair<std::chrono::steady_clock::time_point, std::uint64_t>> monitor_events;

bool is_send_progress (const zlink_poller_event_t &event, void *router)
{
    const auto progress = ZLINK_POLLOUT | ZLINK_POLLCOMPLETION;
    return event.source_kind == ZLINK_POLLER_SOURCE_SOCKET && event.socket == router
           && (event.events & progress) != 0 && (event.events & ~progress) == 0;
}

std::string describe_wait (const wait_observation_t &wait)
{
    std::ostringstream text;
    text << "timeout=" << wait.timeout << " result=" << wait.result;
    if (wait.finished)
        text << " elapsed_us="
             << std::chrono::duration_cast<std::chrono::microseconds> (*wait.finished
                                                                       - wait.started)
                  .count ();
    else
        text << " pending";
    for (const auto &event : wait.events)
        text << " [kind=" << static_cast<int> (event.source_kind) << " socket=" << event.socket
             << " monitor=" << (event.socket == source_monitor)
             << " router=" << (event.socket == source_router) << " flags=" << event.events << ']';
    return text.str ();
}
bool server_ready = false;
std::size_t server_ready_wait_count = 0;
std::vector<long> waits;
std::vector<std::uint32_t> weights;
std::size_t weight_wait_count = 0;

bool observe_until (const std::function<bool ()> &ready)
{
    std::unique_lock lock (observed_mutex);
    return observed_changed.wait_for (lock, 2s, ready);
}

struct manual_source_t
{
    fw::zlink_builder_t builder = fw::test::runtime_failure_builder ();
    std::shared_ptr<zlink::context_t> context = std::make_shared<zlink::context_t> ();
    fw::serializer_registry_t serializers;
    fw::handler_registry_t handlers;
    fw::service_collection_t services;
    fw::service_provider_t provider = services.build_provider ();
    std::unique_ptr<fw::runtime::channel_host_service_t> host;
    std::string endpoint;
    explicit manual_source_t (std::shared_ptr<fw::runtime::application_job_queue_t> jobs = {})
    {
        static std::atomic_uint64_t sequence{0};
        endpoint = "inproc://manual-ingress-wait-" + std::to_string (++sequence);
        {
            std::lock_guard lock (observed_mutex);
            source_poller = nullptr;
            source_monitor = nullptr;
            source_router = nullptr;
            observations.clear ();
            monitor_events.clear ();
            server_ready = false;
            waits.clear ();
            weights.clear ();
        }
        builder.channel ("manual-wait")
          .enable_server ()
          .set_routing_id (zlink::routing_id_t::from ("manual-wait-server"))
          .bind (endpoint);
        auto runtime = fw::detail::channel_runtime_t::from (builder.message_bus ());
        runtime.bind_core_context (context);
        host = std::make_unique<fw::runtime::channel_host_service_t> (
          std::make_shared<fw::runtime::runtime_failure_collector_t> (), builder.message_bus (),
          runtime.channel_snapshots (), handlers, serializers, std::map<std::string, std::string>{},
          std::move (jobs));
        host->start (provider);
    }
    ~manual_source_t () { host->stop (); }
};

fw::task_t<std::vector<zlink::message_t>>
await_reply (zlink::async_result_t<std::vector<zlink::message_t>> reply)
{
    co_return co_await std::move (reply);
}
}

extern "C" zlink_config_result_t __real_zlink_poller_add (void *, void *, void *, short);
extern "C" zlink_config_result_t
__wrap_zlink_poller_add (void *poller, void *socket, void *user, short events)
{
    const auto result = __real_zlink_poller_add (poller, socket, user, events);
    if (result == ZLINK_CONFIG_OK && (events & ZLINK_POLLIN)) {
        std::lock_guard lock (observed_mutex);
        if (!source_poller) {
            source_poller = poller;
            source_monitor = socket;
        }
    }
    if (result == ZLINK_CONFIG_OK && (events & ZLINK_POLLCOMPLETION)) {
        std::lock_guard lock (observed_mutex);
        if (poller == source_poller)
            source_router = socket;
    }
    return result;
}
extern "C" zlink_recv_result_t
__real_zlink_socket_monitor_recv (void *, zlink_socket_monitor_event_t *, zlink_recv_flags_t);
extern "C" zlink_recv_result_t __wrap_zlink_socket_monitor_recv (
  void *monitor, zlink_socket_monitor_event_t *event, zlink_recv_flags_t flags)
{
    const auto result = __real_zlink_socket_monitor_recv (monitor, event, flags);
    if (result == ZLINK_RECV_OK) {
        std::lock_guard lock (observed_mutex);
        if (monitor == source_monitor) {
            monitor_events.emplace_back (std::chrono::steady_clock::now (), event->event);
            if (event->event
                == static_cast<std::uint64_t> (zlink::monitor_event::connection_ready)) {
                server_ready = true;
                server_ready_wait_count = waits.size ();
                observed_changed.notify_all ();
            }
        }
    }
    return result;
}
extern "C" int
__real_zlink_poller_wait (void *, zlink_poller_event_t *, int, long, zlink_config_result_t *);
extern "C" int __wrap_zlink_poller_wait (void *poller,
                                         zlink_poller_event_t *events,
                                         int capacity,
                                         long timeout,
                                         zlink_config_result_t *error)
{
    std::optional<std::size_t> observation;
    {
        std::lock_guard lock (observed_mutex);
        if (poller == source_poller && timeout != 0) {
            waits.push_back (timeout);
            observation = observations.size ();
            observations.push_back ({timeout, std::chrono::steady_clock::now (), {}, 0, {}});
            observed_changed.notify_all ();
        }
    }
    const auto result = __real_zlink_poller_wait (poller, events, capacity, timeout, error);
    if (observation) {
        const auto finished = std::chrono::steady_clock::now ();
        std::lock_guard lock (observed_mutex);
        auto &wait = observations[*observation];
        wait.finished = finished;
        wait.result = result;
        if (result > 0)
            wait.events.assign (events, events + std::min (result, capacity));
        observed_changed.notify_all ();
    }
    return result;
}
extern "C" zlink_config_result_t
__real_zlink_set_router_option (void *, zlink_router_option_t, const void *, size_t);
extern "C" zlink_config_result_t __wrap_zlink_set_router_option (void *socket,
                                                                 zlink_router_option_t option,
                                                                 const void *value,
                                                                 size_t size)
{
    const auto result = __real_zlink_set_router_option (socket, option, value, size);
    if (result == ZLINK_CONFIG_OK && option == ZLINK_ROUTER_OPT_WEIGHT
        && size == sizeof (std::uint32_t)) {
        std::uint32_t weight;
        std::memcpy (&weight, value, sizeof (weight));
        std::lock_guard lock (observed_mutex);
        weights.push_back (weight);
        weight_wait_count = waits.size ();
        observed_changed.notify_all ();
    }
    return result;
}

TEST (ManualIngressWait, IdleSourceWaitsForNotificationAndAppliesWeightWithoutInput)
{
    manual_source_t source;
    ASSERT_TRUE (observe_until ([] { return waits.size () >= 2; }));
    auto runtime = fw::detail::channel_runtime_t::from (source.builder.message_bus ());
    runtime.set_server_weight ("manual-wait", 37);
    ASSERT_TRUE (observe_until (
      [] { return std::find (weights.begin (), weights.end (), 37u) != weights.end (); }))
      << "a weight change did not wake the source without socket input";
    ASSERT_TRUE (observe_until ([] { return waits.size () > weight_wait_count; }));
    {
        std::unique_lock lock (observed_mutex);
        EXPECT_EQ (-1, waits.back ()) << "idle ingress still has a periodic poll deadline";
        const auto before = waits.size ();
        EXPECT_FALSE (
          observed_changed.wait_for (lock, 200ms, [&] { return waits.size () > before; }))
          << "idle source woke without a deadline, input or notification";
    }
    const auto stopping = std::chrono::steady_clock::now ();
    source.host->stop ();
    EXPECT_LT (std::chrono::steady_clock::now () - stopping, 2s);
}

TEST (ManualIngressWait, AdmittedPeerArmsExistingLivenessDeadline)
{
    manual_source_t source;
    ASSERT_TRUE (observe_until ([] { return waits.size () >= 2; }));
    zlink::router_socket_t client (*source.context);
    client.set_routing_id (zlink::routing_id_t::from ("manual-wait-client"));
    {
        fw::test::completion_poller_driver_t completions (client);
        auto monitor = client.monitor_open (zlink::monitor_event::connection_ready);
        zlink::poller_t monitor_poller;
        monitor_poller.add (monitor, zlink::poll_event_flag_t::pollin, 1);
        client.connect (source.endpoint);
        zlink::poll_event_t connected;
        ASSERT_EQ (1u, monitor_poller.wait (&connected, 1, 2s));
        const auto event = monitor.recv (zlink::recv_flags_t::dontwait);
        ASSERT_TRUE (event);
        ASSERT_EQ (zlink::monitor_event::connection_ready, event->event);
        // Establish the idle precondition after the server has drained its
        // connection-ready monitor event, as in the descriptor-update fixture.
        ASSERT_TRUE (
          observe_until ([] { return server_ready && waits.size () > server_ready_wait_count; }));
        const auto hello = fw::runtime::protocol::encode_client_server_client_admission (
          fw::runtime::protocol::command::hello, {"manual-wait", "default", 1024 * 1024});
        auto message = zlink::message_t::from (hello);
        auto pending = client.request (zlink::routing_id_t::from ("manual-wait-server"))
                         .message (message)
                         .timeout (2s)
                         .async ()
                         .reply;
        const auto reply = await_reply (std::move (pending)).result ();
        ASSERT_TRUE (reply);
        ASSERT_FALSE (reply.value ().empty ());
        EXPECT_TRUE (observe_until ([] {
            return std::any_of (waits.begin (), waits.end (),
                                [] (long timeout) { return timeout > 50 && timeout <= 5000; });
        }))
          << "admission did not arm the existing liveness owner's next deadline";
        {
            std::unique_lock lock (observed_mutex);
            ASSERT_FALSE (observations.empty ());
            const auto first_deadline =
              std::find_if (observations.begin (), observations.end (), [] (const auto &wait) {
                  return wait.timeout > 50 && wait.timeout <= 5000;
              });
            ASSERT_NE (observations.end (), first_deadline);
            const auto first = static_cast<std::size_t> (first_deadline - observations.begin ());
            const auto until = std::chrono::steady_clock::now () + 200ms;
            // Admission immediately submits the first probe. Its send completion
            // may arrive after the deadline wait starts; the same public poller
            // must drain it. Observe one interval and classify every return.
            observed_changed.wait_until (lock, until, [] { return false; });
            for (const auto &[at, kind] : monitor_events)
                if (at >= observations[first].started)
                    std::cout << "[ admission monitor ] event=" << kind << std::endl;
            for (std::size_t index = first; index < observations.size (); ++index) {
                const auto &wait = observations[index];
                std::cout << "[ admission wait " << index - first << " ] " << describe_wait (wait)
                          << std::endl;
                SCOPED_TRACE (describe_wait (wait));
                EXPECT_GT (wait.timeout, 50) << "ingress resumed a fixed periodic poll";
                EXPECT_LE (wait.timeout, 5000);
                if (!wait.finished)
                    continue;
                if (wait.result == 0
                    && *wait.finished >= wait.started + std::chrono::milliseconds (wait.timeout))
                    continue;
                ASSERT_GT (wait.result, 0)
                  << "the source woke without an event before its existing liveness deadline";
                ASSERT_FALSE (wait.events.empty ());
                for (const auto &event : wait.events)
                    EXPECT_TRUE (is_send_progress (event, source_router))
                      << "unexpected input, monitor, notification or error during idle admission";
            }
        }
    }
    source.host->stop ();
    client.close ();
}

TEST (ManualIngressWait, OnlyAdmittedPeerReceivesDescriptorUpdates)
{
    manual_source_t source;
    ASSERT_TRUE (observe_until ([] { return waits.size () >= 2; }));
    zlink::router_socket_t client (*source.context);
    client.set_routing_id (zlink::routing_id_t::from ("manual-update-client"));
    {
        fw::test::completion_poller_driver_t completions (client);
        auto monitor = client.monitor_open (zlink::monitor_event::connection_ready);
        zlink::poller_t monitor_poller;
        monitor_poller.add (monitor, zlink::poll_event_flag_t::pollin, 1);
        client.connect (source.endpoint);
        zlink::poll_event_t connected;
        ASSERT_EQ (1u, monitor_poller.wait (&connected, 1, 2s));
        const auto event = monitor.recv (zlink::recv_flags_t::dontwait);
        ASSERT_TRUE (event);
        ASSERT_EQ (zlink::monitor_event::connection_ready, event->event);
        ASSERT_TRUE (
          observe_until ([] { return server_ready && waits.size () > server_ready_wait_count; }));
        auto runtime = fw::detail::channel_runtime_t::from (source.builder.message_bus ());
        runtime.set_server_weight ("manual-wait", 37);
        ASSERT_TRUE (observe_until ([] {
            return std::find (weights.begin (), weights.end (), 37u) != weights.end ()
                   && waits.size () > weight_wait_count;
        }));
        client.options ().recv_timeout (200ms);
        zlink::received_t received;
        EXPECT_EQ (static_cast<int> (zlink::recv_result_t::no_data), client.recv (received))
          << "plain connected peer received an admitted descriptor update";
        received.close ();

        const auto hello = fw::runtime::protocol::encode_client_server_client_admission (
          fw::runtime::protocol::command::hello, {"manual-wait", "default", 1024 * 1024});
        auto message = zlink::message_t::from (hello);
        auto pending = client.request (zlink::routing_id_t::from ("manual-wait-server"))
                         .message (message)
                         .timeout (2s)
                         .async ()
                         .reply;
        const auto admission = await_reply (std::move (pending)).result ();
        ASSERT_TRUE (admission);
        ASSERT_EQ (1u, admission.value ().size ());
        const auto descriptor = fw::runtime::protocol::decode_client_server_server_admission (
          fw::runtime::protocol::message_bytes (admission.value ().front ()),
          fw::runtime::protocol::command::admit);
        EXPECT_EQ (37u, descriptor.weight);
        runtime.set_server_weight ("manual-wait", 38);
        bool updated = false;
        const auto deadline = std::chrono::steady_clock::now () + 2s;
        while (std::chrono::steady_clock::now () < deadline) {
            const auto result = client.recv (received);
            if (result == static_cast<int> (zlink::recv_result_t::no_data))
                continue;
            ASSERT_EQ (static_cast<int> (zlink::recv_result_t::ok), result);
            ASSERT_EQ (1u, received.parts ().size ());
            const auto bytes = fw::runtime::protocol::message_bytes (received.parts ().front ());
            const auto header = fw::runtime::protocol::decode_header (bytes);
            if (header.kind == fw::runtime::protocol::command::update) {
                const auto update = fw::runtime::protocol::decode_client_server_server_admission (
                  bytes, fw::runtime::protocol::command::update);
                EXPECT_EQ (38u, update.weight);
                EXPECT_GT (update.descriptor_revision, descriptor.descriptor_revision);
                received.close ();
                updated = true;
                break;
            }
            EXPECT_EQ (fw::runtime::protocol::command::livenessProbe, header.kind);
            received.close ();
        }
        EXPECT_TRUE (updated) << "admitted peer did not receive its descriptor update";
    }
    source.host->stop ();
    client.close ();
}

TEST (ManualIngressWait, ManagementWakeDoesNotReserveApplicationSupply)
{
    fw::runtime::application_job_queue_configuration_t configuration;
    configuration.effective_max_queued_application_jobs = 1;
    auto jobs = std::make_shared<fw::runtime::application_job_queue_t> (configuration);
    auto held = jobs->try_reserve_supply ();
    ASSERT_TRUE (held);
    ASSERT_EQ (1u, jobs->snapshot ().permits_in_use);
    manual_source_t source (jobs);
    ASSERT_TRUE (observe_until ([] { return waits.size () >= 2; }));
    EXPECT_EQ (0u, jobs->snapshot ().capacity_waiters)
      << "management readiness registered an application supply waiter";
    auto runtime = fw::detail::channel_runtime_t::from (source.builder.message_bus ());
    runtime.set_server_weight ("manual-wait", 37);
    ASSERT_TRUE (observe_until ([] {
        return std::find (weights.begin (), weights.end (), 37u) != weights.end ()
               && waits.size () > weight_wait_count;
    }));
    EXPECT_EQ (0u, jobs->snapshot ().capacity_waiters)
      << "weight notification registered an application supply waiter";
    held.reset ();
    runtime.set_server_weight ("manual-wait", 38);
    ASSERT_TRUE (observe_until ([] {
        return std::find (weights.begin (), weights.end (), 38u) != weights.end ()
               && waits.size () > weight_wait_count;
    }));
    EXPECT_EQ (0u, jobs->snapshot ().permits_in_use)
      << "management-only source retained granted application capacity";
    EXPECT_EQ (0u, jobs->snapshot ().capacity_waiters);
}

TEST (ManualIngressWaitObservation, AcceptsOnlyRouterSendProgress)
{
    int router;
    int other;
    zlink_poller_event_t event{};
    event.source_kind = ZLINK_POLLER_SOURCE_SOCKET;
    event.socket = &router;
    event.events = ZLINK_POLLCOMPLETION;
    EXPECT_TRUE (is_send_progress (event, &router));
    event.events = ZLINK_POLLOUT;
    EXPECT_TRUE (is_send_progress (event, &router));
    event.events |= ZLINK_POLLCOMPLETION;
    EXPECT_TRUE (is_send_progress (event, &router));
    for (const short flags :
         std::vector<short>{0, ZLINK_POLLIN, ZLINK_POLLERR, ZLINK_POLLIN | ZLINK_POLLCOMPLETION}) {
        event.events = flags;
        EXPECT_FALSE (is_send_progress (event, &router));
    }
    event.events = ZLINK_POLLCOMPLETION;
    event.socket = &other;
    EXPECT_FALSE (is_send_progress (event, &router));
    event.socket = &router;
    event.source_kind = ZLINK_POLLER_SOURCE_TIMER;
    EXPECT_FALSE (is_send_progress (event, &router));
    event.source_kind = ZLINK_POLLER_SOURCE_FD;
    EXPECT_FALSE (is_send_progress (event, &router));
}
