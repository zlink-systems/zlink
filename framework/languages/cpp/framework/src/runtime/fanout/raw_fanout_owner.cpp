/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/fanout/raw_fanout_owner.hpp"

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

std::vector<zlink::message_t> materialize (
  const std::vector<std::vector<std::uint8_t>> &parts)
{
    std::vector<zlink::message_t> result;
    result.reserve (parts.size ());
    for (const auto &part : parts) {
        result.push_back (zlink::message_t::from (part));
    }
    return result;
}

bool submit_publish (zlink::pub_socket_t &socket,
                     const std::string &topic,
                     const std::vector<std::vector<std::uint8_t>> &parts)
{
    auto messages = materialize (parts);
    auto operation = std::move (socket.publish (topic)).message (messages[0]);
    for (std::size_t index = 1; index < messages.size (); ++index) {
        operation = std::move (operation).message (messages[index]);
    }
    return std::move (operation).submit ();
}

} // namespace

raw_fanout_publisher_t::raw_fanout_publisher_t (std::string endpoint) :
    _configured_endpoint (std::move (endpoint))
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
    auto context = std::make_unique<zlink::context_t> ();
    auto socket = std::make_unique<zlink::pub_socket_t> (*context);
    socket->options ().linger (std::chrono::milliseconds (0));
    socket->bind (_configured_endpoint);
    _endpoint = socket->options ().last_endpoint ();
    _next_beacon =
      std::chrono::steady_clock::now () + fanout_beacon_interval;
    _socket = std::move (socket);
    _context = std::move (context);
}

void raw_fanout_publisher_t::close () noexcept
{
    std::unique_ptr<zlink::pub_socket_t> socket;
    std::unique_ptr<zlink::context_t> context;
    {
        std::lock_guard lock (_mutex);
        _closed = true;
        socket = std::move (_socket);
        context = std::move (_context);
    }
    if (socket) {
        try {
            socket->close ();
        }
        catch (...) {
        }
    }
    if (context) {
        try {
            context->shutdown ();
            context->term ();
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

bool raw_fanout_publisher_t::publish (
  const std::string &topic,
  const protocol::application_payload_t &payload)
{
    if (topic.empty () || topic == reserved_topic ()) {
        throw std::invalid_argument (
          "fanout application topic is empty or reserved");
    }
    std::lock_guard lock (_mutex);
    return _socket
           && submit_publish (
             *_socket, topic, {protocol::encode_application_payload (payload)});
}

bool raw_fanout_publisher_t::tick (
  std::chrono::steady_clock::time_point now)
{
    std::lock_guard lock (_mutex);
    if (!_socket || now < _next_beacon) {
        return false;
    }
    const auto submitted =
      submit_publish (*_socket, reserved_topic (), {beacon_payload ()});
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
    _context (std::make_unique<zlink::context_t> ()),
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
    std::unique_ptr<zlink::context_t> context;
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
        context = std::move (_context);
    }
    if (context) {
        try {
            context->shutdown ();
            context->term ();
        }
        catch (...) {
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
        zlink::topic_message_t message;
        const auto result =
          connection.socket->subscribe (message, zlink::recv_flags_t::dontwait);
        if (result == static_cast<int> (zlink::recv_result_t::no_data)
            || (result == -1
                && (errno == EAGAIN || errno == EWOULDBLOCK))) {
            continue;
        }
        if (result != 0) {
            reopen_locked (connection);
            return {fanout_receive_status_t::protocol_error, std::nullopt};
        }
        const auto &parts = message.parts ();
        if (message.topic () == raw_fanout_publisher_t::reserved_topic ()) {
            if (parts.size () != 1
                || parts.front ().to_bytes ()
                     != raw_fanout_publisher_t::beacon_payload ()) {
                reopen_locked (connection);
                return {fanout_receive_status_t::protocol_error, std::nullopt};
            }
            connection.ready = true;
            connection.deadline = now + fanout_receive_deadline;
            return {fanout_receive_status_t::beacon, std::nullopt};
        }
        if (parts.size () != 1) {
            reopen_locked (connection);
            return {fanout_receive_status_t::protocol_error, std::nullopt};
        }
        try {
            auto payload =
              protocol::decode_application_payload (parts.front ().to_bytes ());
            connection.ready = true;
            connection.deadline = now + fanout_receive_deadline;
            return {
              fanout_receive_status_t::application,
              fanout_received_t{
                intent.routing_id, message.topic (), std::move (payload)}};
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
    connection.deadline = {};
}

} // namespace zlink::framework::runtime::fanout
