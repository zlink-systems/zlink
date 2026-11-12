/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#define main bench_source_entry
#include "../client/bench_cpp_client.cpp"
#undef main
#include <set>
#include <deque>

namespace
{
class probe_driver_t final : public driver_t
{
  public:
    probe_driver_t (const options_t &options, int window, bool command, bool corrupt) :
        driver_t (options, window, command), _corrupt (corrupt), _completion ([this] {
            for (;;) {
                std::function<void ()> callback;
                {
                    std::unique_lock lock (_mutex);
                    _changed.wait (lock, [&] { return _stop || !_callbacks.empty (); });
                    if (_callbacks.empty ())
                        return;
                    callback = std::move (_callbacks.front ());
                    _callbacks.pop_front ();
                }
                callback ();
            }
        }) {}
    ~probe_driver_t () override
    {
        { std::lock_guard lock (_mutex); _stop = true; }
        _changed.notify_one ();
        _completion.join ();
    }
    bool await_ready (int) override { return true; }
    std::set<std::thread::id> submit_threads;
    std::set<uint64_t> sequences;
    bool duplicate_sequence = false, moved_callback = false;
  private:
    void submit (std::shared_ptr<stream_t> stream, int index, uint64_t seq) override
    {
        const auto submit_id = std::this_thread::get_id ();
        auto payload = make_payload (stream->size, stream->phase, seq);
        {
            std::lock_guard lock (_mutex);
            submit_threads.insert (submit_id);
            duplicate_sequence |= !sequences.insert (seq).second;
            const bool corrupt = _corrupt && _corrupted.insert (index).second;
            _callbacks.push_back ([this, stream, seq, submit_id, corrupt, payload = std::move (payload)] {
                moved_callback |= std::this_thread::get_id () == submit_id;
                stream->finish (now_ns (), _command ? nullptr : &payload, corrupt ? seq + 1 : seq);
            });
        }
        _changed.notify_one ();
        stream->admit ();
    }
    bool _corrupt, _stop = false;
    std::mutex _mutex;
    std::condition_variable _changed;
    std::deque<std::function<void ()>> _callbacks;
    std::set<int> _corrupted;
    std::thread _completion;
};
void require (bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error (message);
}
}

int main ()
{
    for (const auto &pattern : {"request-serial", "request-backpressure", "send-saturation"}) {
        options_t options;
        options.pattern = pattern;
        options.latency_sample_limit = 100;
        const bool serial = options.pattern == "request-serial";
        const bool command = options.pattern == "send-saturation";
        probe_driver_t driver (options, pattern_window (pattern), command, !command);
        counters_t counters;
        latency_sampler_t latency (100);
        driver.run (clock_t_::now () + std::chrono::milliseconds (200), 1024, phase_active, counters, &latency);
        const int count = serial ? 1 : 8;
        require (driver.submit_threads.size () == count, "each stream must submit on a distinct thread");
        require (!driver.duplicate_sequence, "sequences must be unique across streams");
        require (!driver.moved_callback, "callbacks must stay on the completion resource");
        require (counters.completed > 0 && counters.outstanding == 0, "driver must complete and drain");
        require (counters.submitted == counters.completed + counters.errors, "terminal conservation");
        require (counters.errors == (command ? 0 : count), "wrong sequence must be rejected per stream");
        if (!command)
            require (counters.error_summary.size () == 1 && counters.error_summary[0].count == count,
                     "stream error summaries must merge without lost counts");
        if (serial || command)
            require (counters.peak_in_flight == count, "serial and send allow one outstanding operation per stream");
        std::printf ("%s: threads=%zu submitted=%lld completed=%lld deliberately_invalid=%lld\n",
                     pattern, driver.submit_threads.size (), counters.submitted.load (),
                     counters.completed.load (), counters.errors.load ());
    }
}
