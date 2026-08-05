/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/fanout/fanout_location_runtime.hpp"
#include "runtime/eventing/runtime_wake_pipe.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/configuration/service_scope.hpp"

#include <zlink/Contracts/Messaging/message.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <random>
#include <stdexcept>
#include <utility>

namespace zlink::framework::runtime::fanout
{

namespace
{

constexpr std::string_view default_security_identity = "default";

framework_runtime_state_t current_state (
  const location_runtime_t &locations)
{
    return locations.draining ()
             ? framework_runtime_state_t::draining
             : framework_runtime_state_t::serving;
}

mesh::service_node_state_t service_state (
  framework_runtime_state_t state)
{
    switch (state) {
        case framework_runtime_state_t::preparing:
            return mesh::service_node_state_t::preparing;
        case framework_runtime_state_t::serving:
            return mesh::service_node_state_t::serving;
        case framework_runtime_state_t::relocating:
        case framework_runtime_state_t::relocated:
        case framework_runtime_state_t::draining:
            return mesh::service_node_state_t::draining;
        case framework_runtime_state_t::stopped:
            return mesh::service_node_state_t::stopped;
        case framework_runtime_state_t::error:
            return mesh::service_node_state_t::error;
        default:
            throw std::invalid_argument (
              "invalid fanout runtime state");
    }
}

} // namespace

struct fanout_location_runtime_t::publisher_entry_t
{
    std::unique_ptr<raw_fanout_publisher_t> owner;
    fanout_publisher_descriptor_t descriptor;
};

struct fanout_location_runtime_t::subscriber_entry_t
{
    std::string channel_name;
    std::unique_ptr<raw_fanout_subscriber_t> owner;
};

fanout_location_runtime_t::fanout_location_runtime_t (
  message_bus_t bus,
  std::vector<channel_snapshot_t> channels,
  location_runtime_t &locations,
  location_repository_t &store,
  location_repository_t &leases,
  service_provider_t &services,
  serializer_registry_t &serializers,
  const handler_registry_t &handlers) :
    _bus (std::move (bus)),
    _channel_runtime (detail::channel_runtime_t::from (_bus)),
    _channels (std::move (channels)),
    _locations (&locations),
    _store (&store),
    _leases (&leases),
    _services (&services),
    _serializers (&serializers),
    _handlers (&handlers)
{
}

fanout_location_runtime_t::~fanout_location_runtime_t () noexcept
{
    stop ();
}

bool fanout_location_runtime_t::empty () const noexcept
{
    return std::none_of (
      _channels.begin (), _channels.end (),
      [] (const auto &channel) {
          return (channel.publisher.enabled
                  && channel.publisher.discovery)
                 || (channel.subscriber.enabled
                     && channel.subscriber.discovery);
      });
}

void fanout_location_runtime_t::start ()
{
    if (empty ())
        return;
    const auto owner = _locations->current_owner_token ();
    if (!owner)
        throw std::runtime_error (
          "fanout discovery requires an active owner lease");
    _stop.store (false, std::memory_order_release);
    try {
        _subscriber_poller = std::make_unique<zlink::poller_t> ();
        if (!_wake_pipe.open ())
            throw std::runtime_error ("fanout runtime wake pipe creation failed");
        _subscriber_poller->add_fd (
          _wake_pipe.read_fd (),
          zlink::poll_event_flag_t::pollin,
          std::numeric_limits<std::uintptr_t>::max ());
        for (const auto &channel : _channels) {
            if (channel.publisher.enabled
                && channel.publisher.discovery)
                start_publisher (channel, *owner);
            if (channel.subscriber.enabled
                && channel.subscriber.discovery)
                start_subscriber (channel);
        }
        reconcile_subscribers ();
        _channel_runtime.mark_auto_connect_active ();
        _thread = std::thread ([this] { run (); });
    }
    catch (...) {
        stop ();
        throw;
    }
}

void fanout_location_runtime_t::start_publisher (
  const channel_snapshot_t &channel,
  const location_owner_token_t &owner)
{
    if (!channel.publisher.routing_id
        || channel.publisher.bind_endpoints.size () != 1)
        throw std::invalid_argument (
          "discovery fanout publisher requires one routing id and one bind endpoint");
    auto raw = std::make_unique<raw_fanout_publisher_t> (
      channel.publisher.bind_endpoints.front ());
    raw->start ();
    fanout_publisher_descriptor_t descriptor{
      .channel_name = channel.name,
      .publisher_rid = *channel.publisher.routing_id,
      .lifecycle_generation =
        make_lifecycle_generation (),
      .descriptor_revision = 1,
      .endpoint = raw->endpoint (),
      .state = framework_runtime_state_t::serving,
      .security_identity =
        std::string (default_security_identity),
      .owner_id = owner.owner_id,
      .lease_generation = owner.lease_generation};
    const auto stored =
      _store
        ->update_fanout_publisher (
          descriptor,
          location_write_intent_t::new_claim)
        .result ()
        .value ();
    if (stored.status != location_write_status_t::stored) {
        raw->close ();
        throw std::runtime_error (
          "fanout publisher descriptor publication was fenced");
    }
    auto entry = std::make_unique<publisher_entry_t> ();
    entry->owner = std::move (raw);
    entry->descriptor = std::move (descriptor);
    _publishers.emplace (
      channel.name, std::move (entry));
    _channel_runtime.bind_fanout_transport (
      channel.name,
      [this, name = channel.name] (
        std::string topic,
        std::string packet_name,
        std::string content_type,
        zlink::message_t message,
        std::chrono::milliseconds timeout) {
          return publish (
            name, std::move (topic),
            std::move (packet_name),
            std::move (content_type),
            std::move (message), timeout);
      });
}

void fanout_location_runtime_t::start_subscriber (
  const channel_snapshot_t &channel)
{
    auto entry = std::make_unique<subscriber_entry_t> ();
    entry->channel_name = channel.name;
    entry->owner =
      std::make_unique<raw_fanout_subscriber_t> (_subscriber_poller.get ());
    _subscribers.emplace (
      channel.name, std::move (entry));
}

void fanout_location_runtime_t::run ()
{
    auto next_reconcile =
      std::chrono::steady_clock::now ();
    while (!_stop.load (std::memory_order_acquire)) {
        const auto now = std::chrono::steady_clock::now ();
        if (now >= next_reconcile) {
            try {
                publish_descriptors ();
                reconcile_subscribers ();
            }
            catch (...) {
                _locations->record_store_error ();
            }
            next_reconcile =
              now + _locations->options ().polling_interval;
        }
        pump ();
        if (_stop.load (std::memory_order_acquire))
            break;
        auto wake_at = next_reconcile;
        for (const auto &[_, publisher] : _publishers)
            wake_at = std::min (wake_at, publisher->owner->next_activity ());
        const auto after_pump = std::chrono::steady_clock::now ();
        if (wake_at <= after_pump)
            continue;
        wait_for_activity (std::chrono::duration_cast<std::chrono::milliseconds> (
          wake_at - after_pump));
    }
}

void fanout_location_runtime_t::wait_for_activity (
  std::chrono::milliseconds timeout) noexcept
{
    if (!_subscriber_poller || timeout <= std::chrono::milliseconds::zero ())
        return;
        try {
            zlink::poll_event_t readiness;
            const auto count =
              _subscriber_poller->wait (&readiness, 1, timeout);
            if (count == 1
                && readiness.source_kind == zlink::poll_source_kind_t::fd
                && readiness.fd == _wake_pipe.read_fd ())
                _wake_pipe.drain ();
    }
    catch (...) {
    }
}

void fanout_location_runtime_t::publish_descriptors ()
{
    const auto owner = _locations->current_owner_token ();
    if (!owner)
        return;
    std::lock_guard lock (_gate);
    for (auto &[_, publisher] : _publishers) {
        const auto state = current_state (*_locations);
        const bool new_owner =
          publisher->descriptor.owner_id != owner->owner_id
          || publisher->descriptor.lease_generation
               != owner->lease_generation;
        if (!new_owner
            && publisher->descriptor.state == state)
            continue;
        if (publisher->descriptor.descriptor_revision
            == static_cast<std::uint64_t> (
              std::numeric_limits<std::int64_t>::max ()))
            throw std::overflow_error (
              "fanout publisher descriptor revision is exhausted");
        auto descriptor = publisher->descriptor;
        ++descriptor.descriptor_revision;
        descriptor.state = state;
        descriptor.owner_id = owner->owner_id;
        descriptor.lease_generation =
          owner->lease_generation;
        const auto written =
          _store
            ->update_fanout_publisher (
              descriptor,
              new_owner
                ? location_write_intent_t::new_claim
                : location_write_intent_t::renew)
            .result ()
            .value ();
        if (written.status == location_write_status_t::stored)
            publisher->descriptor = std::move (descriptor);
    }
}

void fanout_location_runtime_t::reconcile_subscribers ()
{
    for (auto &[_, subscriber] : _subscribers)
        reconcile_subscriber (*subscriber);
}

void fanout_location_runtime_t::reconcile_subscriber (
  subscriber_entry_t &subscriber)
{
    std::vector<fanout_publisher_intent_t> desired;
    location_page_request_t page;
    do {
        const auto listed =
          _store
            ->list_fanout_publishers (
              subscriber.channel_name, page)
            .result ()
            .value ();
        for (const auto &descriptor : listed.items) {
            if (!owner_is_live (descriptor))
                continue;
            desired.push_back (
              fanout_publisher_intent_t{
                descriptor.publisher_rid.to_bytes (),
                descriptor.lifecycle_generation,
                descriptor.endpoint,
                service_state (descriptor.state)});
        }
        page.continuation_token =
          listed.continuation_token;
    } while (page.continuation_token);
    subscriber.owner->reconcile_automatic (desired);
}

void fanout_location_runtime_t::pump ()
{
    const auto now = std::chrono::steady_clock::now ();
    for (auto &[_, publisher] : _publishers)
        (void) publisher->owner->tick (now);
    std::vector<subscriber_entry_t *> subscribers;
    subscribers.reserve (_subscribers.size ());
    for (auto &[_, subscriber] : _subscribers)
        subscribers.push_back (subscriber.get ());
    if (subscribers.empty ())
        return;
    const auto start = _subscriber_pump_cursor % subscribers.size ();
    for (std::size_t offset = 0; offset < subscribers.size (); ++offset) {
        auto &subscriber = *subscribers[(start + offset) % subscribers.size ()];
        receive_batch_budget_t budget;
        while (budget.can_receive ()) {
            auto [status, received] =
              subscriber.owner->try_receive (now);
            if (status == fanout_receive_status_t::no_data)
                break;
            budget.account (
              received ? received->payload.payload.size () : 0);
            if (status != fanout_receive_status_t::application
                || !received) {
                if (budget.exhausted ())
                    break;
                continue;
            }
            try {
                const auto message =
                  zlink::message_t::from (
                    received->payload.payload);
                detail::inbound_message_context_t
                  inbound;
                inbound.message.channel_name =
                  subscriber.channel_name;
                inbound.message.packet_name =
                  received->payload.packet_name;
                inbound.message.content_type =
                  received->payload.content_type;
                inbound.topic = received->topic;
                auto scope =
                  zlink::framework::detail::service_scope_t::create (
                  *_services,
                  zlink::framework::detail::service_scope_kind_t::
                    handler_invocation);
                auto dispatched = _channel_runtime.dispatch_send (
                  subscriber.channel_name,
                  received->topic,
                  received->payload.packet_name,
                  scope.provider (), *_serializers, *_handlers,
                  message, inbound);
                if (!dispatched) {
                    zlink::framework::detail::dispatch_error_reporter_t (
                      _channel_runtime.dispatch_options ())
                      .report (message_dispatch_error_event_t{
                        .surface = dispatch_error_surface_t::channel,
                        .message_kind = dispatch_message_kind_t::publish,
                        .reason = zlink::framework::detail::dispatch_reason_from_error (
                          dispatched.error ()),
                        .action = dispatch_error_action_t::drop,
                        .packet_name = received->payload.packet_name,
                        .channel_name = subscriber.channel_name,
                        .topic = received->topic,
                        .exception = dispatched.error ()
                          ? std::make_exception_ptr (*dispatched.error ())
                          : std::exception_ptr{}});
                }
            }
            catch (...) {
            }
            if (budget.exhausted ())
                break;
        }
        (void) subscriber.owner->tick (now);
    }
    _subscriber_pump_cursor = (start + 1) % subscribers.size ();
}

result_t<void> fanout_location_runtime_t::publish (
  const std::string &channel_name,
  std::string topic,
  std::string packet_name,
  std::string content_type,
  zlink::message_t message,
  std::chrono::milliseconds timeout)
{
    static_cast<void> (timeout);
    std::lock_guard lock (_gate);
    const auto found = _publishers.find (channel_name);
    if (found == _publishers.end ()
        || found->second->descriptor.state
             != framework_runtime_state_t::serving)
        return result_t<void>::failure (
          framework_error_kind_t::unavailable,
          "fanout publisher is not serving");
    const auto submitted =
      found->second->owner->publish (
        topic,
        protocol::application_payload_t{
          std::move (packet_name),
          std::move (content_type),
          message.to_bytes ()});
    return submitted
             ? result_t<void>::success ()
             : result_t<void>::failure (
                 framework_error_kind_t::capacity_exceeded,
                 "fanout publish is backpressured");
}

void fanout_location_runtime_t::stop () noexcept
{
    const bool was_stopped =
      _stop.exchange (true, std::memory_order_acq_rel);
    for (const auto &[channel_name, _] : _publishers)
        _channel_runtime.unbind_fanout_transport (
          channel_name);
    _wake_pipe.signal ();
    if (_thread.joinable ())
        _thread.join ();
    if (!was_stopped || !_publishers.empty ()
        || !_subscribers.empty ()) {
        stop_subscribers ();
        stop_publishers ();
    }
    if (_subscriber_poller && _wake_pipe.read_fd () >= 0) {
        try {
            _subscriber_poller->remove_fd (_wake_pipe.read_fd ());
        }
        catch (...) {
        }
    }
    _wake_pipe.close ();
    if (_subscriber_poller) {
        try {
            _subscriber_poller->close ();
        }
        catch (...) {
        }
    }
    _subscriber_poller.reset ();
}

void fanout_location_runtime_t::stop_subscribers () noexcept
{
    for (auto &[_, subscriber] : _subscribers)
        subscriber->owner->close ();
    _subscribers.clear ();
}

void fanout_location_runtime_t::stop_publishers () noexcept
{
    std::lock_guard lock (_gate);
    for (auto &[_, publisher] : _publishers) {
        try {
            auto draining = publisher->descriptor;
            if (draining.descriptor_revision
                < static_cast<std::uint64_t> (
                  std::numeric_limits<std::int64_t>::max ())) {
                ++draining.descriptor_revision;
                draining.state =
                  framework_runtime_state_t::draining;
                const auto written =
                  _store
                    ->update_fanout_publisher (
                      draining,
                      location_write_intent_t::renew)
                    .result ()
                    .value ();
                if (written.status
                    == location_write_status_t::stored)
                    publisher->descriptor =
                      std::move (draining);
            }
            (void) _store
              ->remove_fanout_publisher (
                {publisher->descriptor.channel_name,
                 publisher->descriptor.publisher_rid},
                {publisher->descriptor.owner_id,
                 publisher->descriptor.lease_generation})
              .result ()
              .value ();
        }
        catch (...) {
        }
        publisher->owner->close ();
    }
    _publishers.clear ();
}

bool fanout_location_runtime_t::owner_is_live (
  const fanout_publisher_descriptor_t &descriptor) const
{
    const auto lease =
      _leases->read_owner_lease (descriptor.owner_id)
        .result ()
        .value ();
    const auto *found =
      std::get_if<owner_lease_found_t> (&lease);
    return found != nullptr
           && found->token.owner_id == descriptor.owner_id
           && found->token.lease_generation
                == descriptor.lease_generation
           && found->lease_expires_at > found->store_now;
}

std::uint64_t
fanout_location_runtime_t::make_lifecycle_generation ()
{
    static std::atomic_uint64_t counter{1};
    const auto random =
      (static_cast<std::uint64_t> (
         std::random_device{} ())
       << 32u)
      ^ static_cast<std::uint64_t> (
        std::random_device{} ());
    const auto time = static_cast<std::uint64_t> (
      std::chrono::steady_clock::now ()
        .time_since_epoch ()
        .count ());
    auto value =
      (random ^ time ^ counter.fetch_add (1))
      & static_cast<std::uint64_t> (
          std::numeric_limits<std::int64_t>::max ());
    return value == 0 ? 1 : value;
}

} // namespace zlink::framework::runtime::fanout
