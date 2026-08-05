/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "sockets/common/socket_base.hpp"

std::shared_ptr<zlink::socket_reqrep_internal::socket_request_reply_state_t>
zlink::socket_base_t::request_reply_state () const
{
    return std::atomic_load_explicit (&_request_reply_bridge.request_reply_state,
                                      std::memory_order_acquire);
}

std::shared_ptr<zlink::socket_reqrep_internal::socket_request_reply_state_t>
zlink::socket_base_t::set_request_reply_state (
  const std::shared_ptr<zlink::socket_reqrep_internal::socket_request_reply_state_t> &state_)
{
    std::shared_ptr<socket_reqrep_internal::socket_request_reply_state_t> expected;
    if (std::atomic_compare_exchange_strong_explicit (
          &_request_reply_bridge.request_reply_state, &expected, state_,
          std::memory_order_acq_rel, std::memory_order_acquire))
        return state_;
    return expected;
}

void zlink::socket_base_t::clear_request_reply_state ()
{
    std::atomic_store_explicit (
      &_request_reply_bridge.request_reply_state,
      std::shared_ptr<socket_reqrep_internal::socket_request_reply_state_t> (),
      std::memory_order_release);
}

std::shared_ptr<zlink::part_helper_internal::handle_state_t>
zlink::socket_base_t::part_helper_state () const
{
    return std::atomic_load_explicit (&_request_reply_bridge.part_helper_state,
                                      std::memory_order_acquire);
}

std::shared_ptr<zlink::part_helper_internal::handle_state_t>
zlink::socket_base_t::set_part_helper_state (
  const std::shared_ptr<zlink::part_helper_internal::handle_state_t> &state_)
{
    std::shared_ptr<part_helper_internal::handle_state_t> expected;
    if (std::atomic_compare_exchange_strong_explicit (
          &_request_reply_bridge.part_helper_state, &expected, state_, std::memory_order_acq_rel,
          std::memory_order_acquire))
        return state_;
    return expected;
}

void zlink::socket_base_t::clear_part_helper_state ()
{
    std::atomic_store_explicit (
      &_request_reply_bridge.part_helper_state,
      std::shared_ptr<part_helper_internal::handle_state_t> (), std::memory_order_release);
}
