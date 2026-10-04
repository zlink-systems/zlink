/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#include <zlink.h>
#include "runtime/backend/raw_route_port.hpp"
#include "runtime/backend/raw_dealer_port.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/fanout/raw_fanout_owner.hpp"
#include "runtime/timers/core_timer_drain_loop.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/host/hosted_service_lifecycle.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/streams/stream_host_service.hpp"
#include <zlink.hpp>
#include <zlink/framework.hpp>
#include <cassert>
#include <string_view>

namespace
{
std::atomic_bool fail_wait{false};
thread_local bool stream_worker = false;
bool fail_poller = false;
bool fail_socket = false;
int socket_attempts = 0;
int poller_attempts = 0;
}

extern "C" void *__real_zlink_socket (void *, zlink_socket_type_t);
extern "C" void *__wrap_zlink_socket (void *context, zlink_socket_type_t type)
{
    auto *socket = __real_zlink_socket (context, type);
    if (type == ZLINK_SOCKET_STREAM)
        stream_worker = true;
    return socket;
}
extern "C" zlink_close_result_t __real_zlink_poller_destroy (void **);
extern "C" zlink_close_result_t __wrap_zlink_poller_destroy (void **handle)
{
    ++poller_attempts;
    if (std::exchange (fail_poller, false))
        return ZLINK_CLOSE_BUSY;
    return __real_zlink_poller_destroy (handle);
}
extern "C" zlink_close_result_t __real_zlink_close (void *);
extern "C" zlink_close_result_t __wrap_zlink_close (void *handle)
{
    ++socket_attempts;
    if (std::exchange (fail_socket, false))
        return ZLINK_CLOSE_BUSY;
    return __real_zlink_close (handle);
}

extern "C" int
__real_zlink_poller_wait (void *, zlink_poller_event_t *, int, long, zlink_config_result_t *);
extern "C" int __wrap_zlink_poller_wait (void *poller,
                                         zlink_poller_event_t *events,
                                         int capacity,
                                         long timeout,
                                         zlink_config_result_t *error)
{
    if (stream_worker && fail_wait.exchange (false)) {
        fail_wait.notify_all ();
        if (error)
            *error = ZLINK_CONFIG_NOT_FOUND;
        errno = EIO;
        return -1;
    }
    return __real_zlink_poller_wait (poller, events, capacity, timeout, error);
}
template <typename Owner> void verify_poller_close (Owner &owner)
{
    fail_poller = true;
    const auto before = poller_attempts;
    bool failed = false;
    try {
        owner.close ();
    }
    catch (const zlink::close_error_t &) {
        failed = true;
    }
    assert (failed);
    owner.close ();
    assert (poller_attempts == before + 2);
}

class failing_stop_service_t final : public zlink::framework::hosted_service_t,
                                     public zlink::framework::runtime::hosted_service_lifecycle_t
{
  public:
    explicit failing_stop_service_t (zlink::framework::app_t &app, bool timer) :
        _app (app), _timer (timer)
    {
    }
    zlink::framework::task_t<void> start (zlink::framework::service_provider_t &) override
    {
        if (!_timer)
            fail_socket = true;
        _app.stop ();
        return zlink::framework::task_t<void> (zlink::framework::result_t<void>::success ());
    }
    void stop () noexcept override
    {
        if (_timer) {
            fail_poller = true;
            zlink::framework::detail::core_timer_drain_loop_t timer (_runtime_failures);
        }
    }

  private:
    zlink::framework::app_t &_app;
    bool _timer;
};

int main (int argc, char **argv)
{
    assert (argc == 2);
    const std::string_view owner = argv[1];
    if (owner == "channel-publisher" || owner == "channel-poller") {
        zlink::framework::zlink_builder_t builder;
        builder.channel ("close-failure").enable_publisher (false).bind ("tcp://127.0.0.1:0");
        auto runtime = zlink::framework::detail::channel_runtime_t::from (builder.message_bus ());
        runtime.bind_core_context (std::make_shared<zlink::context_t> ());
        runtime.initialize_manual_channel_publishers ();
        if (owner == "channel-poller") {
            verify_poller_close (runtime);
        } else {
            fail_socket = true;
            const auto before = socket_attempts;
            bool failed = false;
            try {
                runtime.close ();
            }
            catch (const zlink::close_error_t &) {
                failed = true;
            }
            assert (failed);
            runtime.close ();
            assert (socket_attempts == before + 2);
        }
        return 0;
    }
    if (owner == "host-stop" || owner == "host-timer") {
        auto app = zlink::framework::app_t::create ();
        app.add_zlink_framework ();
        if (owner == "host-stop")
            app.advanced ()
              .zlink ()
              .channel ("close-failure")
              .enable_publisher (false)
              .bind ("tcp://127.0.0.1:0");
        app.add_hosted_service (
          std::make_unique<failing_stop_service_t> (app, owner == "host-timer"));
        assert (app.run (0, nullptr) != 0);
        const auto &records = app.logging ().captured_records ();
        assert (std::any_of (records.begin (), records.end (), [] (const auto &record) {
            return record.level == zlink::framework::log_level_t::critical;
        }));
        return 0;
    }
    if (owner == "stream-worker" || owner == "stream-worker-no-monitoring") {
        zlink::framework::service_collection_t services;
        zlink::framework::handler_registry_t handlers;
        zlink::framework::serializer_registry_t serializers;
        zlink::framework::zlink_builder_t builder;
        zlink::framework::zlink_framework_options_t options (services, handlers, serializers,
                                                             builder);
        options.add_route_mesh ("worker")
          .listen ("tcp://127.0.0.1:0")
          .set_routing_id (zlink::routing_id_t::from ("worker"));
        options.add_stream_node ("close-failure")
          .bind ("tcp://127.0.0.1:0")
          .register_session ("close-session");
        options.apply ();
        for (const auto &registration :
             zlink::framework::detail::mesh_node_runtime_t::registrations (builder))
            registration->core_context = std::make_shared<zlink::context_t> ();
        auto mesh = zlink::framework::detail::mesh_node_runtime_t::from (builder, "worker");
        mesh->bind_serializers (serializers);
        mesh->start ();
        auto runtime = zlink::framework::detail::stream_runtime_t::from (builder);
        auto monitoring = std::make_shared<zlink::framework::detail::monitoring_runtime_state_t> ();
        zlink::framework::runtime::stream_host_service_t host (
          runtime, runtime.snapshots (),
          {{"close-session",
            [] (zlink::framework::service_provider_t &)
              -> zlink::framework::packet_stream_session_t & {
                throw std::logic_error (
                  "session creation is unexpected before the injected worker failure");
            }}},
          std::chrono::seconds{30}, mesh);
        if (owner == "stream-worker")
            host.bind_monitoring (monitoring);
        host.bind_runtime_failures (monitoring->runtime_failures);
        auto provider = services.build_provider ();
        fail_wait = true;
        host.start (provider).result ().value ();
        fail_wait.wait (true);
        host.stop ();
        mesh->stop ();
        assert (!fail_wait.load ());
        bool failed = false;
        try {
            monitoring->runtime_failures->rethrow_if_failed ();
        }
        catch (const zlink::config_error_t &) {
            failed = true;
        }
        assert (failed);
        return 0;
    }
    if (owner == "timer-destructor") {
        auto failures = std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ();
        const auto before = poller_attempts;
        {
            zlink::framework::detail::core_timer_drain_loop_t timer (failures);
            fail_poller = true;
        }
        bool failed = false;
        try {
            failures->rethrow_if_failed ();
        }
        catch (const zlink::close_error_t &) {
            failed = true;
        }
        assert (failed);
        assert (poller_attempts == before + 1);
        failures->close_retained ();
        assert (poller_attempts == before + 2);
        return 0;
    }
    if (owner == "destructor-publisher" || owner == "destructor-mesh") {
        auto failures = std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ();
        const auto before = socket_attempts;
        if (owner == "destructor-publisher") {
            zlink::framework::runtime::fanout::raw_fanout_publisher_t publisher (
              "tcp://127.0.0.1:0", {}, false, std::nullopt, failures);
            publisher.start ();
            fail_socket = true;
        } else {
            zlink::framework::runtime::mesh::raw_mesh_node_options_t options;
            options.descriptor = {"close-failure",     {'c'},           1, 1,
                                  "tcp://127.0.0.1:0", {{"alpha", 100}}};
            options.runtime_failures = failures;
            zlink::framework::runtime::mesh::raw_mesh_node_owner_t node (options);
            node.start ();
            fail_socket = true;
        }
        assert (socket_attempts == before + 1);
        bool failed = false;
        try {
            failures->rethrow_if_failed ();
        }
        catch (const zlink::close_error_t &) {
            failed = true;
        }
        assert (failed);
        failures->close_retained ();
        assert (socket_attempts == before + 2);
        failures->close_retained ();
        assert (socket_attempts == before + 2);
        return 0;
    }
    zlink::context_t context;
    zlink::router_socket_t router (context);
    zlink::dealer_socket_t dealer (context);
    if (owner == "route") {
        zlink::framework::detail::backend::raw_route_port_t route (router);
        verify_poller_close (route);
        return 0;
    }
    if (owner == "dealer") {
        zlink::framework::detail::backend::raw_dealer_port_t client (dealer);
        verify_poller_close (client);
        return 0;
    }
    if (owner == "timer") {
        zlink::framework::detail::core_timer_drain_loop_t timer;
        verify_poller_close (timer);
        return 0;
    }
    if (owner == "fanout-poller") {
        zlink::framework::runtime::fanout::raw_fanout_subscriber_t subscriber;
        verify_poller_close (subscriber);
        return 0;
    }
    if (owner == "fanout-publisher") {
        zlink::framework::runtime::fanout::raw_fanout_publisher_t publisher ("tcp://127.0.0.1:0");
        publisher.start ();
        fail_socket = true;
        const auto before = socket_attempts;
        bool failed = false;
        try {
            publisher.close ();
        }
        catch (const zlink::close_error_t &) {
            failed = true;
        }
        assert (failed);
        publisher.close ();
        assert (socket_attempts == before + 2);
        return 0;
    }
    if (owner == "fanout-subscriber") {
        zlink::framework::runtime::fanout::raw_fanout_subscriber_t subscriber;
        assert (subscriber.connect_manual ({'p'}, "tcp://127.0.0.1:1"));
        fail_socket = true;
        const auto before = socket_attempts;
        bool failed = false;
        try {
            subscriber.close ();
        }
        catch (const zlink::close_error_t &) {
            failed = true;
        }
        assert (failed);
        subscriber.close ();
        assert (socket_attempts == before + 2);
        return 0;
    }

    zlink::framework::runtime::mesh::raw_mesh_node_options_t options;
    options.descriptor = {"close-failure", {'c'}, 1, 1, "tcp://127.0.0.1:0", {{"alpha", 100}}};
    zlink::framework::runtime::mesh::raw_mesh_node_owner_t node (options);
    node.start ();
    fail_socket = true;
    const auto before = socket_attempts;
    bool failed = false;
    try {
        node.close ();
    }
    catch (const zlink::close_error_t &) {
        failed = true;
    }
    assert (failed);
    node.close ();
    assert (socket_attempts == before + 2);
}
