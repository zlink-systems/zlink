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
#include <Runtime/Sockets/socket_callback_state.hpp>
#include <Runtime/Messaging/received_access.hpp>
#include <Runtime/Native/subscription_reader.hpp>

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

routing_id_t routing_id_from_native_pointer (const void *native_) noexcept
{
    const zlink_routing_id_t *rid = static_cast<const zlink_routing_id_t *> (native_);
    return (rid && rid->size > 0) ? native_routing_id (*rid) : unchecked_empty_routing_id ();
}

} // namespace detail

socket_t::~socket_t ()
{
}

socket_t::socket_t (socket_t &&) noexcept = default;

socket_t &socket_t::operator= (socket_t &&) noexcept = default;

bool socket_t::valid () const noexcept
{
    return _socket && _socket->valid ();
}

void socket_t::close ()
{
    const int rc = _socket ? _socket->close () : 0;
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

void socket_t::set_tls_server (const std::string &cert_,
                               const std::string &key_,
                               bool require_client_cert_)
{
    const int rc = zlink_set_tls_server (detail::native_handle (*this), cert_.c_str (),
                                         key_.c_str (), require_client_cert_ ? 1 : 0);
    if (rc != 0)
        throw config_error_t (detail::config_result_from_errno (zlink_errno ()), zlink_errno ());
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
        throw config_error_t (detail::config_result_from_errno (zlink_errno ()), zlink_errno ());
}

socket_t::socket_t () noexcept :
    _socket (std::make_unique<detail::socket_handle_t> ()),
    _callbacks (std::make_unique<detail::socket_callback_state_t> ()),
    _type (socket_type::pair)
{
}

socket_t::socket_t (context_t &ctx_, socket_type type_) :
    _socket (std::make_unique<detail::socket_handle_t> (
      zlink_socket (detail::native_handle (ctx_), static_cast<zlink_socket_type_t> (type_)), true)),
    _callbacks (std::make_unique<detail::socket_callback_state_t> ()),
    _type (type_)
{
}

int socket_t::send (message_t &part_, send_flags_t flags_)
{
    return detail::submit_one_message_part (
      part_, [&] (zlink_msg_t *part_out_, zlink_part_flag_t part_flag_) {
          return zlink_send_part (detail::native_handle (*this), part_out_,
                                  static_cast<zlink_send_flags_t> (static_cast<int> (flags_)),
                                  part_flag_);
      });
}

int socket_t::send (std::vector<message_t> &parts_, send_flags_t flags_)
{
    return detail::submit_message_parts (
      parts_, [&] (zlink_msg_t *part_out_, zlink_part_flag_t part_flag_, bool) {
          return zlink_send_part (detail::native_handle (*this), part_out_,
                                  static_cast<zlink_send_flags_t> (static_cast<int> (flags_)),
                                  part_flag_);
      });
}

int socket_t::send (const routing_id_t &target_rid_, message_t &part_, send_flags_t flags_)
{
    const zlink_routing_id_t target_rid = zlink::detail::routing_id_native_value (target_rid_);
    return detail::submit_one_message_part (
      part_, [&] (zlink_msg_t *part_out_, zlink_part_flag_t part_flag_) {
          return zlink_send_part_rid (
            detail::native_handle (*this), &target_rid, part_out_,
            static_cast<zlink_send_flags_t> (static_cast<int> (flags_)), part_flag_);
      });
}

int socket_t::send (const routing_id_t &target_rid_,
                    std::vector<message_t> &parts_,
                    send_flags_t flags_)
{
    const zlink_routing_id_t target_rid = zlink::detail::routing_id_native_value (target_rid_);
    return detail::submit_message_parts (
      parts_, [&] (zlink_msg_t *part_out_, zlink_part_flag_t part_flag_, bool) {
          return zlink_send_part_rid (
            detail::native_handle (*this), &target_rid, part_out_,
            static_cast<zlink_send_flags_t> (static_cast<int> (flags_)), part_flag_);
      });
}

int socket_t::receive (received_t &received_, recv_flags_t flags_)
{
    return receive (received_, flags_, true);
}

int socket_t::receive (received_t &received_, recv_flags_t flags_, bool attach_routed_send_context_)
{
    detail::recv_envelope_t envelope;
    const bool use_router_recv = _type == socket_type::router;
    const int rc =
      detail::recv_envelope (detail::native_handle (*this), flags_, envelope, use_router_recv);
    if (rc != 0)
        return rc;

    const std::optional<routing_id_t> source_rid =
      zlink::detail::routing_id_empty (envelope.source_rid)
        ? std::nullopt
        : std::optional<routing_id_t> (envelope.source_rid);
    const std::optional<uint64_t> request_seq =
      envelope.has_request_seq ? std::optional<uint64_t> (envelope.request_seq) : std::nullopt;

    if (envelope.single_part.has_value ()) {
        received_ = detail::received_access_t::make (source_rid, request_seq,
                                                     std::move (*envelope.single_part));
    } else if (envelope.parts.size () == 1u) {
        received_ = detail::received_access_t::make (source_rid, request_seq,
                                                     std::move (envelope.parts[0]));
    } else {
        received_ = detail::received_access_t::make (source_rid, request_seq,
                                                     std::move (envelope.parts));
    }
    if (attach_routed_send_context_ && source_rid.has_value ())
        detail::received_access_t::set_socket_rid_send_context (received_,
                                                                detail::native_handle (*this));
    return 0;
}

int socket_t::publish (const std::string &topic_id_, message_t &part_, send_flags_t flags_)
{
    detail::validate_no_embedded_null (topic_id_, "topic");
    return detail::submit_one_message_part (
      part_, [&] (zlink_msg_t *part_out_, zlink_part_flag_t part_flag_) {
          return zlink_publish_part (detail::native_handle (*this), topic_id_.c_str (), part_out_,
                                     static_cast<zlink_send_flags_t> (static_cast<int> (flags_)),
                                     part_flag_);
      });
}

int socket_t::publish (const std::string &topic_id_,
                       std::vector<message_t> &parts_,
                       send_flags_t flags_)
{
    detail::validate_no_embedded_null (topic_id_, "topic");
    return detail::submit_message_parts (
      parts_, [&] (zlink_msg_t *part_out_, zlink_part_flag_t part_flag_, bool) {
          return zlink_publish_part (detail::native_handle (*this), topic_id_.c_str (), part_out_,
                                     static_cast<zlink_send_flags_t> (static_cast<int> (flags_)),
                                     part_flag_);
      });
}

int socket_t::subscribe (topic_message_t &message_, recv_flags_t flags_)
{
    return detail::read_subscription_message (
      message_,
      [&] (const zlink_routing_id_t **source_rid_out_, char *topic_out_, size_t topic_capacity_,
           size_t *topic_size_out_, zlink_msg_t *part_out_, zlink_part_flag_t *has_more_out_) {
          return static_cast<int> (
            zlink_subscribe_part (detail::native_handle (*this), source_rid_out_, topic_out_,
                                  topic_capacity_, topic_size_out_, part_out_, has_more_out_,
                                  static_cast<zlink_recv_flags_t> (static_cast<int> (flags_))));
      });
}

int socket_t::subscribe_part (std::optional<routing_id_t> &source_rid_out_,
                              std::string &topic_out_,
                              message_t &part_out_,
                              bool &has_more_out_,
                              recv_flags_t flags_)
{
    char topic_buffer[256];
    size_t topic_size = sizeof (topic_buffer);
    const zlink_routing_id_t *source_rid = nullptr;
    detail::scoped_native_message_t native_part;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    if (!native_part.init ())
        return -1;

    const int rc = zlink_subscribe_part (
      detail::native_handle (*this), &source_rid, topic_buffer, sizeof (topic_buffer), &topic_size,
      native_part.get (), &has_more, static_cast<zlink_recv_flags_t> (static_cast<int> (flags_)));
    if (rc != ZLINK_RECV_OK)
        return static_cast<int> (rc);

    detail::assign_subscription_part (&source_rid_out_, topic_out_, part_out_, has_more_out_,
                                      source_rid, topic_buffer, topic_size, sizeof (topic_buffer),
                                      native_part.get (), has_more);
    return 0;
}

int socket_t::subscription_event (subscription_event_t &event_, recv_flags_t flags_)
{
    detail::scoped_native_message_t part;
    if (!part.init ())
        return -1;

    const zlink_routing_id_t *source_rid = nullptr;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    const int rc =
      zlink_recv_part (detail::native_handle (*this), &source_rid, part.get (), &has_more,
                       static_cast<zlink_recv_flags_t> (static_cast<int> (flags_)));
    if (rc != 0)
        return rc;

    const unsigned char *data = static_cast<const unsigned char *> (zlink_msg_data (part.get ()));
    const size_t size = zlink_msg_size (part.get ());
    if (has_more) {
        errno = EMSGSIZE;
        return -1;
    }

    event_.routing_id = std::nullopt;
    event_.topic.clear ();
    event_.subscribed = size > 0 && data[0] != 0;
    const routing_id_t source = detail::routing_id_or_empty (source_rid);
    if (!zlink::detail::routing_id_empty (source))
        event_.routing_id = source;
    event_.topic.assign (size > 1 ? reinterpret_cast<const char *> (data + 1) : "",
                         size > 0 ? size - 1 : 0);
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

void socket_t::set_send_ready_handler (std::function<void ()> handler_)
{
    detail::socket_callback_state_t &state = callback_state ();
    state.send_ready_handler = std::move (handler_);
    auto trampoline = [] (void *, void *userdata_) {
        auto *callback_state = static_cast<detail::socket_callback_state_t *> (userdata_);
        if (callback_state && callback_state->send_ready_handler)
            callback_state->send_ready_handler ();
    };
    if (zlink_send_ready_handler (detail::native_handle (*this),
                                  static_cast<zlink_send_ready_handler_fn> (+trampoline), &state)
        != 0)
        detail::throw_if_failed<handler_error_t> (
          static_cast<handler_result_t> (detail::handler_result_from_errno (zlink_errno ())));
}

detail::socket_callback_state_t &socket_t::callback_state ()
{
    if (!_callbacks)
        _callbacks = std::make_unique<detail::socket_callback_state_t> ();
    return *_callbacks;
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
