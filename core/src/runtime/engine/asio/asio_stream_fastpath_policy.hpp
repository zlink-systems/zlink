/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_ASIO_STREAM_FASTPATH_POLICY_HPP_INCLUDED__
#define __ZLINK_ASIO_STREAM_FASTPATH_POLICY_HPP_INCLUDED__

#include <cstring>

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

inline bool enable_non_tcp_spec_read ()
{
    return env::flag_enabled ("ZLINK_ASIO_STREAM_ENABLE_NON_TCP_SPEC_READ");
}

inline size_t spec_write_budget_bytes ()
{
    return 2097152;
}

//  Diagnostic opt-in. Re-admits the synchronous write turn for non-STREAM
//  sockets, i.e. the behaviour that shipped through 0.13.0. Off by default:
//  that turn has no terminator but a full kernel send buffer, which relocates
//  the application queue into kernel socket memory where byte-HWM accounting
//  cannot see or bound it. Kept only so the old behaviour can be reproduced
//  when diagnosing; it is not a supported configuration.
inline bool legacy_sync_write_opt_in ()
{
    return env::flag_enabled ("ZLINK_ASIO_LEGACY_SYNC_WRITE");
}

//  Diagnostic opt-out for the STREAM speculative write, so the spec'd
//  budgeted turn (08-stream.ko.md:399-401) can be measured against the pure
//  Proactor path. Default off: STREAM keeps the speculative write it is
//  specified to have. Changing that default is a spec amendment.
inline bool stream_async_write_opt_in ()
{
    return env::flag_enabled ("ZLINK_ASIO_STREAM_ASYNC_WRITE");
}

struct transport_fastpath_capabilities_t
{
    bool speculative_write;
    bool speculative_read;
    bool gather_write;
};

struct connection_fastpath_diagnostics_t
{
    bool legacy_sync_write;
    bool stream_async_write;
    bool non_tcp_speculative_read;
};

inline connection_fastpath_diagnostics_t load_connection_fastpath_diagnostics ()
{
    const connection_fastpath_diagnostics_t diagnostics = {
      legacy_sync_write_opt_in (), stream_async_write_opt_in (), enable_non_tcp_spec_read ()};
    return diagnostics;
}

//  Owns the whole "may this engine drain the pipe synchronously?" decision.
//
//  A speculative synchronous write turn keeps pulling messages out of the
//  session pipe and pushing them into the kernel until the pipe runs dry or
//  the kernel send buffer returns EAGAIN. Only the STREAM fast path is
//  specified to do that, and only because spec_write_budget_bytes bounds the
//  turn (core/doc/spec/core/socket/08-stream.ko.md:395-401). Every other
//  socket follows the Proactor write path that
//  core/doc/spec/core/systems/03-io-thread.ko.md section 4 specifies: one
//  async_write_some in flight, re-armed by the completion handler.
//
//  Measured on PAIR/tcp/64B, admitting a general socket into the synchronous
//  turn left 9,485 KiB resident in kernel socket buffers against a 1 MiB pipe
//  HWM, with 0 backpressure parks in 12.25M sends - the HWM never engaged
//  because 90% of the queue had moved outside its jurisdiction. Mean latency
//  was 62.2 ms; on the Proactor path the same cell holds 3.1 KiB and 0.445 ms.
//
//  Kept as a pure predicate so the invariant is directly testable.
inline bool use_speculative_write_for (int socket_type_,
                                       bool tcp_transport_,
                                       bool transport_supports_speculative_,
                                       const connection_fastpath_diagnostics_t &diagnostics_)
{
    if (socket_type_ == ZLINK_CORE_SOCKET_STREAM && tcp_transport_)
        return enable_speculative_write () && !diagnostics_.stream_async_write;

    return transport_supports_speculative_ && diagnostics_.legacy_sync_write;
}

//  Owns the whole "may this connection use a gather write?" decision.
//
//  Gather needs both halves: a transport that can write a header and a body
//  in one operation, and a protocol whose encoder can emit that header apart
//  from the body. Only the ZMP engine has the second half - the raw engine
//  used by STREAM frames nothing, so its body is the whole wire message and
//  there is no header to gather with (core/doc/spec/core/protocol/02-raw).
//  Deciding it once here keeps the raw write turn from preparing a gather
//  that can never be built (see core/doc/spec/core/systems/03-io-thread.ko.md
//  section 4: one prepared buffer per turn).
inline bool use_gather_write_for (int socket_type_,
                                  bool protocol_builds_gather_header_,
                                  bool transport_supports_gather_)
{
    if (!protocol_builds_gather_header_ || !transport_supports_gather_)
        return false;

    return gather_write_enabled ()
           || (socket_type_ == ZLINK_CORE_SOCKET_STREAM && stream_gather_enabled ());
}

inline bool use_speculative_read_for (int socket_type_,
                                      bool tcp_transport_,
                                      bool transport_supports_speculative_,
                                      const connection_fastpath_diagnostics_t &diagnostics_)
{
    return transport_supports_speculative_
           || (socket_type_ == ZLINK_CORE_SOCKET_STREAM
               && (tcp_transport_ || diagnostics_.non_tcp_speculative_read));
}

//  A connection owns this immutable snapshot. Diagnostic environment changes
//  affect engines created afterwards, never a fast-path turn already running
//  on an existing engine.
class connection_fastpath_policy_t
{
  public:
    connection_fastpath_policy_t (
      int socket_type_,
      const char *transport_name_,
      const transport_fastpath_capabilities_t &capabilities_,
      bool protocol_builds_gather_header_,
      const connection_fastpath_diagnostics_t &diagnostics_) :
        _tcp_transport (transport_name_ && std::strcmp (transport_name_, "tcp") == 0),
        _speculative_write_enabled (
          use_speculative_write_for (socket_type_, _tcp_transport,
                                     capabilities_.speculative_write, diagnostics_)),
        _speculative_read_enabled (
          use_speculative_read_for (socket_type_, _tcp_transport,
                                    capabilities_.speculative_read, diagnostics_)),
        _gather_write_enabled (use_gather_write_for (
          socket_type_, protocol_builds_gather_header_, capabilities_.gather_write))
    {
    }

    static connection_fastpath_policy_t from_environment (
      int socket_type_,
      const char *transport_name_,
      const transport_fastpath_capabilities_t &capabilities_,
      bool protocol_builds_gather_header_)
    {
        return connection_fastpath_policy_t (socket_type_, transport_name_, capabilities_,
                                             protocol_builds_gather_header_,
                                             load_connection_fastpath_diagnostics ());
    }

    bool tcp_transport () const { return _tcp_transport; }

    bool speculative_write_enabled () const { return _speculative_write_enabled; }

    bool speculative_read_enabled () const { return _speculative_read_enabled; }

    bool gather_write_enabled () const { return _gather_write_enabled; }

  private:
    const bool _tcp_transport;
    const bool _speculative_write_enabled;
    const bool _speculative_read_enabled;
    const bool _gather_write_enabled;
};

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

//  A read that returned less than it asked for means the kernel receive queue
//  was emptied by that very read: a stream socket returns everything it has up
//  to the requested length. Reading again can only produce EAGAIN, so the
//  engine treats a short read as "socket drained" both for read-target growth
//  and for ending the read drain loop.
inline bool stream_read_filled_request (size_t request_size_, size_t bytes_)
{
    return request_size_ != 0 && bytes_ >= request_size_;
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

    if (last_read_had_partial_prefix_
        || !stream_read_filled_request (last_read_request_size_, bytes_transferred_)) {
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
