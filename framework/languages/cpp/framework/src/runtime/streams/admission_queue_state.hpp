/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "runtime/streams/admission_queue_messages.hpp"

#include <chrono>
#include <deque>
#include <list>
#include <map>
#include <memory>
#include <set>

namespace zlink::framework::runtime::admission_queue
{

struct queue_time_t
{
    std::chrono::steady_clock::time_point steady;
    std::uint64_t unix_ms;

    static queue_time_t now ();
};

class queue_state_t
{
  public:
    queue_state_t (std::shared_ptr<const ticket_signer_t> tickets,
                   std::optional<std::int32_t> global_limit,
                   std::uint64_t epoch = random_nonzero_u64 ());

    enqueue_reply_t enqueue (connection_t connection, queue_time_t now);
    std::set<owner_t> accept_report (const node_report_t &report, queue_time_t now);
    report_reply_t
    finish_report (const node_t &node, queue_time_t now, const std::set<owner_t> &expired = {});
    std::uint64_t admit (const admit_request_t &request, queue_time_t now);
    void release (const release_t &release);
    std::set<owner_t> permit_owners_at_limit (const admit_request_t &request,
                                              queue_time_t now) const;
    void owner_lease_ended (const owner_t &owner);

    static std::int32_t estimate_wait_ms (std::int32_t position, std::size_t issued);

  private:
    friend struct queue_fixture_t;
    using steady_time_t = std::chrono::steady_clock::time_point;

    enum class ticket_delivery_t
    {
        pending,
        delivered,
        used
    };

    struct waiter_t
    {
        connection_t connection;
        std::optional<issued_ticket_t> ticket;
        std::uint64_t expiration{};
        std::optional<position_t> last_position;
        ticket_delivery_t ticket_delivery = ticket_delivery_t::pending;
        steady_time_t last_push;

        bool is_queued () const noexcept { return ticket_delivery != ticket_delivery_t::used; }
    };

    struct report_state_t
    {
        steady_time_t received;
        std::int32_t budget;
    };

    struct permit_t
    {
        node_t node;
        owner_t owner;
        std::uint64_t request_seq;
    };

    void expire (queue_time_t now);
    bool issue (queue_time_t now);
    std::set<owner_t> permit_owners () const;
    std::size_t global_budget () const;
    bool permit_limit_reached () const;
    static bool valid_ticket (const waiter_t &waiter, std::uint64_t now) noexcept;
    position_t position_of (std::size_t position) const;
    delivery_t deliver (waiter_t &waiter, position_t position, steady_time_t now);

    std::shared_ptr<const ticket_signer_t> _tickets;
    std::optional<std::int32_t> _global_limit;
    std::uint64_t _epoch;
    std::list<waiter_t> _waiters;
    std::map<node_t, report_state_t> _reports;
    std::map<std::uint64_t, permit_t> _permits;
    std::deque<steady_time_t> _issued_at;
};

} // namespace zlink::framework::runtime::admission_queue
