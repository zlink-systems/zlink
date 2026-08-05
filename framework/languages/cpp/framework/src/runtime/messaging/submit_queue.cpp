/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/messaging/submit_queue.hpp"

#include <stdexcept>

namespace zlink::framework::runtime::messaging
{

submit_queue_t::submit_queue_t (std::size_t capacity) : _capacity (capacity)
{
    if (capacity == 0) {
        throw std::invalid_argument ("submit queue capacity must be positive");
    }
}

std::size_t submit_queue_t::pending_count () const
{
    std::lock_guard<std::mutex> lock (_gate);
    return _pending.size ();
}

void submit_queue_t::enqueue (pending_submit_t pending)
{
    std::lock_guard<std::mutex> lock (_gate);
    throw_if_disposed ();
    if (_pending.size () >= _capacity) {
        throw std::runtime_error ("ZLink async submit queue is full.");
    }
    _pending.push_back (std::move (pending));
}

bool submit_queue_t::try_peek (pending_submit_t &pending) const
{
    std::lock_guard<std::mutex> lock (_gate);
    if (_pending.empty ()) {
        return false;
    }
    pending = _pending.front ();
    return true;
}

bool submit_queue_t::try_dequeue_expected (const pending_operation_t &expected,
                                           pending_submit_t &pending)
{
    std::lock_guard<std::mutex> lock (_gate);
    if (_pending.empty () || !_pending.front ().operation ().valid ()
        || !detail::same_pending_operation (_pending.front ().operation (), expected)) {
        return false;
    }
    pending = std::move (_pending.front ());
    _pending.pop_front ();
    return true;
}

std::vector<pending_submit_t> submit_queue_t::dispose_all ()
{
    std::lock_guard<std::mutex> lock (_gate);
    if (_disposed) {
        return {};
    }
    _disposed = true;
    std::vector<pending_submit_t> remaining;
    remaining.reserve (_pending.size ());
    while (!_pending.empty ()) {
        remaining.push_back (std::move (_pending.front ()));
        _pending.pop_front ();
    }
    return remaining;
}

void submit_queue_t::throw_if_disposed () const
{
    if (_disposed) {
        throw std::runtime_error ("ZLink async submit queue is disposed.");
    }
}

} // namespace zlink::framework::runtime::messaging
