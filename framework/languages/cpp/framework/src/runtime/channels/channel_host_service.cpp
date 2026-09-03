/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/channels/channel_host_service.hpp"
#include "runtime/configuration/service_scope.hpp"

#include "runtime/channels/channel_packet_dispatcher.hpp"
#include "runtime/channels/channel_reply_writer.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/channels/channel_runtime_manager.hpp"
#include "runtime/channels/channel_socket_options.hpp"
#include "runtime/channels/socket_monitor_event.hpp"
#include "runtime/backend/raw_binding_adapter.hpp"
#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"

#include <zlink/Contracts/Eventing/poller.hpp>
#include <zlink/Contracts/Core/context.hpp>
#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Eventing/events.hpp>
#include <zlink/Contracts/Eventing/monitor.hpp>
#include <zlink/Contracts/Eventing/poll_event.hpp>
#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/Contracts/Messaging/operation_contracts.hpp>
#include <zlink/Contracts/Messaging/received.hpp>
#include <zlink/Contracts/Messaging/topic_message.hpp>
#include <zlink/Contracts/Sockets/message_socket_contracts.hpp>
#include <zlink/Contracts/Sockets/pubsub_socket_contracts.hpp>
#include <zlink/Contracts/Sockets/results.hpp>
#include <zlink/Contracts/Sockets/routed_socket_contracts.hpp>

#include <algorithm>
#include <chrono>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <utility>

namespace zlink::framework::runtime
{

class channel_host_service_t::server_loop_t
{
  public:
    server_loop_t (message_bus_t bus,
                   std::string channel_name,
                   std::vector<std::string> endpoints,
                   std::optional<zlink::routing_id_t> routing_id,
                   channel_capability_snapshot_t capability,
                   service_provider_t &services,
                   serializer_registry_t &serializers,
                   const handler_registry_t &handlers,
                   std::atomic_bool &stop,
                   std::shared_ptr<zlink::context_t> core_context,
                   std::shared_ptr<application_job_queue_t> application_jobs) :
        _runtime (detail::channel_runtime_t::from (bus)),
        _channel_name (std::move (channel_name)),
        _endpoints (std::move (endpoints)),
        _capability (std::move (capability)),
        _services (&services),
        _serializers (&serializers),
        _handlers (&handlers),
        _stop (&stop),
        _application_jobs (std::move (application_jobs)),
        _context (std::move (core_context)),
        _router (std::make_unique<zlink::router_socket_t> (*_context))
    {
        detail::apply_weighted_channel_socket_options (*_router, _capability);
        if (routing_id) {
            _router->set_routing_id (*routing_id);
        }
        _monitor = _router->monitor_open (
          zlink::monitor_event::connected | zlink::monitor_event::accepted
          | zlink::monitor_event::connection_ready | zlink::monitor_event::disconnected
          | zlink::monitor_event::closed | zlink::monitor_event::handshake_failed_no_detail
          | zlink::monitor_event::handshake_failed_protocol
          | zlink::monitor_event::handshake_failed_auth);
        for (const auto &endpoint : _endpoints) {
            _router->bind (endpoint);
        }
        const auto hardware_workers = static_cast<std::size_t> (
          std::max (1u, std::thread::hardware_concurrency ()));
        const auto max_handler_workers = std::max<std::size_t> (
          1, std::min<std::size_t> (hardware_workers, 8));
        _handler_executor = std::make_unique<offload_executor_t> (
          0, max_handler_workers, dispatch_limits::application_mailbox_messages,
          std::chrono::milliseconds (100), "zlink-channel-server");
        _poller.add (*_router, zlink::poll_event_flag_t::pollin, 1);
        _poller.add (_monitor, zlink::poll_event_flag_t::pollin, 2);
    }

    ~server_loop_t () { stop (); }

    void run ()
    {
        while (!_stop->load (std::memory_order_acquire)) {
            apply_runtime_options ();
            flush_replies ();
            zlink::poll_event_t readiness;
            std::size_t ready_count = 0;
            try {
                ready_count = _poller.wait (
                  &readiness, 1, std::chrono::milliseconds (50));
            }
            catch (...) {
                break;
            }
            if (_stop->load (std::memory_order_acquire)) {
                break;
            }
            if (ready_count != 1) {
                continue;
            }
            const short revents = static_cast<short> (readiness.revents);
            const short pollin =
              static_cast<short> (zlink::poll_event_flag_t::pollin);
            if (readiness.slot == 2) {
                if ((revents & pollin) != 0) {
                    drain_monitor_events ();
                }
                continue;
            }
            if (readiness.slot != 1 || (revents & pollin) == 0) {
                continue;
            }
            auto permit = _application_jobs->wait_for_supply_blocking ();
            if (!permit)
                break;
            const int rc = _router->recv (_received, zlink::recv_flags_t::dontwait);
            if (rc == static_cast<int> (zlink::recv_result_t::no_data)) {
                continue;
            }
            if (rc != static_cast<int> (zlink::recv_result_t::ok)) {
                continue;
            }
            if (is_drained ()) {
                _received.close ();
                continue;
            }
            dispatch_async (std::make_shared<zlink::received_t> (std::move (_received)),
                            std::move (*permit));
        }
        if (_handler_executor) {
            _handler_executor->drain ();
        }
        flush_replies ();
        drain_monitor_events ();
    }

    void stop () noexcept
    {
        if (_handler_executor) {
            _handler_executor->drain ();
        }
        flush_replies ();
        try {
            _poller.close ();
        }
        catch (...) {
        }
        if (_monitor.valid ()) {
            try {
                _monitor.close ();
            }
            catch (...) {
            }
        }
        if (_router) {
            try {
                _router->close ();
            }
            catch (...) {
            }
        }
        clear_replies ();
        if (_router) {
            _router.reset ();
        }
    }

  private:
    struct completed_reply_t
    {
        std::optional<zlink::routing_id_t> routing_id;
        std::optional<zlink::reply_token_t> reply_token;
        zlink::framework::runtime::messaging::message_parts_t parts;
    };

    void dispatch_async (std::shared_ptr<zlink::received_t> received,
                         application_job_queue_t::permit_t permit)
    {
        auto application_job =
          std::make_shared<application_job_queue_t::permit_t> (
            std::move (permit));
        application_job->mark_queued ();
        auto routing_id = received->routing_id ();
        const auto reply_token = received->reply_token ();
        auto request_parts =
          zlink::framework::runtime::messaging::message_parts_t (
            detail::backend::copy_binding_messages (received->parts ()));
        auto shared_parts = std::make_shared<
          zlink::framework::runtime::messaging::message_parts_t> (
            std::move (request_parts));
        const auto rejection_routing_id = routing_id;
        received->close ();
        auto work = [this, routing_id = std::move (routing_id), reply_token,
                     shared_parts,
                     application_job] () mutable {
            try {
            detail::channel_packet_dispatcher_t dispatcher (_runtime);
            auto scope = detail::service_scope_t::create (
              *_services, detail::service_scope_kind_t::handler_invocation);
            auto reply = dispatcher.dispatch_server_message (
              _channel_name, *shared_parts, scope.provider (), *_serializers,
              *_handlers, [application_job] {
                  application_job->release_for_handler_entry ();
              });
            if (!reply || reply.value ().size () == 0 || !routing_id || !reply_token) {
                return;
            }
            std::lock_guard<std::mutex> reply_lock (_replies_mutex);
            _replies.push_back (completed_reply_t{
              std::move (routing_id), std::move (reply_token),
              std::move (reply.value ())});
            }
            catch (...) {
            }
        };
        if (!_handler_executor
            || !_handler_executor->try_submit_internal (std::move (work))) {
            (void) rejection_routing_id;
        }
    }

    void reply (completed_reply_t completed)
    {
        if (!completed.routing_id || !completed.reply_token || completed.parts.size () == 0) {
            return;
        }
        std::vector<zlink::message_t> copied;
        copied.reserve (completed.parts.size ());
        for (std::size_t index = 0; index < completed.parts.size (); ++index) {
            copied.push_back (completed.parts[index]);
        }
        auto operation =
          _router->reply (*completed.routing_id, *completed.reply_token).message (copied[0]);
        for (std::size_t index = 1; index < copied.size (); ++index) {
            operation = std::move (operation).message (copied[index]);
        }
        std::move (operation).submit ();
    }

    void flush_replies ()
    {
        for (;;) {
            completed_reply_t completed;
            {
                std::lock_guard<std::mutex> lock (_replies_mutex);
                if (_replies.empty ()) {
                    return;
                }
                completed = std::move (_replies.front ());
                _replies.pop_front ();
            }
            try {
                reply (std::move (completed));
            }
            catch (...) {
                const auto error = std::current_exception ();
                detail::dispatch_error_reporter_t (_runtime.dispatch_options_ref ())
                  .report_lazy ([&] {
                      return message_dispatch_error_event_t{
                        dispatch_error_surface_t::channel,
                        dispatch_message_kind_t::response,
                        dispatch_error_reason_t::unexpected_reply,
                        dispatch_error_action_t::drop,
                        std::nullopt,
                        _channel_name,
                        std::nullopt,
                        std::nullopt,
                        std::nullopt,
                        std::nullopt,
                        std::nullopt,
                        error};
                  });
            }
        }
    }

    void clear_replies () noexcept
    {
        std::lock_guard<std::mutex> lock (_replies_mutex);
        _replies.clear ();
    }

    void apply_runtime_options ()
    {
        const auto peer_weight = _runtime.server_peer_weight_override (_channel_name);
        if (!peer_weight
            || (_applied_peer_weight && *_applied_peer_weight == *peer_weight)) {
            return;
        }
        _router->options ().peer_weight (
          zlink::peer_weight_t::value (
            static_cast<std::uint32_t> (
              std::min (*peer_weight, 100))));
        _applied_peer_weight = *peer_weight;
    }

    bool is_drained () const noexcept
    {
        return _applied_peer_weight && *_applied_peer_weight == 0;
    }

    void drain_monitor_events ()
    {
        if (!_monitor.valid ()) {
            return;
        }
        for (;;) {
            zlink::poll_event_t readiness;
            try {
                if (_poller.wait (
                      &readiness, 1, std::chrono::milliseconds::zero ())
                      != 1
                    || readiness.slot != 2
                    || (static_cast<short> (readiness.revents)
                        & static_cast<short> (zlink::poll_event_flag_t::pollin))
                         == 0) {
                    return;
                }
            }
            catch (...) {
                return;
            }
            std::optional<zlink::monitor_event_t> event;
            try {
                event = _monitor.recv (zlink::recv_flags_t::dontwait);
            }
            catch (...) {
                return;
            }
            if (!event) {
                return;
            }
            const auto kind = detail::map_socket_monitor_event (event->event);
            if (!kind) {
                continue;
            }
            if (!event->remote_addr.empty ()) {
                if (*kind == detail::socket_event_kind_t::connected) {
                    _pending_handshake_remotes.insert (event->remote_addr);
                } else if (*kind == detail::socket_event_kind_t::connection_ready) {
                    _pending_handshake_remotes.erase (event->remote_addr);
                } else if (*kind == detail::socket_event_kind_t::disconnected
                           && _pending_handshake_remotes.erase (event->remote_addr) != 0) {
                    _runtime.publish_socket_event (
                      _channel_name, detail::socket_event_kind_t::handshake_failed, event->local_addr,
                      event->remote_addr);
                }
            }
            _runtime.publish_socket_event (_channel_name, *kind, event->local_addr,
                                           event->remote_addr);
        }
    }

    detail::channel_runtime_t _runtime;
    std::string _channel_name;
    std::vector<std::string> _endpoints;
    channel_capability_snapshot_t _capability;
    service_provider_t *_services;
    serializer_registry_t *_serializers;
    const handler_registry_t *_handlers;
    std::atomic_bool *_stop;
    std::shared_ptr<application_job_queue_t> _application_jobs;
    std::shared_ptr<zlink::context_t> _context;
    std::unique_ptr<zlink::router_socket_t> _router;
    zlink::received_t _received;
    zlink::socket_monitor_t _monitor;
    zlink::poller_t _poller;
    std::set<std::string> _pending_handshake_remotes;
    std::optional<int> _applied_peer_weight;
    std::unique_ptr<offload_executor_t> _handler_executor;
    std::mutex _replies_mutex;
    std::deque<completed_reply_t> _replies;
};


class channel_host_service_t::subscriber_loop_t
{
  public:
    subscriber_loop_t (message_bus_t bus,
                       std::string channel_name,
                       detail::channel_runtime_bundle_t &bundle,
                       channel_capability_snapshot_t capability,
                       service_provider_t &services,
                       serializer_registry_t &serializers,
                       const handler_registry_t &handlers,
                       std::atomic_bool &stop,
                       std::shared_ptr<zlink::context_t> core_context,
                       std::shared_ptr<application_job_queue_t> application_jobs) :
        _runtime (detail::channel_runtime_t::from (bus)),
        _channel_name (std::move (channel_name)),
        _bundle (&bundle),
        _capability (std::move (capability)),
        _services (&services),
        _serializers (&serializers),
        _handlers (&handlers),
        _stop (&stop),
        _application_jobs (std::move (application_jobs)),
        _context (std::move (core_context)),
        _subscriber (std::make_unique<zlink::sub_socket_t> (*_context))
    {
        detail::apply_common_channel_socket_options (*_subscriber, _capability);
        _subscriber->set_subscription ("");
        apply_runtime_connections ();
        const auto hardware_workers = static_cast<std::size_t> (
          std::max (1u, std::thread::hardware_concurrency ()));
        const auto max_handler_workers = std::max<std::size_t> (
          1, std::min<std::size_t> (hardware_workers, 8));
        _handler_executor = std::make_unique<offload_executor_t> (
          0, max_handler_workers, dispatch_limits::application_mailbox_messages,
          std::chrono::milliseconds (100), "zlink-channel-subscriber");
        _poller.add (*_subscriber, zlink::poll_event_flag_t::pollin, 1);
    }

    ~subscriber_loop_t () { stop (); }

    void run ()
    {
        while (!_stop->load (std::memory_order_acquire)) {
            apply_runtime_connections ();
            zlink::poll_event_t readiness;
            std::size_t ready_count = 0;
            try {
                ready_count = _poller.wait (
                  &readiness, 1, std::chrono::milliseconds (50));
            }
            catch (...) {
                break;
            }
            if (_stop->load (std::memory_order_acquire)
                || ready_count != 1
                || readiness.slot != 1
                || (static_cast<short> (readiness.revents)
                    & static_cast<short> (zlink::poll_event_flag_t::pollin))
                     == 0
                ) {
                continue;
            }
            auto permit = _application_jobs->wait_for_supply_blocking ();
            if (!permit)
                break;
            const int rc = _subscriber->subscribe (
              _received_message, zlink::recv_flags_t::dontwait);
            if (rc == static_cast<int> (zlink::recv_result_t::no_data)) {
                continue;
            }
            if (rc != static_cast<int> (zlink::recv_result_t::ok)) {
                continue;
            }
            dispatch_async (
              std::make_shared<zlink::topic_message_t> (
                std::move (_received_message)),
              std::move (*permit));
        }
    }

    void stop () noexcept
    {
        if (_handler_executor) {
            _handler_executor->drain ();
        }
        try {
            _poller.close ();
        }
        catch (...) {
        }
        if (_subscriber) {
            try {
                _subscriber->close ();
            }
            catch (...) {
            }
        }
        if (_subscriber) {
            _subscriber.reset ();
        }
    }

  private:
    void dispatch_async (std::shared_ptr<zlink::topic_message_t> message,
                         application_job_queue_t::permit_t permit)
    {
        auto application_job =
          std::make_shared<application_job_queue_t::permit_t> (
            std::move (permit));
        application_job->mark_queued ();
        auto parts = zlink::framework::runtime::messaging::message_parts_t (
          detail::backend::copy_binding_messages (message->parts ()));
        auto shared_parts = std::make_shared<
          zlink::framework::runtime::messaging::message_parts_t> (std::move (parts));
        auto work = [this, message = std::move (message), shared_parts,
                     application_job] () mutable {
            try {
            detail::channel_packet_dispatcher_t dispatcher (_runtime);
            auto scope = detail::service_scope_t::create (
              *_services, detail::service_scope_kind_t::handler_invocation);
            (void) dispatcher.dispatch_server_message (_channel_name, *shared_parts,
                                                       scope.provider (),
                                                       *_serializers, *_handlers,
                                                       [application_job] {
                                                           application_job
                                                             ->release_for_handler_entry ();
                                                       });
            }
            catch (...) {
            }
        };
        if (!_handler_executor
            || !_handler_executor->try_submit_internal (std::move (work))) {
        }
    }

    void apply_runtime_connections ()
    {
        if (_bundle == nullptr || _subscriber == nullptr) {
            return;
        }
        std::set<std::string> desired;
        for (const auto &endpoint : _bundle->list_manual_connections ()) {
            desired.insert (endpoint);
            if (_connected.insert (endpoint).second) {
                _subscriber->connect (endpoint);
            }
        }
        for (auto it = _connected.begin (); it != _connected.end ();) {
            if (desired.find (*it) != desired.end ()) {
                ++it;
                continue;
            }
            try {
                _subscriber->disconnect (*it);
            }
            catch (...) {
            }
            it = _connected.erase (it);
        }
    }

    detail::channel_runtime_t _runtime;
    std::string _channel_name;
    detail::channel_runtime_bundle_t *_bundle;
    channel_capability_snapshot_t _capability;
    service_provider_t *_services;
    serializer_registry_t *_serializers;
    const handler_registry_t *_handlers;
    std::atomic_bool *_stop;
    std::shared_ptr<application_job_queue_t> _application_jobs;
    std::shared_ptr<zlink::context_t> _context;
    std::unique_ptr<zlink::sub_socket_t> _subscriber;
    zlink::topic_message_t _received_message;
    zlink::poller_t _poller;
    std::set<std::string> _connected;
    std::unique_ptr<offload_executor_t> _handler_executor;
};

channel_host_service_t::channel_host_service_t (message_bus_t bus,
                                                std::vector<channel_snapshot_t> channels,
                                                handler_registry_t &handlers,
                                                serializer_registry_t &serializers,
                                                std::shared_ptr<application_job_queue_t> application_jobs) :
    _bus (std::move (bus)),
    _channels (std::move (channels)),
    _handlers (&handlers),
    _serializers (&serializers),
    _core_context (detail::channel_runtime_t::from (_bus).core_context ()),
    _application_jobs (
      application_jobs
        ? std::move (application_jobs)
        : std::make_shared<application_job_queue_t> (
            application_job_queue_configuration_t{
              application_job_queue_profile_t::balanced,
              std::nullopt, 1,
              static_cast<std::uint32_t> (
                std::numeric_limits<std::int32_t>::max ())}))
{
}

channel_host_service_t::~channel_host_service_t () = default;

task_t<void> channel_host_service_t::start (service_provider_t &services)
{
    _services = &services;
    auto manager = detail::channel_runtime_manager_t::from (_bus);
    const bool shared_client_server_runtime_active =
      detail::channel_runtime_t::from (_bus).auto_connect_active ();
    _stop.store (false, std::memory_order_release);
    for (const auto &channel : _channels) {
        if (!channel.server.enabled
            || shared_client_server_runtime_active
            || channel.server.bind_endpoints.empty ()) {
            continue;
        }
        auto loop = std::make_unique<server_loop_t> (
          _bus, channel.name, channel.server.bind_endpoints, channel.server.routing_id,
          channel.server, services, *_serializers, *_handlers, _stop,
          _core_context, _application_jobs);
        auto *raw = loop.get ();
        _loops.push_back (std::move (loop));
        _threads.emplace_back ([raw] { raw->run (); });
    }
    for (const auto &channel : _channels) {
        if (!channel.subscriber.enabled
            || channel.subscriber.discovery
            || (!channel.subscriber.discovery && channel.subscriber.connect_endpoints.empty ())) {
            continue;
        }
        auto &bundle = manager.get_or_create_subscriber_bundle (channel.name);
        auto loop = std::make_unique<subscriber_loop_t> (
          _bus, channel.name, bundle, channel.subscriber, services, *_serializers, *_handlers,
          _stop, _core_context, _application_jobs);
        auto *raw = loop.get ();
        _subscriber_loops.push_back (std::move (loop));
        _threads.emplace_back ([raw] { raw->run (); });
    }
    return task_t<void> (result_t<void>::success ());
}

void channel_host_service_t::request_stop () noexcept
{
    _stop.store (true, std::memory_order_release);
    if (_application_jobs)
        _application_jobs->stop ();
}

void channel_host_service_t::stop () noexcept
{
    request_stop ();
    for (auto &thread : _threads) {
        if (thread.joinable ()) {
            thread.join ();
        }
    }
    for (auto &loop : _loops) {
        loop->stop ();
    }
    for (auto &loop : _subscriber_loops) {
        loop->stop ();
    }
    _threads.clear ();
    _loops.clear ();
    _subscriber_loops.clear ();
    _services = nullptr;
}

} // namespace zlink::framework::runtime
