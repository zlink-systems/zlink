/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#include "../support/runtime_failure_fixture.hpp"
#include <zlink.h>
#include "runtime/client_server/raw_client_server_owner.hpp"
#include "runtime/execution/state_lane.hpp"
#include "runtime/backend/raw_route_port.hpp"
#include "runtime/backend/raw_dealer_port.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/fanout/raw_fanout_owner.hpp"
#include "runtime/timers/core_timer_drain_loop.hpp"
#include "runtime/timers/async_delay.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/host/hosted_service_lifecycle.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/streams/stream_host_service.hpp"
#include <zlink.hpp>
#include <zlink/framework.hpp>
#include <cassert>
#include <future>
#include <string_view>

namespace
{
std::atomic_bool fail_wait{false};
std::atomic_bool fail_timer_wait{false};
thread_local bool stream_worker = false;
std::atomic_bool fail_poller{false};
bool fail_socket = false;
bool check_close_lane = false;
bool closed_on_lane = false;
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
    if (fail_poller.exchange (false))
        return ZLINK_CLOSE_BUSY;
    return __real_zlink_poller_destroy (handle);
}
extern "C" zlink_close_result_t __real_zlink_close (void *);
extern "C" zlink_close_result_t __wrap_zlink_close (void *handle)
{
    ++socket_attempts;
    if (check_close_lane && zlink::framework::runtime::state_lane_t::current ())
        closed_on_lane = true;
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
    if (fail_timer_wait.exchange (false) || (stream_worker && fail_wait.exchange (false))) {
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
    explicit failing_stop_service_t (zlink::framework::app_t &app,
                                     bool timer,
                                     bool runtime_report = false) :
        _app (app), _timer (timer), _runtime_report (runtime_report)
    {
    }
    zlink::framework::task_t<void> start (zlink::framework::service_provider_t &) override
    {
        if (_runtime_report)
            _runtime_failures->report (
              std::make_exception_ptr (std::runtime_error ("runtime-only failure")));
        else if (!_timer)
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
    bool _runtime_report;
};

int main (int argc, char **argv)
{
    assert (argc == 2);
    const std::string_view owner = argv[1];
    if (owner == "delay-close" || owner == "delay-wait" || owner == "delay-success") {
        if (owner == "delay-close")
            fail_poller = true;
        else if (owner == "delay-wait")
            fail_timer_wait = true;
        const auto result = zlink::framework::detail::delay (std::chrono::milliseconds (1))
                              .result_for (std::chrono::seconds (1));
        assert (result);
        if (owner == "delay-success")
            assert (*result);
        else
            assert (!*result && result->exception ());
        return 0;
    }
    if (owner == "timer-self-close") {
        auto failures = std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ();
        zlink::framework::detail::core_timer_drain_loop_t timer (failures);
        std::promise<void> stopped;
        timer.start (std::chrono::milliseconds (1), 1, [&] (std::uint64_t) {
            timer.close ();
            stopped.set_value ();
        });
        assert (stopped.get_future ().wait_for (std::chrono::seconds (1))
                == std::future_status::ready);
        timer.close ();
        failures->rethrow_if_failed ();
        assert (!timer.valid ());
        return 0;
    }
    if (owner == "timer-concurrent-close") {
        auto failures = std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ();
        zlink::framework::detail::core_timer_drain_loop_t timer (failures);
        std::promise<void> entered, release;
        auto released = release.get_future ();
        timer.start (std::chrono::milliseconds (1), 1, [&] (std::uint64_t) {
            entered.set_value ();
            released.wait ();
            timer.close ();
        });
        entered.get_future ().wait ();
        auto first = std::async (std::launch::async, [&] { timer.close (); });
        auto second = std::async (std::launch::async, [&] { timer.close (); });
        const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (1);
        while (first.wait_for (std::chrono::milliseconds::zero ()) != std::future_status::ready
               && second.wait_for (std::chrono::milliseconds::zero ()) != std::future_status::ready
               && std::chrono::steady_clock::now () < deadline)
            std::this_thread::yield ();
        assert (first.wait_for (std::chrono::milliseconds::zero ()) == std::future_status::ready
                || second.wait_for (std::chrono::milliseconds::zero ())
                     == std::future_status::ready);
        release.set_value ();
        first.get ();
        second.get ();
        failures->rethrow_if_failed ();
        return 0;
    }
    if (owner == "native-channel-continue") {
        zlink::framework::zlink_builder_t builder =
          zlink::framework::test::runtime_failure_builder ();
        builder.channel ("first").enable_publisher (false).bind ("tcp://127.0.0.1:0");
        builder.channel ("second").enable_publisher (false).bind ("tcp://127.0.0.1:0");
        auto runtime = zlink::framework::detail::channel_runtime_t::from (builder.message_bus ());
        runtime.bind_core_context (std::make_shared<zlink::context_t> ());
        runtime.initialize_manual_channel_publishers ();
        const auto before = socket_attempts;
        fail_socket = true;
        bool failed = false;
        try {
            runtime.close ();
        }
        catch (const zlink::close_error_t &) {
            failed = true;
        }
        assert (failed && socket_attempts == before + 2);
        runtime.close ();
        assert (socket_attempts == before + 3);
        return 0;
    }
    if (owner == "cleanup-continue" || owner == "cleanup-multiple") {
        struct resource_t
        {
            int &attempts;
            bool fail;
            void close ()
            {
                ++attempts;
                if (std::exchange (fail, false))
                    throw std::runtime_error ("injected close failure");
            }
        };
        int first_attempts = 0, second_attempts = 0;
        auto first = std::make_unique<resource_t> (resource_t{first_attempts, true});
        auto second =
          std::make_unique<resource_t> (resource_t{second_attempts, owner == "cleanup-multiple"});
        bool failed = false;
        std::string failure_message;
        try {
            zlink::framework::runtime::runtime_failure_collector_t::close_resources (first, second);
        }
        catch (const std::runtime_error &error) {
            failed = true;
            failure_message = error.what ();
        }
        if (owner == "cleanup-multiple") {
            assert (failed && first && second && first_attempts == 1 && second_attempts == 1);
            assert (failure_message.find ("; ") != std::string::npos);
            zlink::framework::runtime::runtime_failure_collector_t::close_resources (first, second);
            assert (!first && !second && first_attempts == 2 && second_attempts == 2);
            return 0;
        }
        assert (failed && first && !second && second_attempts == 1);
        zlink::framework::runtime::runtime_failure_collector_t::close_resources (first, second);
        assert (!first && first_attempts == 2 && second_attempts == 1);
        return 0;
    }
    if (owner == "server-close-lane" || owner == "client-close-lane"
        || owner == "server-close-retry" || owner == "client-close-retry") {
        namespace rt = zlink::framework::runtime;
        rt::protocol::client_server_server_admission_t descriptor{
          "close-lane",
          {'s'},
          1,
          1,
          100,
          rt::mesh::service_node_state_t::serving,
          "default",
          16 * 1024 * 1024,
          "tcp://127.0.0.1:0"};
        auto failures = std::make_shared<rt::runtime_failure_collector_t> ();
        rt::client_server::raw_client_server_server_options_t options{descriptor};
        options.runtime_failures = failures;
        rt::client_server::raw_client_server_server_t server (options);
        server.start ();
        check_close_lane = true;
        const auto retry_close = [] (auto &transport) {
            const auto before = socket_attempts;
            fail_socket = true;
            bool failed = false;
            try {
                transport.close ();
            }
            catch (const std::exception &) {
                failed = true;
            }
            assert (failed && socket_attempts == before + 1);
            transport.close ();
            assert (socket_attempts == before + 2);
        };
        if (owner == "server-close-lane") {
            server.close ();
        } else if (owner == "server-close-retry") {
            retry_close (server);
        } else {
            rt::client_server::raw_client_server_client_options_t client_options;
            client_options.runtime_failures = failures;
            client_options.client_routing_id = {'c'};
            client_options.admission = {"close-lane", "default", 16 * 1024 * 1024};
            client_options.expected_server = server.descriptor ();
            rt::client_server::raw_client_server_client_t client (client_options);
            client.start ();
            if (owner == "client-close-retry")
                retry_close (client);
            else
                client.close ();
        }
        server.close ();
        assert (!closed_on_lane);
        return 0;
    }
    if (owner == "channel-publisher" || owner == "channel-poller") {
        zlink::framework::zlink_builder_t builder =
          zlink::framework::test::runtime_failure_builder ();
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
    if (owner == "host-stop" || owner == "host-timer" || owner == "host-runtime-report") {
        auto app = zlink::framework::app_t::create ();
        app.add_zlink_framework ();
        if (owner == "host-stop")
            app.advanced ()
              .zlink ()
              .channel ("close-failure")
              .enable_publisher (false)
              .bind ("tcp://127.0.0.1:0");
        app.add_hosted_service (std::make_unique<failing_stop_service_t> (
          app, owner == "host-timer", owner == "host-runtime-report"));
        const auto exit_code = app.run (0, nullptr);
        if (owner == "host-runtime-report") {
            assert (exit_code == 0);
            const auto &records = app.logging ().captured_records ();
            assert (std::any_of (records.begin (), records.end (), [] (const auto &record) {
                return record.message.find ("runtime-only failure") != std::string::npos;
            }));
            return 0;
        }
        assert (exit_code != 0);
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
        zlink::framework::zlink_builder_t builder =
          zlink::framework::test::runtime_failure_builder ();
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
        auto monitoring = zlink::framework::test::runtime_failure_monitoring ();
        zlink::framework::runtime::stream_host_service_t host (
          monitoring->runtime_failures, runtime, runtime.snapshots (),
          {{"close-session",
            [] (zlink::framework::service_provider_t &)
              -> zlink::framework::packet_stream_session_t & {
                throw std::logic_error (
                  "session creation is unexpected before the injected worker failure");
            }}},
          std::chrono::seconds{30}, mesh);
        if (owner == "stream-worker")
            host.bind_monitoring (monitoring);
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
            options.runtime_failures =
              std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ();
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
        client.close ();
        return 0;
    }
    if (owner == "timer") {
        zlink::framework::detail::core_timer_drain_loop_t timer (
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ());
        std::promise<void> fired;
        timer.start (std::chrono::milliseconds (1), 1, [&] (std::uint64_t) { fired.set_value (); });
        assert (fired.get_future ().wait_for (std::chrono::seconds (1))
                == std::future_status::ready);
        verify_poller_close (timer);
        assert (!timer.valid ());
        return 0;
    }
    if (owner == "fanout-poller") {
        zlink::framework::runtime::fanout::raw_fanout_subscriber_t subscriber (
          nullptr, {}, std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ());
        verify_poller_close (subscriber);
        return 0;
    }
    if (owner == "fanout-publisher") {
        zlink::framework::runtime::fanout::raw_fanout_publisher_t publisher (
          "tcp://127.0.0.1:0", {}, false, std::nullopt,
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ());
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
        zlink::framework::runtime::fanout::raw_fanout_subscriber_t subscriber (
          nullptr, {}, std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ());
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
    options.runtime_failures =
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ();
    options.descriptor = {"close-failure", {'c'}, 1, 1, "tcp://127.0.0.1:0", {{"alpha", 100}}};
    zlink::framework::runtime::mesh::raw_mesh_node_owner_t node (options);
    node.start ();
    if (owner == "mesh-close-lane") {
        check_close_lane = true;
        node.close ();
        assert (!closed_on_lane);
        return 0;
    }
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
