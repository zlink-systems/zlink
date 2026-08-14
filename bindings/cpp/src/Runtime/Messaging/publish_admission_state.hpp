/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_MESSAGING_PUBLISH_ADMISSION_STATE_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_MESSAGING_PUBLISH_ADMISSION_STATE_HPP_INCLUDED

#include <zlink/Contracts/Sockets/results.hpp>

#include <chrono>
#include <functional>
#include <memory>

namespace zlink::detail
{
struct socket_callback_state_t;

struct publish_attempt_result_t
{
    submit_result_t result = submit_result_t::internal_error;
    int error = 0;
};

class publish_admission_state_t;

class publish_admission_ticket_t
{
  public:
    publish_admission_ticket_t () = default;
    publish_admission_ticket_t (std::weak_ptr<publish_admission_state_t> owner,
                                uint64_t id) noexcept :
        _owner (std::move (owner)), _id (id)
    {
    }
    bool cancel () const noexcept;

  private:
    std::weak_ptr<publish_admission_state_t> _owner;
    uint64_t _id = 0;
};

using publish_attempt_fn_t = std::function<publish_attempt_result_t ()>;
using publish_accepted_fn_t = std::function<void ()>;
using publish_terminal_fn_t = std::function<void (submit_result_t, int)>;

std::shared_ptr<publish_admission_state_t>
ensure_publish_admission_state (socket_callback_state_t &callbacks);
void shutdown_publish_admission_state (socket_callback_state_t &callbacks) noexcept;
void notify_publish_admission_ready (socket_callback_state_t &callbacks) noexcept;

publish_admission_ticket_t enqueue_publish_admission (
  const std::shared_ptr<publish_admission_state_t> &owner,
  publish_attempt_fn_t attempt, publish_accepted_fn_t accepted,
  publish_terminal_fn_t terminal, std::chrono::steady_clock::time_point deadline);
} // namespace zlink::detail
#endif
