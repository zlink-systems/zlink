/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/system/error_code.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <exception>
#include <future>
#include <functional>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>

namespace zlink::stream_connector::detail
{

class transport_connect_control_t final
{
  public:
    using cancel_handler_t = std::function<void ()>;

    void set_cancel_handler (cancel_handler_t handler)
    {
        bool invoke = false;
        {
            std::lock_guard<std::mutex> lock (_mutex);
            if (_cancelled.load (std::memory_order_acquire))
                invoke = true;
            else
                _cancel_handler = std::move (handler);
        }
        if (invoke && handler)
            handler ();
    }

    void cancel ()
    {
        cancel_handler_t handler;
        {
            std::lock_guard<std::mutex> lock (_mutex);
            if (_cancelled.exchange (true, std::memory_order_acq_rel))
                return;
            handler = std::move (_cancel_handler);
        }
        if (handler)
            handler ();
    }

    bool cancelled () const noexcept
    {
        return _cancelled.load (std::memory_order_acquire);
    }

  private:
    mutable std::mutex _mutex;
    std::atomic_bool _cancelled{false};
    cancel_handler_t _cancel_handler;
};

template <typename Strand, typename Function>
auto run_serialized_sync (boost::asio::io_context &io_context,
                          Strand &strand,
                          Function function) -> std::invoke_result_t<Function &>
{
    using result_type = std::invoke_result_t<Function &>;
    if (strand.running_in_this_thread () || io_context.stopped ()) {
        return function ();
    }

    auto promise = std::make_shared<std::promise<result_type>> ();
    auto ready = promise->get_future ();
    boost::asio::post (
      strand,
      [promise, function = std::move (function)] () mutable {
          try {
              if constexpr (std::is_void_v<result_type>) {
                  function ();
                  promise->set_value ();
              } else {
                  promise->set_value (function ());
              }
          }
          catch (...) {
              promise->set_exception (std::current_exception ());
          }
      });
    return ready.get ();
}

class stream_connection_t : public std::enable_shared_from_this<stream_connection_t>
{
  public:
    virtual ~stream_connection_t () = default;
    virtual bool is_open () const = 0;
    virtual std::size_t available (boost::system::error_code &error) = 0;
    virtual std::size_t
    read_some (std::uint8_t *buffer, std::size_t size, boost::system::error_code &error) = 0;
    virtual void async_read_some (
      std::size_t max_size,
      std::function<void (boost::system::error_code, std::vector<std::uint8_t>)> completion) = 0;
    // Message-oriented transports must reject an oversized remote message
    // before Beast materializes it. Byte-stream transports do not need a
    // transport-level limit and keep the default implementation.
    virtual void set_read_message_limit (std::size_t) {}
    virtual bool
    wait_readable_until (std::chrono::steady_clock::time_point deadline,
                         boost::system::error_code &error)
    {
        (void) deadline;
        error.clear ();
        return true;
    }
    virtual void write (const std::vector<std::uint8_t> &bytes) = 0;
    virtual void async_write (std::vector<std::uint8_t> bytes,
                              std::function<void (boost::system::error_code)> completion) = 0;
    virtual void shutdown_and_close () = 0;
    virtual void shutdown_and_close_async () { shutdown_and_close (); }
    virtual void close (boost::system::error_code &error) = 0;
};

} // namespace zlink::stream_connector::detail
