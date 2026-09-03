/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "sockets/common/socket_base.hpp"

std::shared_ptr<zlink::socket_reqrep_internal::socket_request_reply_state_t>
zlink::socket_base_t::request_reply_state () const
{
    if (!_request_reply_bridge.request_reply_state_present.load (
          std::memory_order_acquire))
        return std::shared_ptr<
          socket_reqrep_internal::socket_request_reply_state_t> ();

    // The owner is installed once and remains immutable until socket
    // destruction.  Cleanup only withdraws the published bit, so hot-path
    // readers can copy the per-socket owner without the process-wide
    // shared_ptr atomic lock.
    return _request_reply_bridge.request_reply_state;
}

bool zlink::socket_base_t::has_request_reply_state () const
{
    return _request_reply_bridge.request_reply_state_present.load (
      std::memory_order_acquire);
}

std::shared_ptr<zlink::socket_reqrep_internal::socket_request_reply_state_t>
zlink::socket_base_t::set_request_reply_state (
  const std::shared_ptr<zlink::socket_reqrep_internal::socket_request_reply_state_t> &state_)
{
    std::shared_ptr<socket_reqrep_internal::socket_request_reply_state_t> expected;
    if (std::atomic_compare_exchange_strong_explicit (
          &_request_reply_bridge.request_reply_state, &expected, state_,
          std::memory_order_acq_rel, std::memory_order_acquire)) {
        _request_reply_bridge.request_reply_state_present.store (
          true, std::memory_order_release);
        return state_;
    }
    _request_reply_bridge.request_reply_state_present.store (
      true, std::memory_order_release);
    return expected;
}

void zlink::socket_base_t::clear_request_reply_state ()
{
    _request_reply_bridge.request_reply_state_present.store (
      false, std::memory_order_release);
}

std::shared_ptr<zlink::part_helper_internal::handle_state_t>
zlink::socket_base_t::part_helper_state () const
{
    if (!_request_reply_bridge.part_helper_state_present.load (
          std::memory_order_acquire))
        return std::shared_ptr<part_helper_internal::handle_state_t> ();

    // Like request/reply state, this owner is a one-time per-socket cache.
    // Keeping it immutable until socket destruction removes the shared_ptr
    // atomic lock from every helper part.
    return _request_reply_bridge.part_helper_state;
}

bool zlink::socket_base_t::has_part_helper_state () const
{
    return _request_reply_bridge.part_helper_state_present.load (
      std::memory_order_acquire);
}

std::shared_ptr<zlink::part_helper_internal::handle_state_t>
zlink::socket_base_t::set_part_helper_state (
  const std::shared_ptr<zlink::part_helper_internal::handle_state_t> &state_)
{
    std::shared_ptr<part_helper_internal::handle_state_t> expected;
    if (std::atomic_compare_exchange_strong_explicit (
          &_request_reply_bridge.part_helper_state, &expected, state_, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
        _request_reply_bridge.part_helper_state_present.store (
          true, std::memory_order_release);
        return state_;
    }
    _request_reply_bridge.part_helper_state_present.store (
      true, std::memory_order_release);
    return expected;
}

void zlink::socket_base_t::clear_part_helper_state ()
{
    _request_reply_bridge.part_helper_state_present.store (
      false, std::memory_order_release);
}

bool zlink::socket_base_t::part_helper_send_active () const
{
    return _request_reply_bridge.part_helper_send_active_flag.load (
      std::memory_order_acquire);
}

void zlink::socket_base_t::set_part_helper_send_active (bool active_)
{
    _request_reply_bridge.part_helper_send_active_flag.store (
      active_, std::memory_order_release);
}
