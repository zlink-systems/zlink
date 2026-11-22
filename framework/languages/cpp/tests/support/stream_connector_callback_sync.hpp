/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include <zlink/stream_connector.hpp>
#include <boost/asio/post.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace stream_connector_fixture
{
inline constexpr auto eventually = [] (const std::function<bool ()> &predicate) {
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
    while (!predicate () && std::chrono::steady_clock::now () < deadline) {
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    return predicate ();
};

class callback_latch_t
{
  public:
    void signal ()
    {
        // Notify under the lock so a waiter cannot destroy this object mid-notify.
        std::lock_guard<std::mutex> lock (_mutex);
        _ready = true;
        _changed.notify_all ();
    }

    bool wait_for (std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock (_mutex);
        return _changed.wait_for (lock, timeout, [this] { return _ready; });
    }

  private:
    std::mutex _mutex;
    std::condition_variable _changed;
    bool _ready = false;
};

// A callback owns its flag and latch, so a late callback never touches freed stack state.
struct callback_outcome_t
{
    std::atomic_bool seen{false};
    callback_latch_t latch;
};

class joining_thread_t
{
  public:
    joining_thread_t () = default;

    template <typename F, typename... Args>
    explicit joining_thread_t (F &&function, Args &&...args) :
        _thread (std::forward<F> (function), std::forward<Args> (args)...)
    {
    }

    joining_thread_t (joining_thread_t &&) noexcept = default;
    joining_thread_t &operator= (joining_thread_t &&) noexcept = default;

    joining_thread_t (const joining_thread_t &) = delete;
    joining_thread_t &operator= (const joining_thread_t &) = delete;

    ~joining_thread_t ()
    {
        if (_thread.joinable ()) {
            _thread.join ();
        }
    }

    void join ()
    {
        if (_thread.joinable ()) {
            _thread.join ();
        }
    }

  private:
    std::thread _thread;
};

/* Returns once every handler posted to `strand` before the call has run. */
template <typename TStrand> bool drain_strand (TStrand &strand)
{
    auto drained = std::make_shared<std::promise<void>> ();
    auto future = drained->get_future ();
    boost::asio::post (strand, [drained] { drained->set_value (); });
    return future.wait_for (std::chrono::seconds (5)) == std::future_status::ready;
}

/* Collects the results of Send callbacks and waits for them to settle. */
class send_results_t
{
  public:
    std::function<void (zlink::stream_connector::result_t<void>)> recorder ()
    {
        return [this] (zlink::stream_connector::result_t<void> result) {
            {
                std::lock_guard<std::mutex> lock (_mutex);
                _codes.push_back (result.error_code ());
                _changed.notify_all ();
            }
        };
    }

    bool wait_for_count (std::size_t count)
    {
        std::unique_lock<std::mutex> lock (_mutex);
        return _changed.wait_for (lock, std::chrono::seconds (5),
                                  [&] { return _codes.size () >= count; });
    }

    std::vector<std::optional<zlink::stream_connector::error_code_t>> codes ()
    {
        std::lock_guard<std::mutex> lock (_mutex);
        return _codes;
    }

    bool all_succeeded ()
    {
        const auto settled = codes ();
        return std::all_of (settled.begin (), settled.end (),
                            [] (const auto &code) { return !code.has_value (); });
    }

  private:
    std::mutex _mutex;
    std::condition_variable _changed;
    std::vector<std::optional<zlink::stream_connector::error_code_t>> _codes;
};

inline bool dispatch_until (zlink::stream_connector::connector_t &connector,
                            callback_latch_t &latch,
                            std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now () + timeout;
    do {
        connector.dispatch ();
        if (latch.wait_for (std::chrono::milliseconds (1))) {
            return true;
        }
    } while (std::chrono::steady_clock::now () < deadline);
    connector.dispatch ();
    return latch.wait_for (std::chrono::milliseconds (0));
}
} // namespace stream_connector_fixture
