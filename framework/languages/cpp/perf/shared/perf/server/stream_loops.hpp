/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

// §4.2, §13: a server-driven workload is `logicalStreams` independent streams, each with `inflight` closed-loop operations.
// This is only the fan-out of loops and the per-stream sequence source; what one operation calls stays in the scenario file.

#include <perf/server/server_application.hpp>

namespace perf
{
// The per-stream sequence source: the source issues each clientId's sequence, with no reuse inside a cell.
class stream_sequences_t
{
  public:
    explicit stream_sequences_t (int streams) : _values (static_cast<std::size_t> (streams)) {}
    std::uint64_t next (int stream) { return _values[static_cast<std::size_t> (stream) % _values.size ()].fetch_add (1) + 1; }

  private:
    std::vector<std::atomic<std::uint64_t>> _values;
};

// Starts stream x inflight loop coroutines; `loop(stream)` is the scenario's measured loop and returns when the window closes.
template <typename TLoop> void spawn_stream_loops (const loops_t &loops, role_t &role, TLoop loop)
{
    for (int stream = 0; stream < *role.config.workload.logical_streams; ++stream)
        for (int slot = 0; slot < role.config.workload.inflight; ++slot)
            loops->spawn (loop (stream), [&role] (std::exception_ptr error) { role.measurement.record_diagnostic (std::move (error)); });
}

// The return Channel handler of a send/send caller (§10.4, §10.10): the echo arrives as a second one-way send and the
// correlation table decides the operation first result.
class correlation_return_handler_t
{
  public:
    using message_type = echo_reply_t;
    explicit correlation_return_handler_t (role_t &role) : _role (role) {}
    fw::task_t<void> handle (const echo_reply_t &message)
    {
        _role.correlations->reply (message);
        co_return;
    }

  private:
    role_t &_role;
};

// Runs fn(0..count-1) on at most  threads (setup fan-out bounded by --connect-concurrency, §5); the first
// failure is rethrown after every thread has finished.
template <typename TFn> void for_each_concurrently (int count, int concurrency, TFn fn)
{
    std::atomic<int> next{0};
    std::mutex gate;
    std::exception_ptr first_error;
    std::vector<std::thread> workers;
    for (int w = 0; w < std::min (std::max (concurrency, 1), std::max (count, 1)); ++w)
        workers.emplace_back ([&] {
            for (int i; (i = next.fetch_add (1)) < count;) {
                try {
                    fn (i);
                }
                catch (...) {
                    std::lock_guard lock (gate);
                    if (!first_error)
                        first_error = std::current_exception ();
                }
            }
        });
    for (auto &worker : workers)
        worker.join ();
    if (first_error)
        std::rethrow_exception (first_error);
}
} // namespace perf
