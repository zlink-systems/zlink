/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_STREAM_DISPATCH_SEND_POLICY_INTERNAL_HPP_INCLUDED__
#define __ZLINK_STREAM_DISPATCH_SEND_POLICY_INTERNAL_HPP_INCLUDED__

#include <zlink.h>

#include "sockets/stream/stream.hpp"
#include "sockets/stream/stream_dispatch_internal.hpp"

#include <chrono>
#include <thread>

namespace zlink
{
inline uint32_t stream_dispatch_decode_routing_id (const zlink_routing_id_t *rid_)
{
    return (static_cast<uint32_t> (rid_->data[0]) << 24)
           | (static_cast<uint32_t> (rid_->data[1]) << 16)
           | (static_cast<uint32_t> (rid_->data[2]) << 8) | static_cast<uint32_t> (rid_->data[3]);
}

inline pipe_t *resolve_direct_dispatch_output_pipe (const stream_t *socket_, uint32_t routing_id_)
{
    if (!socket_ || !stream_dispatch_owns_socket (socket_))
        return NULL;

    if (stream_dispatch_context_t::current_routing_id () != routing_id_)
        return NULL;

    pipe_t *dispatch_pipe = stream_dispatch_context_t::current_pipe ();
    if (!dispatch_pipe)
        return NULL;

    pipe_t *out = dispatch_pipe->get_peer ();
    return out ? out : dispatch_pipe;
}

inline pipe_t *resolve_current_dispatch_output_pipe ()
{
    pipe_t *dispatch_pipe = stream_dispatch_context_t::current_pipe ();
    if (!dispatch_pipe)
        return NULL;

    pipe_t *direct_out = dispatch_pipe->get_peer ();
    return direct_out ? direct_out : dispatch_pipe;
}

class stream_dispatch_send_policy_t
{
  public:
    explicit stream_dispatch_send_policy_t (int flags_, int sndtimeo_) :
        _dontwait ((flags_ & ZLINK_DONTWAIT) != 0 || sndtimeo_ == 0),
        _sndtimeo (sndtimeo_),
        _deadline (sndtimeo_ < 0
                     ? std::chrono::steady_clock::time_point::max ()
                     : std::chrono::steady_clock::now () + std::chrono::milliseconds (sndtimeo_)),
        _retry_count (0)
    {
    }

    bool dontwait () const { return _dontwait; }

    bool timed_out () const
    {
        return _sndtimeo >= 0 && std::chrono::steady_clock::now () >= _deadline;
    }

    bool wait_retry () const
    {
        if (_dontwait)
            return false;

        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now ();
        if (now >= _deadline)
            return false;

        if (_retry_count < 32) {
            ++_retry_count;
            std::this_thread::yield ();
            return true;
        }

        const std::chrono::microseconds remaining =
          std::chrono::duration_cast<std::chrono::microseconds> (_deadline - now);
        const std::chrono::microseconds backoff =
          remaining < std::chrono::microseconds (100) ? remaining : std::chrono::microseconds (100);
        if (backoff.count () > 0)
            std::this_thread::sleep_for (backoff);
        else
            std::this_thread::yield ();

        return true;
    }

  private:
    bool _dontwait;
    int _sndtimeo;
    std::chrono::steady_clock::time_point _deadline;
    mutable unsigned int _retry_count;
};
}

#endif
