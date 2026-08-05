#ifndef PERF_STREAM_PERF_CLIENT_IFACE_HPP
#define PERF_STREAM_PERF_CLIENT_IFACE_HPP

// Interface contract between client_session_t and bench_client_t.
// Decouples the async session from the concrete orchestrator, allowing
// the session to report events without knowing the full bench_client_t type.

#include <cstddef>
#include <cstdint>
#include <memory>

#include "../../multi/common/perf_multi_metric_header.hpp"

class client_session_t;

class bench_client_iface_t
{
  public:
    virtual ~bench_client_iface_t () {}

    // --- Connection lifecycle ---
    virtual void on_connect_result (bool success,
                                    const std::shared_ptr<client_session_t> &session) = 0;

    // --- Phase control (called by session before sending) ---
    virtual bool allow_send () const = 0;           // true if within active phase window
    virtual size_t current_phase_size () const = 0; // payload size for current phase
    virtual uint32_t metric_run_id () const = 0;
    virtual perf_multi_metric::phase_t metric_phase () const = 0;
    virtual uint64_t next_seq () = 0; // monotonic sequence for latency embedding

    // --- Metrics callbacks (called from I/O threads via strand) ---
    virtual void on_send_begin (size_t size) = 0; // increment outstanding
    virtual void on_recv_done (size_t bytes,
                               uint64_t sent_ts_ns) = 0; // decrement outstanding, record RTT
    virtual void on_send_error () = 0;
    virtual void on_recv_error () = 0;
    virtual void on_abandon (long count) = 0; // adjust outstanding on close/error
    virtual void on_size_mismatch () = 0;
};

#endif
