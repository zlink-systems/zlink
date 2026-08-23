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

    // A request that already reached its terminal leaves no pending record to
    // cancel, so its ticket is empty.
    bool valid () const noexcept { return _id != 0; }

  private:
    std::weak_ptr<routed_admission_state_t> _owner;
    uint64_t _id = 0;
};

// One routed admission request.
//
// The admission state parks whole requests, not per-message callables: the
// caller already owns a heap record for the send it is submitting, so making
// that record the request removes the per-message std::function copies the
// admission bookkeeping used to allocate.
class routed_admission_request_t
{
  public:
    virtual ~routed_admission_request_t () = default;

    // Non-blocking Core submit. Never called with the admission lock held.
    virtual routed_attempt_result_t attempt () = 0;
    virtual void accepted () = 0;
    virtual void terminal (submit_result_t result_, int error_) = 0;

    // The instant this request stops waiting for send credit, or
    // time_point::max() for a request that waits indefinitely. A zero
    // configured timeout returns the instant the request started, which
    // expires it as soon as its one attempt has been made. Consulted only
    // when the request has to wait, so a send that Core accepts on its first
    // attempt never pays for resolving its timeout.
    virtual std::chrono::steady_clock::time_point deadline () = 0;
};

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
  std::shared_ptr<routed_admission_request_t> request_);

} // namespace detail
} // namespace zlink

#endif
