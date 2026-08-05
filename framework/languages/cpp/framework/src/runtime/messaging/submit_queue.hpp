/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/messaging/pending_submit.hpp"

#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

namespace zlink::framework::runtime::messaging
{

class submit_queue_t
{
  public:
    explicit submit_queue_t (std::size_t capacity);

    std::size_t capacity () const noexcept { return _capacity; }
    std::size_t pending_count () const;

    void enqueue (pending_submit_t pending);
    bool try_peek (pending_submit_t &pending) const;
    bool try_dequeue_expected (const pending_operation_t &expected, pending_submit_t &pending);
    std::vector<pending_submit_t> dispose_all ();

  private:
    void throw_if_disposed () const;

    mutable std::mutex _gate;
    std::deque<pending_submit_t> _pending;
    std::size_t _capacity;
    bool _disposed = false;
};

} // namespace zlink::framework::runtime::messaging
