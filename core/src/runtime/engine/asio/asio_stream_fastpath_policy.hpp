/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_ASIO_STREAM_FASTPATH_POLICY_HPP_INCLUDED__
#define __ZLINK_ASIO_STREAM_FASTPATH_POLICY_HPP_INCLUDED__

#include "utils/env.hpp"

namespace zlink
{
namespace asio_stream_fastpath_policy
{
inline bool gather_write_enabled ()
{
    return env::flag_enabled ("ZLINK_ASIO_GATHER_WRITE");
}

inline bool single_write_enabled ()
{
    return env::flag_enabled ("ZLINK_ASIO_SINGLE_WRITE");
}

inline size_t gather_threshold ()
{
    return env::positive_size ("ZLINK_ASIO_GATHER_THRESHOLD", 65536);
}

inline bool stream_gather_enabled ()
{
    return !env::flag_enabled ("ZLINK_ASIO_STREAM_DISABLE_GATHER");
}

inline size_t stream_gather_threshold ()
{
    return env::positive_size ("ZLINK_ASIO_STREAM_GATHER_THRESHOLD", 1024);
}

inline size_t stream_tiny_gather_threshold ()
{
    return env::positive_size ("ZLINK_ASIO_STREAM_TINY_GATHER_THRESHOLD", 0);
}

inline bool trace_enabled ()
{
    return env::flag_enabled ("ZLINK_ASIO_TRACE");
}

inline bool enable_handler_alloc ()
{
    return true;
}

inline bool enable_read_drain ()
{
    return true;
}

inline bool enable_speculative_write ()
{
    return true;
}

inline bool enable_rx_slab ()
{
    return true;
}

inline bool enable_non_tcp_spec_read ()
{
    return env::flag_enabled ("ZLINK_ASIO_STREAM_ENABLE_NON_TCP_SPEC_READ");
}

inline size_t spec_write_budget_bytes ()
{
    return 2097152;
}

inline size_t read_drain_max_loops ()
{
    return 64;
}

inline size_t read_drain_max_bytes ()
{
    return 1048576;
}

inline size_t default_target_size ()
{
    return 4096;
}

inline size_t initial_target_cap ()
{
    return env::positive_size ("ZLINK_ASIO_STREAM_INITIAL_TARGET_CAP", 4096);
}

inline size_t
clamp_stream_target_limit (size_t target_, const zlink::options_t &options_, bool read_path_)
{
    size_t clamped = target_ > 0 ? target_ : default_target_size ();

    if (read_path_ && options_.rcvbuf > 0 && static_cast<size_t> (options_.rcvbuf) < clamped) {
        clamped = static_cast<size_t> (options_.rcvbuf);
    }

    if (!read_path_ && options_.sndbuf > 0 && static_cast<size_t> (options_.sndbuf) < clamped) {
        clamped = static_cast<size_t> (options_.sndbuf);
    }

    if (options_.maxmsgsize > 0 && static_cast<size_t> (options_.maxmsgsize) < clamped) {
        clamped = static_cast<size_t> (options_.maxmsgsize);
    }

    return clamped > 0 ? clamped : static_cast<size_t> (1);
}

inline size_t decoder_initial_read_target (const zlink::options_t &options_,
                                           size_t stream_min_target_,
                                           size_t target_cap_)
{
    size_t target = options_.in_batch_size > 0 ? static_cast<size_t> (options_.in_batch_size)
                                               : default_target_size ();
    if (options_.type == ZLINK_CORE_SOCKET_STREAM && target < stream_min_target_)
        target = stream_min_target_;
    if (target > target_cap_)
        target = target_cap_;
    return clamp_stream_target_limit (target, options_, true);
}

inline size_t decoder_initial_read_target (const zlink::options_t &options_)
{
    return decoder_initial_read_target (options_, 0, initial_target_cap ());
}

inline size_t decoder_max_read_target (const zlink::options_t &options_,
                                       size_t stream_min_target_,
                                       size_t target_cap_)
{
    const size_t initial_target =
      decoder_initial_read_target (options_, stream_min_target_, target_cap_);
    size_t max_target = initial_target;

    if (options_.rcvbuf > 0 && static_cast<size_t> (options_.rcvbuf) > max_target)
        max_target = static_cast<size_t> (options_.rcvbuf);

    if (options_.maxmsgsize > 0 && static_cast<size_t> (options_.maxmsgsize) < max_target)
        max_target = static_cast<size_t> (options_.maxmsgsize);

    if (max_target == 0)
        return initial_target;

    return max_target;
}

inline size_t decoder_max_read_target (const zlink::options_t &options_)
{
    return decoder_max_read_target (options_, 0, initial_target_cap ());
}

inline size_t encoder_initial_write_target (const zlink::options_t &options_,
                                            size_t stream_min_target_,
                                            size_t target_cap_)
{
    size_t target = options_.out_batch_size > 0 ? static_cast<size_t> (options_.out_batch_size)
                                                : default_target_size ();
    if (options_.type == ZLINK_CORE_SOCKET_STREAM && target < stream_min_target_)
        target = stream_min_target_;
    if (target > target_cap_)
        target = target_cap_;
    return clamp_stream_target_limit (target, options_, false);
}

inline size_t encoder_initial_write_target (const zlink::options_t &options_)
{
    return encoder_initial_write_target (options_, 0, initial_target_cap ());
}

inline size_t encoder_max_write_target (const zlink::options_t &options_,
                                        size_t stream_min_target_,
                                        size_t target_cap_)
{
    const size_t initial_target =
      encoder_initial_write_target (options_, stream_min_target_, target_cap_);
    size_t max_target = initial_target;

    if (options_.sndbuf > 0 && static_cast<size_t> (options_.sndbuf) > max_target)
        max_target = static_cast<size_t> (options_.sndbuf);

    if (options_.maxmsgsize > 0 && static_cast<size_t> (options_.maxmsgsize) < max_target)
        max_target = static_cast<size_t> (options_.maxmsgsize);

    if (max_target == 0)
        return initial_target;

    return max_target;
}

inline size_t encoder_max_write_target (const zlink::options_t &options_)
{
    return encoder_max_write_target (options_, 0, initial_target_cap ());
}

inline size_t next_stream_target_after_full_hit (size_t current_,
                                                 size_t max_,
                                                 size_t *full_hits_,
                                                 size_t required_hits_)
{
    if (!full_hits_ || current_ >= max_)
        return 0;

    ++(*full_hits_);
    if (*full_hits_ < required_hits_)
        return 0;
    *full_hits_ = 0;

    size_t grown = current_;
    if (grown > max_ / 2)
        grown = max_;
    else
        grown *= 2;

    if (grown > max_)
        grown = max_;
    return grown > current_ ? grown : 0;
}

inline bool can_grow_stream_target (int socket_type_, const void *codec_, size_t current_, size_t max_)
{
    return socket_type_ == ZLINK_CORE_SOCKET_STREAM && codec_ != NULL && max_ > current_;
}

inline size_t next_decoder_read_target (int socket_type_,
                                        const void *decoder_,
                                        size_t current_,
                                        size_t max_,
                                        bool last_read_had_partial_prefix_,
                                        size_t last_read_request_size_,
                                        size_t bytes_transferred_,
                                        size_t *full_hits_,
                                        size_t required_hits_)
{
    if (!can_grow_stream_target (socket_type_, decoder_, current_, max_))
        return 0;

    if (last_read_had_partial_prefix_ || last_read_request_size_ == 0) {
        if (full_hits_)
            *full_hits_ = 0;
        return 0;
    }

    if (bytes_transferred_ < last_read_request_size_) {
        if (full_hits_)
            *full_hits_ = 0;
        return 0;
    }

    return next_stream_target_after_full_hit (current_, max_, full_hits_, required_hits_);
}

inline size_t next_encoder_write_target (int socket_type_,
                                         const void *encoder_,
                                         size_t current_,
                                         size_t max_,
                                         size_t filled_out_batch_,
                                         size_t *full_hits_,
                                         size_t required_hits_)
{
    if (!can_grow_stream_target (socket_type_, encoder_, current_, max_))
        return 0;

    if (filled_out_batch_ < current_) {
        if (full_hits_)
            *full_hits_ = 0;
        return 0;
    }

    return next_stream_target_after_full_hit (current_, max_, full_hits_, required_hits_);
}

template <typename Engine>
inline size_t output_target_batch (const Engine &engine_, const zlink::options_t &options_)
{
    if (options_.type == ZLINK_CORE_SOCKET_STREAM) {
        const size_t stream_target = engine_.stream_encoder_write_target_size ();
        return stream_target > 0 ? stream_target : static_cast<size_t> (options_.out_batch_size);
    }

    return static_cast<size_t> (options_.out_batch_size);
}
}
}

#endif
