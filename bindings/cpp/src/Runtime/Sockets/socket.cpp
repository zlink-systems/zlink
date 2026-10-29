/* SPDX-License-Identifier: MPL-2.0 */

#include <zlink/Contracts/Sockets/socket_contracts.hpp>

#include <Runtime/Native/native_message_guard.hpp>
#include <Runtime/Native/native_message_parts.hpp>
#include <Runtime/Native/native_options.hpp>
#include <Runtime/Native/native_receive.hpp>
#include <Runtime/Native/native_send.hpp>
#include <Runtime/Native/socket_handle.hpp>
#include <Runtime/Core/context_access.hpp>
#include <Runtime/Options/option_ids.hpp>
#include <Runtime/Sockets/socket_access.hpp>
#include <Runtime/Sockets/socket_runtime_state.hpp>
#include <Runtime/Messaging/received_access.hpp>
#include <Runtime/Native/subscription_reader.hpp>

#include <zlink.h>

#include <cerrno>
#include <stdexcept>

namespace zlink
{

namespace detail
{

void *socket_access_t::native_handle (socket_t &socket_) noexcept
{
    return socket_._socket ? detail::native_handle (*socket_._socket) : nullptr;
}

const void *socket_access_t::native_handle (const socket_t &socket_) noexcept
{
    return socket_._socket ? detail::native_handle (*socket_._socket) : nullptr;
}

std::shared_ptr<socket_runtime_state_t> socket_access_t::runtime_state (
  socket_t &socket_) noexcept
{
    return socket_._runtime;
}

routing_id_t routing_id_from_native_pointer (const void *native_) noexcept
{
    const zlink_routing_id_t *rid = static_cast<const zlink_routing_id_t *> (native_);
    return (rid && rid->size > 0) ? native_routing_id (*rid) : unchecked_empty_routing_id ();
}

} // namespace detail

socket_t::~socket_t ()
{
    if (_socket)
        (void) _socket->close ();
    if (_runtime)
        _runtime->completion->shutdown ();
}

socket_t::socket_t (socket_t &&) noexcept = default;

socket_t &socket_t::operator= (socket_t &&other_) noexcept
{
    if (this == &other_)
        return *this;
    if (_socket) {
        const int rc = _socket->close ();
        if (rc != 0 && _socket->valid ())
            return *this;
    }
    if (_runtime)
        _runtime->completion->shutdown ();
    _socket = std::move (other_._socket);
    _runtime = std::move (other_._runtime);
    _receive_envelope = std::move (other_._receive_envelope);
    _type = other_._type;
    return *this;
}

bool socket_t::valid () const noexcept
{
    return _socket && _socket->valid ();
}

void socket_t::close ()
{
    if (!_socket || !_socket->valid ())
        return;
    const int rc = _socket->close ();
    // Core can reject a concurrent close with EBUSY while the socket remains
    // live. Preserve the completion owner and all retained retry packets in
    // that case; only a terminal native close may settle them.
    if (!_socket->valid () && _runtime)
        _runtime->completion->shutdown (ESHUTDOWN);
    if (rc != 0)
        throw close_error_t (static_cast<close_result_t> (rc), zlink_errno ());
}

void socket_t::bind (const std::string &endpoint_)
{
    const int rc = zlink_bind (detail::native_handle (*this), endpoint_.c_str ());
    if (rc != 0)
        throw bind_error_t (static_cast<bind_result_t> (rc), zlink_errno ());
}

void socket_t::connect (const std::string &endpoint_)
{
    const int rc = zlink_connect (detail::native_handle (*this), endpoint_.c_str ());
    if (rc != 0)
        throw connect_error_t (static_cast<connect_result_t> (rc), zlink_errno ());
}

void socket_t::unbind (const std::string &endpoint_)
{
    const int rc = zlink_unbind (detail::native_handle (*this), endpoint_.c_str ());
    if (rc != 0)
        throw connect_error_t (static_cast<connect_result_t> (rc), zlink_errno ());
}

void socket_t::disconnect (const std::string &endpoint_)
{
    const int rc = zlink_disconnect (detail::native_handle (*this), endpoint_.c_str ());
    if (rc != 0)
        throw connect_error_t (static_cast<connect_result_t> (rc), zlink_errno ());
}

void socket_t::disconnect_rid (const routing_id_t &peer_rid_)
{
    const zlink_routing_id_t native = *zlink::detail::routing_id_native (peer_rid_);
    const int rc = zlink_disconnect_rid (detail::native_handle (*this), &native);
    if (rc != 0)
        throw connect_error_t (static_cast<connect_result_t> (rc), zlink_errno ());
}

namespace
{
[[noreturn]] void connection_snapshot_protocol_violation ()
{
    throw std::runtime_error ("connection snapshot protocol violation");
}

connection_attempt_state_t connection_snapshot_connection_attempt_state (uint32_t value_)
{
    switch (value_) {
        case ZLINK_CONNECTION_CONNECTING:
            return connection_attempt_state_t::connecting;
        case ZLINK_CONNECTION_CONNECTED:
            return connection_attempt_state_t::connected;
        case ZLINK_CONNECTION_ATTEMPT_FAILED:
            return connection_attempt_state_t::attempt_failed;
        default:
            connection_snapshot_protocol_violation ();
    }
}

connection_failure_kind_t connection_snapshot_connection_failure_kind (uint32_t value_)
{
    switch (value_) {
        case ZLINK_CONNECTION_FAILURE_NONE:
            return connection_failure_kind_t::none;
        case ZLINK_CONNECTION_FAILURE_REFUSED:
            return connection_failure_kind_t::refused;
        case ZLINK_CONNECTION_FAILURE_ENDPOINT_MISSING:
            return connection_failure_kind_t::endpoint_missing;
        case ZLINK_CONNECTION_FAILURE_TIMED_OUT:
            return connection_failure_kind_t::timed_out;
        case ZLINK_CONNECTION_FAILURE_UNREACHABLE:
            return connection_failure_kind_t::unreachable;
        case ZLINK_CONNECTION_FAILURE_NAME_RESOLUTION:
            return connection_failure_kind_t::name_resolution;
        case ZLINK_CONNECTION_FAILURE_AUTH_FAILED:
            return connection_failure_kind_t::auth_failed;
        case ZLINK_CONNECTION_FAILURE_PROTOCOL_FAILED:
            return connection_failure_kind_t::protocol_failed;
        case ZLINK_CONNECTION_FAILURE_CONNECTION_LOST:
            return connection_failure_kind_t::connection_lost;
        case ZLINK_CONNECTION_FAILURE_OTHER:
            return connection_failure_kind_t::other;
        default:
            connection_snapshot_protocol_violation ();
    }
}

connection_failure_stage_t connection_snapshot_connection_failure_stage (uint32_t value_)
{
    switch (value_) {
        case ZLINK_CONNECTION_STAGE_NONE:
            return connection_failure_stage_t::none;
        case ZLINK_CONNECTION_STAGE_RESOLVE:
            return connection_failure_stage_t::resolve;
        case ZLINK_CONNECTION_STAGE_TRANSPORT:
            return connection_failure_stage_t::transport;
        case ZLINK_CONNECTION_STAGE_TLS:
            return connection_failure_stage_t::tls;
        case ZLINK_CONNECTION_STAGE_WEBSOCKET:
            return connection_failure_stage_t::websocket;
        case ZLINK_CONNECTION_STAGE_CORE_HANDSHAKE:
            return connection_failure_stage_t::core_handshake;
        case ZLINK_CONNECTION_STAGE_ESTABLISHED:
            return connection_failure_stage_t::established;
        default:
            connection_snapshot_protocol_violation ();
    }
}

} // namespace

std::vector<connection_attempt_t> socket_t::connection_attempts_snapshot ()
{
    std::vector<zlink_connection_attempt_t> native;
    std::vector<char> endpoints;
    for (;;) {
        size_t count = 0;
        size_t bytes = 0;
        const auto rc = zlink_socket_connection_attempts_snapshot (
          detail::native_handle (*this), native.empty () ? nullptr : native.data (),
          native.size (), &count, endpoints.empty () ? nullptr : endpoints.data (),
          endpoints.size (), &bytes);
        const int native_errno = zlink_errno ();
        if (rc == ZLINK_CONFIG_BUFFER_TOO_SMALL) {
            if (count <= native.size () && bytes <= endpoints.size ())
                connection_snapshot_protocol_violation ();
            if (count > native.size ())
                native.resize (count);
            if (bytes > endpoints.size ())
                endpoints.resize (bytes);
            continue;
        }
        if (rc != ZLINK_CONFIG_OK)
            throw config_error_t (static_cast<config_result_t> (rc), native_errno);
        if (count > native.size () || bytes > endpoints.size ())
            connection_snapshot_protocol_violation ();

        std::vector<connection_attempt_t> attempts;
        attempts.reserve (count);
        for (size_t index = 0; index < count; ++index) {
            const auto &row = native[index];
            if (row.intent_id == 0 || row.revision == 0
                || row.endpoint_offset >= bytes
                || row.endpoint_length >= bytes - row.endpoint_offset
                || endpoints[row.endpoint_offset + row.endpoint_length] != '\0')
                connection_snapshot_protocol_violation ();
            attempts.push_back (connection_attempt_t{
              std::string (endpoints.data () + row.endpoint_offset, row.endpoint_length),
              row.intent_id, row.revision, row.last_failure_id,
              connection_snapshot_connection_attempt_state (row.state),
              connection_failure_t{
                connection_snapshot_connection_failure_kind (row.last_failure.kind),
                connection_snapshot_connection_failure_stage (row.last_failure.stage),
                row.last_failure.normalized_errno, row.last_failure.disconnect_reason}});
        }
        return attempts;
    }
}

void socket_t::set_tls_server (const std::string &cert_,
                               const std::string &key_,
                               bool require_client_cert_)
{
    const int rc = zlink_set_tls_server (detail::native_handle (*this), cert_.c_str (),
                                         key_.c_str (), require_client_cert_ ? 1 : 0);
    if (rc != 0)
        throw config_error_t (static_cast<config_result_t> (rc), zlink_errno ());
}

void socket_t::set_tls_client (const std::string &ca_cert_,
                               const std::string &hostname_,
                               bool trust_system_)
{
    const char *ca = ca_cert_.empty () ? nullptr : ca_cert_.c_str ();
    const char *hostname = hostname_.empty () ? nullptr : hostname_.c_str ();
    const int rc =
      zlink_set_tls_client (detail::native_handle (*this), ca, hostname, trust_system_ ? 1 : 0);
    if (rc != 0)
        throw config_error_t (static_cast<config_result_t> (rc), zlink_errno ());
}

void socket_t::set_receive_flow_state (receive_flow_state_t state_)
{
    detail::throw_if_failed<config_error_t> (static_cast<config_result_t> (
      zlink_socket_set_receive_flow_state (detail::native_handle (*this),
                                           static_cast<zlink_receive_flow_state_t> (state_))));
}

socket_t::socket_t () noexcept :
    _socket (std::make_unique<detail::socket_handle_t> ()),
    _runtime (),
    _type (socket_type::pair)
{
}

socket_t::socket_t (context_t &ctx_, socket_type type_) :
    _socket (std::make_unique<detail::socket_handle_t> (
      zlink_socket (detail::native_handle (ctx_), static_cast<zlink_socket_type_t> (type_)), true)),
    _runtime (),
    _type (type_)
{
    if (_socket->valid ())
        _runtime = std::make_shared<detail::socket_runtime_state_t> (
          detail::native_handle (*_socket));
}

int socket_t::send (message_t &part_, send_flags_t flags_)
{
    return detail::submit_borrowed_message_part (
      part_, [&] (zlink_msg_t *parts_, size_t part_count_) {
          return zlink_send (detail::native_handle (*this), parts_, part_count_,
                             static_cast<zlink_send_flags_t> (static_cast<int> (flags_)),
                             nullptr, nullptr);
      });
}

int socket_t::send (std::vector<message_t> &parts_, send_flags_t flags_)
{
    return detail::submit_message_parts (
      parts_, [&] (zlink_msg_t *native_parts_, size_t part_count_) {
          return zlink_send (detail::native_handle (*this), native_parts_, part_count_,
                             static_cast<zlink_send_flags_t> (static_cast<int> (flags_)),
                             nullptr, nullptr);
      });
}

int socket_t::send (const routing_id_t &target_rid_, message_t &part_, send_flags_t flags_)
{
    const zlink_routing_id_t target_rid = zlink::detail::routing_id_native_value (target_rid_);
    return detail::submit_borrowed_message_part (
      part_, [&] (zlink_msg_t *parts_, size_t part_count_) {
          return zlink_send_rid (
            detail::native_handle (*this), &target_rid, parts_, part_count_,
            static_cast<zlink_send_flags_t> (static_cast<int> (flags_)), nullptr, nullptr);
      });
}

int socket_t::send (const routing_id_t &target_rid_,
                    std::vector<message_t> &parts_,
                    send_flags_t flags_)
{
    const zlink_routing_id_t target_rid = zlink::detail::routing_id_native_value (target_rid_);
    return detail::submit_message_parts (
      parts_, [&] (zlink_msg_t *native_parts_, size_t part_count_) {
          return zlink_send_rid (
            detail::native_handle (*this), &target_rid, native_parts_, part_count_,
            static_cast<zlink_send_flags_t> (static_cast<int> (flags_)), nullptr, nullptr);
      });
}

int socket_t::receive (received_t &received_, recv_flags_t flags_)
{
    return receive_impl (received_, flags_, true);
}

int socket_t::receive (received_t &received_, recv_flags_t flags_, bool attach_routed_send_context_)
{
    return receive_impl (received_, flags_, attach_routed_send_context_);
}

int socket_t::receive_impl (
  received_t &received_, recv_flags_t flags_, bool attach_routed_send_context_)
{
    if (!_receive_envelope)
        _receive_envelope = std::make_unique<detail::recv_envelope_t> ();
    auto &envelope = *_receive_envelope;
    // The staging envelope carries only metadata. Physical parts are received
    // directly into the caller-provided result so its vector capacity is the
    // single reusable collection for this receive path.
    envelope.bind (detail::received_access_t::prepare_receive (received_));
    const bool use_router_recv = _type == socket_type::router;
    int rc = -1;
    try {
        rc = detail::recv_whole_envelope (
          detail::native_handle (*this), flags_, envelope, use_router_recv);
    }
    catch (...) {
        // A failed binding-side allocation must not leave earlier multipart
        // frame leases parked in the reusable envelope.
        envelope.reset ();
        throw;
    }
    if (rc != 0) {
        // A multipart receive can fail after earlier physical parts were
        // accepted.  The reusable staging envelope must not retain their Core
        // credit until a later receive or socket destruction.
        envelope.reset ();
        return rc;
    }

    // Core reports the route generation of the record returned by the last
    // successful zlink_router_recv; read it before any other data receive.
    const uint64_t route_generation =
      use_router_recv ? zlink_router_recv_route_generation (detail::native_handle (*this)) : 0;
    detail::received_access_t::commit_receive_metadata (
      received_, std::move (envelope.source_rid), envelope.has_reply_token,
      envelope.reply_token, _runtime ? _runtime->reply_owner : nullptr, route_generation);
    if (attach_routed_send_context_ && received_.routing_id ().has_value ())
        detail::received_access_t::set_socket_rid_send_context (received_,
                                                                detail::native_handle (*this),
                                                                _runtime);
    return 0;
}

int socket_t::publish (const std::string &topic_id_, message_t &part_, send_flags_t flags_)
{
    detail::validate_no_embedded_null (topic_id_, "topic");
    return detail::submit_borrowed_message_part (
      part_, [&] (zlink_msg_t *parts_, size_t part_count_) {
          return zlink_publish (detail::native_handle (*this), topic_id_.c_str (), parts_,
                                part_count_, static_cast<zlink_send_flags_t> (
                                               static_cast<int> (flags_)));
      });
}

int socket_t::publish (const std::string &topic_id_,
                       std::vector<message_t> &parts_,
                       send_flags_t flags_)
{
    detail::validate_no_embedded_null (topic_id_, "topic");
    return detail::submit_message_parts (
      parts_, [&] (zlink_msg_t *native_parts_, size_t part_count_) {
          return zlink_publish (detail::native_handle (*this), topic_id_.c_str (), native_parts_,
                                part_count_, static_cast<zlink_send_flags_t> (
                                               static_cast<int> (flags_)));
      });
}

int socket_t::subscribe (topic_message_t &message_, recv_flags_t flags_)
{
    message_.close ();
    if (_receive_envelope && _receive_envelope->has_subscription_part ()) {
        _receive_envelope->take_subscription_message (message_);
        return ZLINK_RECV_OK;
    }
    return detail::read_subscription_message (
      message_,
      [&] (const zlink_routing_id_t **source_rid_out_, char *topic_out_, size_t topic_capacity_,
           size_t *topic_size_out_, zlink_msg_t *parts_out_, size_t parts_capacity_,
           size_t *part_count_out_) {
           return static_cast<int> (zlink_subscribe (
             detail::native_handle (*this), source_rid_out_, topic_out_,
             topic_capacity_, topic_size_out_, parts_out_, parts_capacity_, part_count_out_,
             static_cast<zlink_recv_flags_t> (static_cast<int> (flags_))));
       });
}

int socket_t::subscribe_part (std::optional<routing_id_t> &source_rid_out_,
                              std::string &topic_out_,
                              message_t &part_out_,
                              bool &has_more_out_,
                              recv_flags_t flags_)
{
    if (!_receive_envelope)
        _receive_envelope = std::make_unique<detail::recv_envelope_t> ();
    if (!_receive_envelope->has_subscription_part ()) {
        topic_message_t message;
        const int rc = subscribe (message, flags_);
        if (rc != ZLINK_RECV_OK)
            return rc;
        _receive_envelope->stage_subscription (message);
    }

    _receive_envelope->take_subscription_part (
      source_rid_out_, topic_out_, part_out_, has_more_out_);
    return 0;
}

int socket_t::subscription_event (subscription_event_t &event_, recv_flags_t flags_)
{
    const zlink_routing_id_t *source_rid = nullptr;
    zlink_msg_t native_part;
    size_t part_count = 0;
    const int rc = zlink_recv (
      detail::native_handle (*this), &source_rid, &native_part, 1u, &part_count,
      static_cast<zlink_recv_flags_t> (static_cast<int> (flags_)));
    if (rc != ZLINK_RECV_OK)
        return rc;
    if (part_count != 1u) {
        if (part_count > 0)
            detail::close_message_array (&native_part, 1u);
        errno = EPROTO;
        return -1;
    }

    const unsigned char *data =
      static_cast<const unsigned char *> (zlink_msg_data (&native_part));
    const size_t size = zlink_msg_size (&native_part);

    event_.routing_id = std::nullopt;
    event_.topic.clear ();
    event_.subscribed = size > 0 && data[0] != 0;
    const routing_id_t source = detail::routing_id_or_empty (source_rid);
    if (!zlink::detail::routing_id_empty (source))
        event_.routing_id = source;
    event_.topic.assign (size > 1 ? reinterpret_cast<const char *> (data + 1) : "",
                         size > 0 ? size - 1 : 0);
    detail::close_message_array (&native_part, 1u);
    return 0;
}

int socket_t::set_subscription (const std::string &filter_)
{
    detail::validate_no_embedded_null (filter_, "filter");
    return zlink_set_subscription (detail::native_handle (*this), filter_.c_str ());
}

int socket_t::unset_subscription (const std::string &filter_)
{
    detail::validate_no_embedded_null (filter_, "filter");
    return zlink_unset_subscription (detail::native_handle (*this), filter_.c_str ());
}

int socket_t::subscription_at (size_t index_, std::string &filter_, bool *is_pattern_)
{
    int pattern = 0;
    const int rc = detail::read_growing_string (
      [&] (char *buffer_, size_t, size_t *size_out_) {
          return zlink_subscription_at (detail::native_handle (*this), index_, buffer_, size_out_,
                                        &pattern);
      },
      256u, filter_);
    if (rc != 0)
        return rc;
    if (is_pattern_)
        *is_pattern_ = pattern != 0;
    return 0;
}

detail::socket_runtime_state_t &socket_t::runtime_state ()
{
    if (!_runtime)
        throw config_error_t (config_result_t::invalid_handle, EINVAL);
    return *_runtime;
}

int socket_t::set_routing_id_raw (std::span<const std::byte> data_)
{
    return zlink_set_routing_id (detail::native_handle (*this), data_.data (), data_.size ());
}

int socket_t::get_routing_id_raw (routing_id_t &routing_id_) const
{
    zlink_routing_id_t native;
    std::memset (&native, 0, sizeof (native));
    const int rc =
      zlink_get_routing_id (const_cast<void *> (detail::native_handle (*this)), &native);
    if (rc == 0)
        zlink::detail::assign_routing_id_native (routing_id_, native);
    return rc;
}

} // namespace zlink
