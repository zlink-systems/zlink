/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/channels/channel_pending_requests.hpp"

#include <utility>

namespace zlink::framework::detail
{

std::uint64_t channel_pending_requests_t::next_request_seq ()
{
    const auto request_seq = _next_request_seq++;
    if (_next_request_seq == 0) {
        _next_request_seq = 1;
    }
    return request_seq;
}

bool channel_pending_requests_t::register_request (std::uint64_t request_seq,
                                                   std::string channel_name)
{
    return _pending.emplace (request_seq, std::move (channel_name)).second;
}

std::optional<std::string> channel_pending_requests_t::remove (std::uint64_t request_seq)
{
    const auto found = _pending.find (request_seq);
    if (found == _pending.end ()) {
        return std::nullopt;
    }
    auto channel_name = std::move (found->second);
    _pending.erase (found);
    return channel_name;
}

bool channel_pending_requests_t::contains (std::uint64_t request_seq) const
{
    return _pending.find (request_seq) != _pending.end ();
}

void channel_pending_requests_t::clear () noexcept
{
    _pending.clear ();
}

std::size_t channel_pending_requests_t::count () const noexcept
{
    return _pending.size ();
}

} // namespace zlink::framework::detail
