/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/fanout/raw_fanout_owner.hpp"
#include "runtime/backend/raw_binding_adapter.hpp"

#include <zlink/Contracts/Core/context.hpp>
#include <zlink/Contracts/Eventing/poll_event.hpp>
#include <zlink/Contracts/Eventing/poller.hpp>
#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/Contracts/Messaging/operation_contracts.hpp>
#include <zlink/Contracts/Messaging/topic_message.hpp>
#include <zlink/Contracts/Sockets/message_socket_contracts.hpp>
#include <zlink/Contracts/Sockets/pubsub_socket_contracts.hpp>
#include <zlink/Contracts/Sockets/results.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <set>
#include <stdexcept>
#include <utility>

namespace zlink::framework::runtime::fanout
{
namespace
{

std::atomic<std::uintptr_t> next_fanout_poller_slot{1};

} // namespace

raw_fanout_publisher_t::raw_fanout_publisher_t (
  std::string endpoint,
  std::shared_ptr<zlink::context_t> context) :
    _configured_endpoint (std::move (endpoint)),
    _context (
      context ? std::move (context) : std::make_shared<zlink::context_t> ())
{
    if (_configured_endpoint.empty ()) {
        throw std::invalid_argument ("fanout publisher endpoint is required");
    }
}

raw_fanout_publisher_t::~raw_fanout_publisher_t () noexcept
{
    close ();
}

void raw_fanout_publisher_t::start ()
{
    std::lock_guard lock (_mutex);
    if (_socket) {
        return;
    }
    if (_closed) {
        throw std::logic_error ("fanout publisher cannot restart after close");
    }
    auto socket = std::make_unique<zlink::pub_socket_t> (*_context);
    socket->options ().linger (std::chrono::milliseconds (0));
    socket->bind (_configured_endpoint);
    _endpoint = socket->options ().last_endpoint ();
    _next_beacon =
      std::chrono::steady_clock::now () + fanout_beacon_interval;
    _socket = std::move (socket);
}

void raw_fanout_publisher_t::close () noexcept
{
    std::unique_ptr<zlink::pub_socket_t> socket;
    {
        std::lock_guard lock (_mutex);
        _closed = true;
        socket = std::move (_socket);
    }
    if (socket) {
        try {
            socket->close ();
        }
        catch (...) {
        }
    }
}

std::string raw_fanout_publisher_t::endpoint () const
{
    std::lock_guard lock (_mutex);
    return _endpoint;
}

std::chrono::steady_clock::time_point
raw_fanout_publisher_t::next_activity () const
{
    std::lock_guard lock (_mutex);
    return _socket ? _next_beacon
                   : std::chrono::steady_clock::now () + fanout_beacon_interval;
}

task_t<void> raw_fanout_publisher_t::publish (
  std::string topic,
  protocol::application_payload_t payload,
  std::chrono::milliseconds timeout)
{
    if (topic.empty () || topic == reserved_topic ()) {
        throw std::invalid_argument (
          "fanout application topic is empty or reserved");
    }
    auto encoded = protocol::encode_application_payload (payload);
    auto message = zlink::message_t::from (std::move (encoded));
    std::optional<zlink::async_result_t<void>> submitted;
    {
        std::lock_guard lock (_mutex);
        if (!_socket) {
            throw framework_exception_t (
              framework_error_kind_t::unavailable,
              "fanout publisher is stopped");
        }
        auto operation =
          std::move (_socket->publish (topic)).message (std::move (message));
        submitted.emplace (
          timeout.count () > 0
            ? std::move (operation).timeout (timeout).async ()
            : std::move (operation).async ());
    }
    co_await std::move (*submitted);
}

bool raw_fanout_publisher_t::tick (
  std::chrono::steady_clock::time_point now)
{
    {
        std::lock_guard lock (_mutex);
        if (!_socket || now < _next_beacon) {
            return false;
        }
    }
    auto message =
      zlink::message_t::from (std::vector<std::uint8_t> (beacon_payload ()));
    std::lock_guard lock (_mutex);
    if (!_socket || now < _next_beacon) {
        return false;
    }
    const auto submitted = std::move (
      _socket->publish (reserved_topic ())).message (std::move (message))
      .flags (static_cast<int> (zlink::send_flags_t::dontwait)).submit ();
    do {
        _next_beacon += fanout_beacon_interval;
    } while (_next_beacon <= now);
    return submitted;
}

const std::string &raw_fanout_publisher_t::reserved_topic ()
{
    static const std::string value{
      static_cast<char> (0x01), static_cast<char> (0x5a),
      static_cast<char> (0x4c), static_cast<char> (0x46),
      static_cast<char> (0x31)};
    return value;
}

const std::vector<std::uint8_t> &
raw_fanout_publisher_t::beacon_payload ()
{
    static const std::vector<std::uint8_t> value{0x5a, 0x46, 0x01, 0x01};
    return value;
}

raw_fanout_subscriber_t::raw_fanout_subscriber_t (zlink::poller_t *poller) :
    raw_fanout_subscriber_t (std::make_shared<zlink::context_t> (), poller)
{
}

raw_fanout_subscriber_t::raw_fanout_subscriber_t (
  std::shared_ptr<zlink::context_t> context,
  zlink::poller_t *poller) :
    _context (
      context ? std::move (context) : std::make_shared<zlink::context_t> ()),
    _owned_poller (poller == nullptr
                     ? std::make_unique<zlink::poller_t> ()
                     : nullptr),
    _poller (poller != nullptr ? poller : _owned_poller.get ())
{
}

raw_fanout_subscriber_t::~raw_fanout_subscriber_t () noexcept
{
    close ();
}

bool raw_fanout_subscriber_t::connect_manual (
  std::vector<std::uint8_t> publisher_routing_id,
  std::string endpoint)
{
    std::lock_guard lock (_mutex);
    return connect_locked (
      std::move (publisher_routing_id), 0, std::move (endpoint), false);
}

void raw_fanout_subscriber_t::reconcile_automatic (
  const std::vector<fanout_publisher_intent_t> &publishers)
{
    std::lock_guard lock (_mutex);
    if (_automatic_mode && !*_automatic_mode) {
        throw std::logic_error (
          "manual and automatic fanout subscriber modes cannot be combined");
    }
    _automatic_mode = true;
    std::set<publisher_intent_key_t> desired;
    for (const auto &publisher : publishers) {
        if (publisher.state
            != mesh::service_node_state_t::serving) {
            continue;
        }
        const publisher_intent_key_t intent{
          publisher.publisher_routing_id,
          publisher.lifecycle_generation};
        desired.insert (intent);
        const auto found = _connections.find (intent);
        if (found == _connections.end ()) {
            (void) connect_locked (
              publisher.publisher_routing_id,
              publisher.lifecycle_generation,
              publisher.endpoint,
              true);
        } else if (found->second.automatic
                   && found->second.endpoint
                        != publisher.endpoint) {
            found->second.endpoint = publisher.endpoint;
            reopen_locked (found->second);
        }
    }
    for (auto entry = _connections.begin (); entry != _connections.end ();) {
        if (entry->second.automatic
            && !desired.contains (entry->first)) {
            close_connection_locked (entry->second);
            entry = _connections.erase (entry);
        } else {
            ++entry;
        }
    }
}

bool raw_fanout_subscriber_t::disconnect (
    const std::vector<std::uint8_t> &publisher_routing_id)
{
    std::lock_guard lock (_mutex);
    const auto found = std::find_if (
      _connections.begin (),
      _connections.end (),
      [&publisher_routing_id] (const auto &entry) {
          return entry.first.routing_id == publisher_routing_id;
      });
    if (found == _connections.end ()) {
        return false;
    }
    close_connection_locked (found->second);
    _connections.erase (found);
    return true;
}

void raw_fanout_subscriber_t::close () noexcept
{
    {
        std::lock_guard lock (_mutex);
        if (_closed) {
            return;
        }
        _closed = true;
        for (auto &[id, connection] : _connections) {
            static_cast<void> (id);
            close_connection_locked (connection);
        }
        _connections.clear ();
        if (_owned_poller) {
            try {
                _owned_poller->close ();
            }
            catch (...) {
            }
        }
    }
}

std::pair<fanout_receive_status_t, std::optional<fanout_received_t>>
raw_fanout_subscriber_t::try_receive (
  std::chrono::steady_clock::time_point now)
{
    std::lock_guard lock (_mutex);
    if (_connections.empty ())
        return {fanout_receive_status_t::no_data, std::nullopt};

    const auto count = _connections.size ();
    const auto start = _receive_cursor % count;
    auto current = _connections.begin ();
    for (std::size_t index = 0; index < start; ++index)
        ++current;
    for (std::size_t checked = 0; checked < count; ++checked) {
        auto selected = current;
        ++current;
        if (current == _connections.end ())
            current = _connections.begin ();
        _receive_cursor = (start + checked + 1) % count;
        auto &intent = selected->first;
        auto &connection = selected->second;
        if (!connection.socket)
            continue;
        zlink::poll_event_t readiness;
        try {
            if (_poller->wait (
                  &readiness, 1, std::chrono::milliseconds::zero ())
                  != 1
                || readiness.slot != connection.poller_slot
                || (static_cast<short> (readiness.revents)
                    & static_cast<short> (zlink::poll_event_flag_t::pollin))
                     == 0) {
                continue;
            }
        }
        catch (...) {
            return {fanout_receive_status_t::no_data, std::nullopt};
        }
        const auto result =
          connection.socket->subscribe (_received, zlink::recv_flags_t::dontwait);
        if (result == static_cast<int> (zlink::recv_result_t::no_data)
            || (result == -1
                && (errno == EAGAIN || errno == EWOULDBLOCK))) {
            continue;
        }
        if (result != 0) {
            reopen_locked (connection);
            return {fanout_receive_status_t::protocol_error, std::nullopt};
        }
        detail::backend::binding_received_release_t received_release (
          _received);
        const auto &parts = _received.parts ();
        if (_received.topic () == raw_fanout_publisher_t::reserved_topic ()) {
            const auto payload = parts.empty ()
                                   ? std::span<const std::byte>{}
                                   : parts.front ().bytes ();
            const auto &expected = raw_fanout_publisher_t::beacon_payload ();
            if (parts.size () != 1
                || payload.size () != expected.size ()
                || !std::equal (payload.begin (), payload.end (), expected.begin (),
                                [] (std::byte left, std::uint8_t right) {
                                    return static_cast<std::uint8_t> (left) == right;
                })) {
                reopen_locked (connection);
                return {fanout_receive_status_t::protocol_error, std::nullopt};
            }
            connection.ready = true;
            connection.reconnecting = false;
            connection.deadline = now + fanout_receive_deadline;
            return {fanout_receive_status_t::beacon, std::nullopt};
        }
        if (parts.size () != 1) {
            reopen_locked (connection);
            return {fanout_receive_status_t::protocol_error, std::nullopt};
        }
        try {
            const auto bytes = parts.front ().bytes ();
            /* The classic-fanout consumer never reads the flow pair —
             * structural skip keeps Off nodes from failing on it. */
            auto payload = protocol::decode_application_payload (
              std::span<const std::uint8_t> (
                reinterpret_cast<const std::uint8_t *> (bytes.data ()), bytes.size ()),
              false);
            connection.ready = true;
            connection.reconnecting = false;
            connection.deadline = now + fanout_receive_deadline;
            auto topic = _received.topic ();
            auto retained = std::make_shared<zlink::topic_message_t> (_received);
            return {
              fanout_receive_status_t::application,
              fanout_received_t{intent.routing_id, std::move (topic),
                                std::move (payload), std::move (retained)}};
        }
        catch (const protocol::service_wire_error_t &) {
            reopen_locked (connection);
            return {fanout_receive_status_t::protocol_error, std::nullopt};
        }
    }
    return {fanout_receive_status_t::no_data, std::nullopt};
}

std::vector<std::vector<std::uint8_t>> raw_fanout_subscriber_t::tick (
  std::chrono::steady_clock::time_point now)
{
    std::lock_guard lock (_mutex);
    std::vector<std::vector<std::uint8_t>> timed_out;
    for (auto &[intent, connection] : _connections) {
        if (connection.ready && now >= connection.deadline) {
            timed_out.push_back (intent.routing_id);
            reopen_locked (connection);
        }
    }
    return timed_out;
}

bool raw_fanout_subscriber_t::ready (
  const std::vector<std::uint8_t> &publisher_routing_id) const
{
    std::lock_guard lock (_mutex);
    return std::any_of (
      _connections.begin (),
      _connections.end (),
      [&publisher_routing_id] (const auto &entry) {
          return entry.first.routing_id == publisher_routing_id
                 && entry.second.ready;
      });
}

std::size_t raw_fanout_subscriber_t::publisher_count () const
{
    std::lock_guard lock (_mutex);
    return _connections.size ();
}

std::vector<raw_fanout_connection_snapshot_t>
raw_fanout_subscriber_t::connection_snapshots () const
{
    std::lock_guard lock (_mutex);
    std::vector<raw_fanout_connection_snapshot_t> snapshots;
    snapshots.reserve (_connections.size ());
    for (const auto &[intent, connection] : _connections) {
        auto state = raw_fanout_connection_state_t::disconnected;
        if (connection.ready)
            state = raw_fanout_connection_state_t::ready;
        else if (connection.reconnecting)
            state = raw_fanout_connection_state_t::reconnecting;
        else if (connection.socket)
            state = raw_fanout_connection_state_t::connecting;
        snapshots.push_back (raw_fanout_connection_snapshot_t{
          intent.routing_id,
          intent.lifecycle_generation,
          true,
          connection.ready,
          state,
          std::nullopt});
    }
    return snapshots;
}

bool raw_fanout_subscriber_t::connect_locked (
  std::vector<std::uint8_t> publisher_routing_id,
  std::uint64_t lifecycle_generation,
  std::string endpoint,
  bool automatic)
{
    if (_closed || !_context || publisher_routing_id.empty ()
        || endpoint.empty ()) {
        return false;
    }
    if (_automatic_mode && *_automatic_mode != automatic) {
        return false;
    }
    _automatic_mode = automatic;
    publisher_intent_key_t intent{
      std::move (publisher_routing_id), lifecycle_generation};
    if (_connections.contains (intent)) {
        return false;
    }
    connection_t connection;
    connection.endpoint = std::move (endpoint);
    connection.poller_slot =
      next_fanout_poller_slot.fetch_add (1, std::memory_order_relaxed);
    if (connection.poller_slot == 0)
        connection.poller_slot =
          next_fanout_poller_slot.fetch_add (1, std::memory_order_relaxed);
    connection.automatic = automatic;
    const auto [inserted, was_inserted] =
      _connections.emplace (std::move (intent), std::move (connection));
    if (!was_inserted) {
        return false;
    }
    try {
        reopen_locked (inserted->second);
        inserted->second.reconnecting = false;
    }
    catch (...) {
        close_connection_locked (inserted->second);
        _connections.erase (inserted);
        return false;
    }
    return true;
}

void raw_fanout_subscriber_t::close_connection_locked (connection_t &connection) noexcept
{
    if (!connection.socket) {
        return;
    }
    try {
        _poller->remove (*connection.socket);
    }
    catch (...) {
    }
    try {
        connection.socket->close ();
    }
    catch (...) {
    }
    connection.socket.reset ();
}

void raw_fanout_subscriber_t::reopen_locked (connection_t &connection)
{
    close_connection_locked (connection);
    auto socket = std::make_unique<zlink::sub_socket_t> (*_context);
    socket->options ().linger (std::chrono::milliseconds (0));
    socket->set_subscription ("");
    socket->connect (connection.endpoint);
    _poller->add (*socket, zlink::poll_event_flag_t::pollin,
                  connection.poller_slot);
    connection.socket = std::move (socket);
    connection.ready = false;
    connection.reconnecting = true;
    connection.deadline = {};
}

} // namespace zlink::framework::runtime::fanout
