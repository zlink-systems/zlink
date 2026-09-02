/* SPDX-License-Identifier: MPL-2.0 */

#include <zlink/Contracts/Sockets/stream_socket.hpp>
#include <Runtime/Core/routing_id_access.hpp>
#include <Runtime/Native/message_access.hpp>
#include <Runtime/Sockets/detail.hpp>
#include <Runtime/Sockets/socket_access.hpp>
#include <Runtime/Messaging/operation_state.hpp>
#include <zlink/Contracts/Messaging/operation_contracts.hpp>

#include <stdexcept>

namespace zlink
{

stream_socket_t::stream_socket_t (context_t &ctx_) :
    routed_message_socket_t (ctx_, socket_type::stream)
{
}

send_operation_t stream_socket_t::send (const routing_id_t &target_rid_)
{
    auto state_ptr = detail::acquire_state ();
    state_ptr->kind = detail::operation_kind_t::raw_routed_send;
    state_ptr->raw.socket = detail::native_handle (*this);
    detail::bind_runtime_state (state_ptr->raw, detail::runtime_state (*this));
    state_ptr->raw.target.first_rid = target_rid_;
    return send_operation_t (std::move (state_ptr));
}

int stream_socket_t::recv (received_t &out_, recv_flags_t flags_)
{
    return socket_t::receive (out_, flags_);
}

stream_packet_t::~stream_packet_t ()
{
    close ();
}

stream_packet_t::stream_packet_t (stream_packet_t &&other_) noexcept
{
    if (!other_._receiving.load (std::memory_order_acquire)) {
        _routing_id = std::move (other_._routing_id);
        _header = std::move (other_._header);
        _body = std::move (other_._body);
    }
}

stream_packet_t &stream_packet_t::operator= (stream_packet_t &&other_) noexcept
{
    if (this == &other_)
        return *this;
    close ();
    if (!other_._receiving.load (std::memory_order_acquire)) {
        _routing_id = std::move (other_._routing_id);
        _header = std::move (other_._header);
        _body = std::move (other_._body);
    }
    return *this;
}

bool stream_packet_t::empty () const noexcept
{
    return !_routing_id && !_header && !_body;
}

const std::optional<routing_id_t> &stream_packet_t::routing_id () const noexcept
{
    return _routing_id;
}

message_t &stream_packet_t::header ()
{
    if (!_header)
        throw std::logic_error ("stream packet has no header");
    return *_header;
}

message_t &stream_packet_t::body ()
{
    if (!_body)
        throw std::logic_error ("stream packet has no body");
    return *_body;
}

void stream_packet_t::close () noexcept
{
    _routing_id.reset ();
    _header.reset ();
    _body.reset ();
}

bool stream_socket_t::recv_packet (stream_packet_t &out_, recv_flags_t flags_)
{
    bool expected = false;
    if (!out_._receiving.compare_exchange_strong (
          expected, true, std::memory_order_acq_rel, std::memory_order_acquire))
        throw recv_error_t (recv_result_t::invalid_state, EBUSY);

    struct receive_claim_t
    {
        std::atomic<bool> &claim;
        ~receive_claim_t () { claim.store (false, std::memory_order_release); }
    } claim{out_._receiving};

    out_.close ();
    message_t header;
    message_t body;
    const zlink_routing_id_t *source_rid = nullptr;
    const recv_result_t result = static_cast<recv_result_t> (zlink_stream_recv_packet (
      detail::native_handle (*this), &source_rid, detail::native_handle (header),
      detail::native_handle (body),
      static_cast<zlink_recv_flags_t> (static_cast<int> (flags_))));
    if (result == recv_result_t::no_data)
        return false;
    if (result != recv_result_t::ok)
        throw recv_error_t (result, zlink_errno ());
    if (!source_rid || source_rid->size == 0)
        throw recv_error_t (recv_result_t::internal_error, EPROTO);

    detail::refresh_payload_presence (header);
    detail::refresh_payload_presence (body);
    out_._routing_id.emplace (detail::native_routing_id (*source_rid));
    out_._header.emplace (std::move (header));
    out_._body.emplace (std::move (body));
    return true;
}

void stream_socket_t::set_routing_id (const routing_id_t &routing_id_)
{
    detail::set_routing_id_or_throw (detail::native_handle (*this), routing_id_);
}

void stream_socket_t::get_routing_id (routing_id_t &routing_id_) const
{
    detail::get_routing_id_or_throw (const_cast<void *> (detail::native_handle (*this)),
                                     routing_id_);
}

} // namespace zlink
