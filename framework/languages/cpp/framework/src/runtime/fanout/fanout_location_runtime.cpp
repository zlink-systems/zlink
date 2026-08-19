/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/fanout/fanout_location_runtime.hpp"
#include "runtime/transport/listener_identity.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/configuration/service_scope.hpp"

#include <zlink/Contracts/Messaging/message.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <random>
#include <set>
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
    std::shared_ptr<raw_fanout_publisher_t> owner;
    fanout_publisher_descriptor_t descriptor;
};

struct fanout_location_runtime_t::subscriber_entry_t
{
    std::string channel_name;
    std::unique_ptr<raw_fanout_subscriber_t> owner;
    std::vector<fanout_publisher_intent_t> desired;
};

namespace
{

class fanout_observation_t final : public fanout_runtime_observation_t
{
  public:
    explicit fanout_observation_t (
      std::shared_ptr<fanout_location_runtime_t::observer_t> observer) :
        _observer (std::move (observer))
    {
    }

    ~fanout_observation_t () override { close (); }

    void close () override
    {
        if (_observer) {
            _observer->close ();
            _observer.reset ();
        }
    }

  private:
    std::shared_ptr<fanout_location_runtime_t::observer_t> _observer;
};

fanout_publisher_connection_state_t connection_state (
  raw_fanout_connection_state_t state)
{
    switch (state) {
        case raw_fanout_connection_state_t::connecting:
            return fanout_publisher_connection_state_t::connecting;
        case raw_fanout_connection_state_t::ready:
            return fanout_publisher_connection_state_t::ready;
        case raw_fanout_connection_state_t::disconnected:
            return fanout_publisher_connection_state_t::disconnected;
        case raw_fanout_connection_state_t::reconnecting:
            return fanout_publisher_connection_state_t::reconnecting;
    }
    return fanout_publisher_connection_state_t::disconnected;
}

std::string fanout_location_observation_source (
  const std::string &channel_name)
{
    return "location:" + channel_name;
}

std::string fanout_publisher_observation_source (
  const fanout_publisher_connection_snapshot_t &publisher)
{
    return "publisher:" + publisher.publisher_rid.to_hex () + ":"
           + std::to_string (publisher.lifecycle_generation);
}

} // namespace

fanout_location_runtime_t::fanout_location_runtime_t (
  message_bus_t bus,
  std::vector<channel_snapshot_t> channels,
  location_runtime_t &locations,
  location_repository_t &store,
  location_repository_t &leases,
  service_provider_t &services,
  serializer_registry_t &serializers,
  const handler_registry_t &handlers,
  std::map<std::string, std::string> publisher_advertise_hosts,
  std::shared_ptr<listener_status_registry_t> listener_statuses,
  std::shared_ptr<application_job_queue_t> application_jobs) :
    _bus (std::move (bus)),
    _channel_runtime (detail::channel_runtime_t::from (_bus)),
    _channels (std::move (channels)),
    _publisher_advertise_hosts (std::move (publisher_advertise_hosts)),
    _listener_statuses (std::move (listener_statuses)),
    _locations (&locations),
    _store (&store),
    _leases (&leases),
    _services (services),
    _serializers (&serializers),
    _handlers (&handlers),
    _application_jobs (
      application_jobs
        ? std::move (application_jobs)
        : std::make_shared<application_job_queue_t> (
            application_job_queue_configuration_t{
              application_job_queue_profile_t::balanced,
              static_cast<std::uint32_t> (
                std::numeric_limits<std::int32_t>::max ()),
              1,
              static_cast<std::uint32_t> (
                std::numeric_limits<std::int32_t>::max ())}))
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
        _wake_timer.attach (*_subscriber_poller);
        _application_supply = std::make_unique<application_supply_slot_t> (
          _application_jobs, [this] { _wake_timer.signal (); });
        for (const auto &channel : _channels) {
            if (channel.publisher.enabled
                && channel.publisher.discovery)
                start_publisher (channel, *owner);
            if (channel.subscriber.enabled
                && channel.subscriber.discovery)
                start_subscriber (channel);
        }
        reconcile_subscribers ();
        publish_snapshot_changes ();
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
    auto raw = std::make_shared<raw_fanout_publisher_t> (
      channel.publisher.bind_endpoints.front (), _channel_runtime.core_context ());
    raw->start ();
    std::optional<std::string> advertise_host;
    if (const auto found = _publisher_advertise_hosts.find (channel.name);
        found != _publisher_advertise_hosts.end ()) {
        advertise_host = found->second;
    }
    fanout_publisher_descriptor_t descriptor{
      .channel_name = channel.name,
      .publisher_rid = *channel.publisher.routing_id,
      .lifecycle_generation =
        make_lifecycle_generation (),
      .descriptor_revision = 1,
      .endpoint = transport::advertised_tcp_endpoint (
        raw->endpoint (), advertise_host, "Fanout"),
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
    if (_listener_statuses)
        _listener_statuses->update (
          listener_kind_t::fanout,
          channel.name,
          entry->descriptor.endpoint);
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
      std::make_unique<raw_fanout_subscriber_t> (
        _channel_runtime.core_context (), _subscriber_poller.get ());
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
                publish_snapshot_changes ();
            }
            catch (...) {
                _locations->record_store_error ();
            }
            next_reconcile =
              now + _locations->options ().polling_interval;
        }
        pump ();
        publish_snapshot_changes ();
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
        const auto count = _subscriber_poller->wait (&readiness, 1, timeout);
        if (count == 1 && _wake_timer.is_event (readiness))
            _wake_timer.consume ();
    }
    catch (...) {
    }
}

void fanout_location_runtime_t::publish_descriptors ()
{
    const auto owner = _locations->current_owner_token ();
    if (!owner)
        return;
    struct pending_descriptor_t
    {
        std::string channel_name;
        fanout_publisher_descriptor_t descriptor;
        location_write_intent_t intent;
    };
    std::vector<pending_descriptor_t> pending;
    {
        std::lock_guard lock (_gate);
        const auto state = current_state (*_locations);
        for (const auto &[channel_name, publisher] : _publishers) {
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
            descriptor.lease_generation = owner->lease_generation;
            pending.push_back (
              pending_descriptor_t{
                channel_name,
                std::move (descriptor),
                new_owner ? location_write_intent_t::new_claim
                          : location_write_intent_t::renew});
        }
    }
    for (auto &item : pending) {
        const auto written =
          _store
            ->update_fanout_publisher (item.descriptor, item.intent)
            .result ()
            .value ();
        if (written.status != location_write_status_t::stored)
            continue;
        std::lock_guard lock (_gate);
        const auto found = _publishers.find (item.channel_name);
        if (found == _publishers.end ())
            continue;
        const auto &current = found->second->descriptor;
        if (current.descriptor_revision < item.descriptor.descriptor_revision)
            found->second->descriptor = std::move (item.descriptor);
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
            const auto live = owner_is_live (descriptor);
            desired.push_back (
              fanout_publisher_intent_t{
                descriptor.publisher_rid.to_bytes (),
                descriptor.lifecycle_generation,
                descriptor.endpoint,
                live ? service_state (descriptor.state)
                     : mesh::service_node_state_t::stopped});
        }
        page.continuation_token =
          listed.continuation_token;
    } while (page.continuation_token);
    subscriber.owner->reconcile_automatic (desired);
    {
        std::lock_guard lock (_gate);
        subscriber.desired = std::move (desired);
    }
}

fanout_channel_snapshot_t fanout_location_runtime_t::snapshot (
  std::string channel_name) const
{
    if (!is_observable_channel (channel_name))
        throw framework_exception_t (
          framework_error_kind_t::not_configured,
          "automatic Fanout channel is not configured: " + channel_name);
    std::lock_guard lock (_gate);
    return build_snapshot_locked (channel_name);
}

std::unique_ptr<fanout_runtime_observation_t>
fanout_location_runtime_t::observe (
  std::string channel_name,
  std::size_t capacity,
  std::function<void (
    const observed_status_t<fanout_runtime_event_t> &)> observer)
{
    if (channel_name.empty () || capacity == 0 || !observer)
        throw std::invalid_argument (
          "Fanout observation requires a channel and callback");
    if (!is_observable_channel (channel_name))
        throw framework_exception_t (
          framework_error_kind_t::not_configured,
          "automatic Fanout channel is not configured: " + channel_name);
    auto value = std::make_shared<observer_t> (
      capacity, std::move (observer));
    value->start ();
    {
        std::lock_guard lock (_gate);
        _observers[channel_name].push_back (value);
        const auto current = build_snapshot_locked (channel_name);
        value->enqueue (
          fanout_location_observation_source (channel_name),
          fanout_runtime_event_t{
            fanout_location_changed_event_t{
              current.sequence,
              current.observed_at,
              channel_name,
              current.location}});
    }
    return std::make_unique<fanout_observation_t> (std::move (value));
}

bool fanout_location_runtime_t::is_observable_channel (
  std::string_view channel_name) const noexcept
{
    return std::any_of (
      _channels.begin (), _channels.end (), [channel_name] (const auto &channel) {
          return channel.name == channel_name
                 && ((channel.publisher.enabled
                      && channel.publisher.discovery)
                     || (channel.subscriber.enabled
                         && channel.subscriber.discovery));
      });
}

fanout_channel_snapshot_t fanout_location_runtime_t::build_snapshot_locked (
  const std::string &channel_name) const
{
    fanout_channel_snapshot_t result;
    result.channel_name = channel_name;
    result.observed_at = std::chrono::system_clock::now ();
    if (_locations != nullptr) {
        result.location.store_healthy = !_locations->last_error ().has_value ();
        result.location.last_refresh_at =
          _locations->owner_lease_renewed_at ();
        result.location.owner_lease_healthy =
          _locations->owner_lease_healthy ();
        result.location.owner_lease_renewed_at =
          _locations->owner_lease_renewed_at ();
    }

    const auto subscriber = _subscribers.find (channel_name);
    if (subscriber != _subscribers.end ()) {
        const auto raw = subscriber->second->owner->connection_snapshots ();
        for (const auto &intent : subscriber->second->desired) {
            const auto connected = std::find_if (
              raw.begin (), raw.end (), [&intent] (const auto &entry) {
                  return entry.publisher_routing_id
                           == intent.publisher_routing_id
                         && entry.lifecycle_generation
                              == intent.lifecycle_generation;
              });
            fanout_publisher_connection_snapshot_t entry{
              zlink::routing_id_t::from (intent.publisher_routing_id),
              intent.lifecycle_generation,
              false,
              false,
              fanout_publisher_connection_state_t::excluded_stale,
              std::nullopt};
            if (intent.state == mesh::service_node_state_t::draining) {
                entry.state =
                  fanout_publisher_connection_state_t::excluded_draining;
            } else if (intent.state == mesh::service_node_state_t::serving) {
                if (connected != raw.end ()) {
                    entry.connection_intent = connected->connection_intent;
                    entry.ready = connected->ready;
                    entry.state = connection_state (connected->state);
                    entry.last_failure = connected->last_failure;
                } else {
                    entry.connection_intent = true;
                    entry.state =
                      fanout_publisher_connection_state_t::connecting;
                }
            }
            if (entry.connection_intent)
                ++result.connection_intent_count;
            if (entry.ready)
                ++result.ready_connection_count;
            result.publishers.push_back (std::move (entry));
        }
    }
    const auto sequence = _snapshot_sequences.find (channel_name);
    result.sequence = sequence == _snapshot_sequences.end ()
                        ? 0
                        : sequence->second;
    return result;
}

bool fanout_location_runtime_t::snapshot_equivalent (
  const fanout_channel_snapshot_t &left,
  const fanout_channel_snapshot_t &right) noexcept
{
    return left.channel_name == right.channel_name
           && left.connection_intent_count == right.connection_intent_count
           && left.ready_connection_count == right.ready_connection_count
           && left.publishers == right.publishers
           && left.location == right.location;
}

void fanout_location_runtime_t::publish_snapshot_changes ()
{
    struct pending_observation_t
    {
        std::shared_ptr<observer_t> observer;
        std::string source_key;
        fanout_runtime_event_t event;
        bool terminal = false;
    };
    std::vector<pending_observation_t> notifications;
    {
        std::lock_guard lock (_gate);
        std::set<std::string> channel_names;
        for (const auto &channel : _channels)
            channel_names.insert (channel.name);
        for (const auto &[channel_name, _] : _subscribers)
            channel_names.insert (channel_name);

        for (const auto &channel_name : channel_names) {
            auto current = build_snapshot_locked (channel_name);
            const auto previous = _last_snapshots.find (channel_name);
            if (previous != _last_snapshots.end ()
                && snapshot_equivalent (previous->second, current))
                continue;
            if (previous == _last_snapshots.end ()) {
                _last_snapshots.insert_or_assign (channel_name, current);
                continue;
            }

            const auto sequence = ++_snapshot_sequences[channel_name];
            current.sequence = sequence;
            current.observed_at = std::chrono::system_clock::now ();
            const bool publishers_changed =
              previous->second.publishers != current.publishers;
            const bool location_changed =
              previous->second.location != current.location;
            _last_snapshots.insert_or_assign (channel_name, current);

            auto &registered = _observers[channel_name];
            auto write = registered.begin ();
            for (auto read = registered.begin (); read != registered.end (); ++read) {
                if (auto current_observer = read->lock ()) {
                    if (publishers_changed) {
                        for (const auto &publisher : previous->second.publishers) {
                            const auto still_present = std::any_of (
                              current.publishers.begin (),
                              current.publishers.end (),
                              [&publisher] (const auto &current_publisher) {
                                  return current_publisher.publisher_rid
                                           == publisher.publisher_rid
                                         && current_publisher.lifecycle_generation
                                              == publisher.lifecycle_generation;
                              });
                            if (still_present)
                                continue;
                            auto removed = publisher;
                            removed.connection_intent = false;
                            removed.ready = false;
                            removed.state =
                              fanout_publisher_connection_state_t::disconnected;
                            const auto source_key =
                              fanout_publisher_observation_source (removed);
                            notifications.push_back (
                              pending_observation_t{
                                current_observer,
                                source_key,
                                fanout_runtime_event_t{
                                  fanout_publisher_changed_event_t{
                                    sequence,
                                    current.observed_at,
                                    channel_name,
                                    std::move (removed)}},
                                true});
                        }
                        for (const auto &publisher : current.publishers) {
                            notifications.push_back (
                              pending_observation_t{
                                current_observer,
                                fanout_publisher_observation_source (
                                  publisher),
                                fanout_runtime_event_t{
                                  fanout_publisher_changed_event_t{
                                    sequence,
                                    current.observed_at,
                                    channel_name,
                                    publisher}},
                                false});
                        }
                    }
                    if (location_changed || !publishers_changed) {
                        notifications.push_back (
                          pending_observation_t{
                            current_observer,
                            fanout_location_observation_source (
                              channel_name),
                            fanout_runtime_event_t{
                              fanout_location_changed_event_t{
                                sequence,
                                current.observed_at,
                                channel_name,
                                current.location}},
                            false});
                    }
                    *write++ = *read;
                }
            }
            registered.erase (write, registered.end ());
        }
    }
    for (auto &notification : notifications)
        notification.observer->enqueue (
          std::move (notification.source_key),
          std::move (notification.event),
          notification.terminal);
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
            _application_supply->ensure_waiter ();
            auto reserved = _application_supply->take ();
            if (!reserved)
                break;
            auto application_permit = std::make_shared<
              application_job_queue_t::permit_t> (
                std::move (*reserved));
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
                application_permit->mark_queued ();
                const auto message =
                  zlink::message_t::from (
                    received->payload.payload);
                detail::inbound_message_context_t
                  inbound;
                inbound.before_application_handler =
                  [permit = application_permit] () mutable {
                      if (!permit)
                          return;
                      permit->release_for_handler_entry ();
                      permit.reset ();
                  };
                inbound.message.channel_name =
                  subscriber.channel_name;
                inbound.message.packet_name =
                  received->payload.packet_name;
                inbound.message.content_type =
                  received->payload.content_type;
                inbound.topic = received->topic;
                auto scope = std::make_shared<
                  zlink::framework::detail::service_scope_t> (
                  zlink::framework::detail::service_scope_t::create (
                    _services,
                    zlink::framework::detail::service_scope_kind_t::
                      handler_invocation));
                auto dispatched = _channel_runtime.dispatch_send_async (
                  subscriber.channel_name,
                  received->topic,
                  received->payload.packet_name,
                  scope->provider (), *_serializers, *_handlers,
                  std::move (message), std::move (inbound));
                auto retained = std::move (received->retained);
                auto runtime = _channel_runtime;
                auto packet_name = received->payload.packet_name;
                auto channel_name = subscriber.channel_name;
                auto topic = received->topic;
                detail::observe_task_completion (
                  dispatched,
                  [scope = std::move (scope), retained = std::move (retained),
                   runtime = std::move (runtime),
                   packet_name = std::move (packet_name),
                   channel_name = std::move (channel_name),
                   topic = std::move (topic)] (const result_t<void> &result) {
                      static_cast<void> (scope);
                      static_cast<void> (retained);
                      if (result) {
                          return;
                      }
                      zlink::framework::detail::dispatch_error_reporter_t (
                        runtime.dispatch_options_ref ())
                        .report_lazy ([&] { return message_dispatch_error_event_t{
                          .surface = dispatch_error_surface_t::classic_fanout,
                          .message_kind = dispatch_message_kind_t::send,
                          .reason = zlink::framework::detail::dispatch_reason_from_error (
                            result.error ()),
                          .action = dispatch_error_action_t::drop,
                          .packet_name = packet_name,
                          .channel_name = channel_name,
                          .topic = topic,
                          .exception = result.error ()
                            ? std::make_exception_ptr (*result.error ())
                            : std::exception_ptr{}}; });
                  });
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

task_t<void> fanout_location_runtime_t::publish (
  const std::string &channel_name,
  std::string topic,
  std::string packet_name,
  std::string content_type,
  zlink::message_t message,
  std::chrono::milliseconds timeout)
{
    if (_stop.load (std::memory_order_acquire))
        throw framework_exception_t (
          framework_error_kind_t::shutting_down,
          "fanout runtime is shutting down");
    std::shared_ptr<raw_fanout_publisher_t> publisher;
    {
        std::lock_guard lock (_gate);
        const auto found = _publishers.find (channel_name);
        if (found == _publishers.end ()
            || found->second->descriptor.state
                 != framework_runtime_state_t::serving)
            throw framework_exception_t (
              framework_error_kind_t::unavailable,
              "fanout publisher is not serving");
        publisher = found->second->owner;
    }
    if (_stop.load (std::memory_order_acquire))
        throw framework_exception_t (
          framework_error_kind_t::shutting_down,
          "fanout runtime is shutting down");
    auto encoded = protocol::application_payload_t{
      std::move (packet_name),
      std::move (content_type),
      message.to_bytes ()};
    co_await publisher->publish (channel_name, topic, encoded, timeout);
}

void fanout_location_runtime_t::stop () noexcept
{
    const bool was_stopped =
      _stop.exchange (true, std::memory_order_acq_rel);
    _wake_timer.signal ();
    if (_thread.joinable ())
        _thread.join ();
    if (_application_supply) {
        _application_supply->close ();
        _application_supply.reset ();
    }
    _wake_timer.detach ();
    std::vector<std::string> publisher_channels;
    bool has_publishers = false;
    bool has_subscribers = false;
    {
        std::lock_guard lock (_gate);
        publisher_channels.reserve (_publishers.size ());
        for (const auto &[channel_name, _] : _publishers)
            publisher_channels.push_back (channel_name);
        has_publishers = !_publishers.empty ();
        has_subscribers = !_subscribers.empty ();
    }
    if (!was_stopped || has_publishers || has_subscribers) {
        stop_subscribers ();
        stop_publishers ();
    }
    /* Keep the automatic binding until its owner has rejected and completed
     * every pending retry. Removing it first lets the generic publish path
     * fall through to the manual publisher during shutdown. */
    for (const auto &channel_name : publisher_channels)
        _channel_runtime.unbind_fanout_transport (channel_name);
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
    std::map<std::string, std::unique_ptr<subscriber_entry_t>> subscribers;
    {
        std::lock_guard lock (_gate);
        subscribers.swap (_subscribers);
    }
    for (auto &[_, subscriber] : subscribers)
        subscriber->owner->close ();
}

void fanout_location_runtime_t::stop_publishers () noexcept
{
    std::map<std::string, std::unique_ptr<publisher_entry_t>> publishers;
    {
        std::lock_guard lock (_gate);
        publishers.swap (_publishers);
    }
    for (auto &[_, publisher] : publishers) {
        publisher->owner->close ();
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
        if (_listener_statuses)
            _listener_statuses->remove (
              listener_kind_t::fanout, publisher->descriptor.channel_name);
    }
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
