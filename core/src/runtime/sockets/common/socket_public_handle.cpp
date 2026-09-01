/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "sockets/common/socket_public_handle.hpp"

#include "sockets/common/socket_base.hpp"
#include "utils/err.hpp"

zlink::socket_public_handle_t::socket_public_handle_t (socket_base_t *socket_) :
    _tag (tag_value), _state (0), _socket (socket_)
{
}

bool zlink::socket_public_handle_t::check_tag () const
{
    return _tag == tag_value;
}

bool zlink::socket_public_handle_t::acquire (socket_base_t **socket_out_)
{
    if (!socket_out_) {
        errno = EFAULT;
        return false;
    }

    uint32_t old = _state.load (std::memory_order_acquire);
    while (true) {
        if ((old & (closing_bit | destroy_pending_bit | finalizing_bit)) != 0) {
            errno = ESHUTDOWN;
            return false;
        }
        if ((old & ref_mask) == ref_mask) {
            errno = EBUSY;
            return false;
        }
        if (_state.compare_exchange_weak (old, old + 1,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire))
            break;
    }

    socket_base_t *socket = _socket.load (std::memory_order_acquire);
    if (!socket) {
        release ();
        errno = ESHUTDOWN;
        return false;
    }
    *socket_out_ = socket;
    return true;
}

void zlink::socket_public_handle_t::add_ref ()
{
    const uint32_t old = _state.fetch_add (1, std::memory_order_relaxed);
    zlink_assert ((old & ref_mask) != ref_mask);
}

bool zlink::socket_public_handle_t::try_claim_final_destroy (uint32_t state_)
{
    while ((state_ & ref_mask) == 0
           && (state_ & destroy_pending_bit) != 0
           && (state_ & finalizing_bit) == 0) {
        const uint32_t desired = state_ | finalizing_bit;
        if (_state.compare_exchange_weak (state_, desired,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire))
            return true;
    }
    return false;
}

void zlink::socket_public_handle_t::release ()
{
    const uint32_t old = _state.fetch_sub (1, std::memory_order_acq_rel);
    zlink_assert ((old & ref_mask) > 0);
    const uint32_t current = old - 1;
    if (!try_claim_final_destroy (current))
        return;

    socket_base_t *socket = _socket.load (std::memory_order_acquire);
    zlink_assert (socket);
    const int saved_errno = errno;
    socket->schedule_finalize_destroy ();
    errno = saved_errno;
}

bool zlink::socket_public_handle_t::begin_close ()
{
    // The close caller owns one pin, but a blocking pull may own another.
    // Sealing admission must not wait for that pull to release its handle:
    // close first publishes the lifecycle error that wakes it, and final
    // destruction remains deferred until every pre-existing pin leaves.
    uint32_t old = _state.load (std::memory_order_acquire);
    while (true) {
        if ((old & (closing_bit | destroy_pending_bit | finalizing_bit))
            != 0) {
            errno = ESHUTDOWN;
            return false;
        }
        zlink_assert ((old & ref_mask) != 0);
        if (_state.compare_exchange_weak (old, old | closing_bit,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire))
            return true;
    }
}

void zlink::socket_public_handle_t::cancel_close ()
{
    const uint32_t old = _state.fetch_and (~closing_bit, std::memory_order_release);
    zlink_assert ((old & closing_bit) != 0);
    // Existing calls can still hold pins while close admission is being
    // attempted. They cannot acquire new pins after closing_bit is visible.
    zlink_assert ((old & ref_mask) != 0);
    zlink_assert ((old & (destroy_pending_bit | finalizing_bit)) == 0);
}

bool zlink::socket_public_handle_t::request_destroy ()
{
    uint32_t old = _state.load (std::memory_order_acquire);
    while (true) {
        const uint32_t desired = old | closing_bit | destroy_pending_bit;
        if (_state.compare_exchange_weak (old, desired,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
            old = desired;
            break;
        }
    }
    return try_claim_final_destroy (old);
}

void zlink::socket_public_handle_t::clear_socket ()
{
    _socket.store (NULL, std::memory_order_release);
}
