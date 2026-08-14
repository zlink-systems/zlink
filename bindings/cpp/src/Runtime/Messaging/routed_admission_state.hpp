/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_MESSAGING_ROUTED_ADMISSION_STATE_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_MESSAGING_ROUTED_ADMISSION_STATE_HPP_INCLUDED

#include <zlink/Contracts/Sockets/results.hpp>
#include <zlink/socket/api.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

namespace zlink
{
namespace detail
{

struct socket_callback_state_t;

struct routed_attempt_result_t
{
    submit_result_t result = submit_result_t::internal_error;
    int error = 0;
};

class routed_admission_state_t;

class routed_admission_ticket_t
{
  public:
    routed_admission_ticket_t () = default;
    routed_admission_ticket_t (std::weak_ptr<routed_admission_state_t> owner_,
                               uint64_t id_) noexcept :
        _owner (std::move (owner_)), _id (id_)
    {
    }

    bool cancel () const noexcept;

  private:
    std::weak_ptr<routed_admission_state_t> _owner;
    uint64_t _id = 0;
};

using routed_attempt_fn_t = std::function<routed_attempt_result_t ()>;
using routed_accepted_fn_t = std::function<void ()>;
using routed_terminal_fn_t = std::function<void (submit_result_t, int)>;

std::shared_ptr<routed_admission_state_t>
ensure_routed_admission_state (void *socket_, socket_callback_state_t &callbacks_);

void shutdown_routed_admission_state (socket_callback_state_t &callbacks_) noexcept;
void notify_routed_admission_ready (socket_callback_state_t &callbacks_) noexcept;

void post_routed_completion (std::function<void ()> completion_);

zlink_routed_submit_target_t
select_routed_submit_target (void *socket_, const zlink_routing_id_t *router_rid_or_null_);

routed_admission_ticket_t enqueue_routed_admission (
  const std::shared_ptr<routed_admission_state_t> &owner_,
  const zlink_routed_submit_target_t &target_,
  routed_attempt_fn_t attempt_,
  routed_accepted_fn_t accepted_,
  routed_terminal_fn_t terminal_,
  std::chrono::steady_clock::time_point deadline_ = std::chrono::steady_clock::time_point::max (),
  bool attempt_before_expiry_ = false);

} // namespace detail
} // namespace zlink

#endif
