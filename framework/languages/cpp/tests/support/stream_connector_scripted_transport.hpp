/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "runtime/connector_runtime.hpp"
#include <boost/asio/error.hpp>
#include <zlink/Contracts/Messaging/message.hpp>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace stream_connector_fixture
{
class async_write_connection_t final : public zlink::stream_connector::detail::stream_connection_t
{
  public:
    explicit async_write_connection_t (bool fail_write = false) : _fail_write (fail_write) {}

    void async_read_some (std::size_t,
                          std::function<void (boost::system::error_code, std::vector<std::uint8_t>)>
                            completion) override
    {
        std::lock_guard lock (_mutex);
        _read_completion = std::move (completion);
    }

    void async_write (std::vector<std::uint8_t> bytes,
                      std::function<void (boost::system::error_code)> completion) override
    {
        {
            std::lock_guard lock (_mutex);
            _written.push_back (std::move (bytes));
        }
        completion (_fail_write ? boost::asio::error::operation_aborted
                                : boost::system::error_code{});
    }

    boost::system::error_code shutdown_and_close () override
    {
        std::lock_guard lock (_mutex);
        _read_completion = {};
        return {};
    }

    std::vector<std::vector<std::uint8_t>> written () const
    {
        std::lock_guard lock (_mutex);
        return _written;
    }

  private:
    mutable std::mutex _mutex;
    std::vector<std::vector<std::uint8_t>> _written;
    std::function<void (boost::system::error_code, std::vector<std::uint8_t>)> _read_completion;
    bool _fail_write = false;
};

/* Serves a scripted byte stream to async_read_some. Once the script is
 * consumed, an idle async read stays pending like a real socket; with
 * eof_after, the stream ends with EOF after the script. */
class scripted_read_connection_t final : public zlink::stream_connector::detail::stream_connection_t
{
  public:
    scripted_read_connection_t (std::vector<std::uint8_t> bytes, bool eof_after) :
        _bytes (std::move (bytes)), _eof_after (eof_after)
    {
    }

    void async_read_some (std::size_t,
                          std::function<void (boost::system::error_code, std::vector<std::uint8_t>)>
                            completion) override
    {
        std::unique_lock<std::mutex> lock (_mutex);
        if (_offset < _bytes.size () || (_eof_after && !_eof_reported)) {
            std::vector<std::uint8_t> rest (_bytes.begin () + static_cast<std::ptrdiff_t> (_offset),
                                            _bytes.end ());
            _offset = _bytes.size ();
            boost::system::error_code error;
            if (_eof_after) {
                error = boost::asio::error::eof;
                _eof_reported = true;
            }
            lock.unlock ();
            completion (error, std::move (rest));
            return;
        }
        _pending_read = std::move (completion);
    }

    /* Completes the held idle read with EOF, as a socket that ends later. */
    bool fail_pending_read ()
    {
        std::function<void (boost::system::error_code, std::vector<std::uint8_t>)> completion;
        {
            std::lock_guard<std::mutex> lock (_mutex);
            completion = std::move (_pending_read);
            _pending_read = {};
        }
        if (!completion) {
            return false;
        }
        completion (boost::asio::error::eof, {});
        return true;
    }

    void async_write (std::vector<std::uint8_t>,
                      std::function<void (boost::system::error_code)> completion) override
    {
        completion ({});
    }

    boost::system::error_code shutdown_and_close () override
    {
        std::lock_guard<std::mutex> lock (_mutex);
        _pending_read = {};
        return {};
    }

  private:
    std::mutex _mutex;
    std::vector<std::uint8_t> _bytes;
    std::size_t _offset = 0;
    bool _eof_after = false;
    bool _eof_reported = false;
    std::function<void (boost::system::error_code, std::vector<std::uint8_t>)> _pending_read;
};

class held_async_write_connection_t final
    : public zlink::stream_connector::detail::stream_connection_t
{
  public:
    void async_read_some (std::size_t,
                          std::function<void (boost::system::error_code, std::vector<std::uint8_t>)>
                            completion) override
    {
        std::lock_guard lock (_mutex);
        _read_completion = std::move (completion);
    }

    void async_write (std::vector<std::uint8_t> bytes,
                      std::function<void (boost::system::error_code)> completion) override
    {
        {
            std::lock_guard lock (_mutex);
            _overlapped = _overlapped || static_cast<bool> (_write_completion);
            _written.push_back (std::move (bytes));
            _write_completion = std::move (completion);
        }
        _write_started.notify_all ();
    }

    /* Waits until `count` writes have started. The bound only ends a broken
   * run; a passing run returns as soon as the write starts. */
    bool wait_for_write_count (std::size_t count) const
    {
        std::unique_lock lock (_mutex);
        return _write_started.wait_for (lock, std::chrono::seconds (5),
                                        [&] { return _written.size () >= count; });
    }

    void complete_write ()
    {
        std::function<void (boost::system::error_code)> completion;
        {
            std::lock_guard lock (_mutex);
            completion = std::move (_write_completion);
        }
        if (completion)
            completion ({});
    }

    void fail_write ()
    {
        std::function<void (boost::system::error_code)> completion;
        {
            std::lock_guard lock (_mutex);
            completion = std::move (_write_completion);
        }
        if (completion)
            completion (boost::asio::error::operation_aborted);
    }

    std::size_t write_count () const
    {
        std::lock_guard lock (_mutex);
        return _written.size ();
    }

    bool overlapped () const
    {
        std::lock_guard lock (_mutex);
        return _overlapped;
    }

    std::vector<std::uint8_t> written (std::size_t index) const
    {
        std::lock_guard lock (_mutex);
        return _written.at (index);
    }

    boost::system::error_code shutdown_and_close () override
    {
        std::lock_guard lock (_mutex);
        _read_completion = {};
        _write_completion = {};
        return {};
    }

  private:
    mutable std::mutex _mutex;
    mutable std::condition_variable _write_started;
    bool _overlapped = false;
    std::vector<std::vector<std::uint8_t>> _written;
    std::function<void (boost::system::error_code, std::vector<std::uint8_t>)> _read_completion;
    std::function<void (boost::system::error_code)> _write_completion;
};

/* A connection whose inbound bytes the test feeds one step at a time; every
 * write completes at once and is recorded. */
class fed_read_connection_t final : public zlink::stream_connector::detail::stream_connection_t
{
  public:
    void async_read_some (std::size_t,
                          std::function<void (boost::system::error_code, std::vector<std::uint8_t>)>
                            completion) override
    {
        std::unique_lock lock (_mutex);
        if (_inbound.empty ()) {
            _pending_read = std::move (completion);
            return;
        }
        auto bytes = std::move (_inbound);
        _inbound.clear ();
        lock.unlock ();
        completion ({}, std::move (bytes));
    }

    /* Completes the pending read with `bytes` on the calling thread, as the
   * read pump's completion. */
    void feed (const zlink::message_t &frame)
    {
        const auto text = frame.to_string ();
        feed (std::vector<std::uint8_t> (text.begin (), text.end ()));
    }

    void feed (std::vector<std::uint8_t> bytes)
    {
        std::unique_lock lock (_mutex);
        if (!_pending_read) {
            _inbound.insert (_inbound.end (), bytes.begin (), bytes.end ());
            return;
        }
        auto completion = std::move (_pending_read);
        _pending_read = {};
        lock.unlock ();
        completion ({}, std::move (bytes));
    }

    /* Completes the pending read with EOF, as a peer that closed the socket. */
    void end ()
    {
        std::unique_lock lock (_mutex);
        auto completion = std::move (_pending_read);
        _pending_read = {};
        lock.unlock ();
        if (completion) {
            completion (boost::asio::error::eof, {});
        }
    }

    void async_write (std::vector<std::uint8_t> bytes,
                      std::function<void (boost::system::error_code)> completion) override
    {
        {
            std::lock_guard lock (_mutex);
            _written.push_back (std::move (bytes));
        }
        _write_started.notify_all ();
        completion ({});
    }

    /* The frame of write `index`, once it has been written. */
    std::optional<std::vector<std::uint8_t>> wait_for_written (std::size_t index)
    {
        std::unique_lock lock (_mutex);
        if (!_write_started.wait_for (lock, std::chrono::seconds (5),
                                      [&] { return _written.size () > index; })) {
            return std::nullopt;
        }
        return _written[index];
    }

    std::size_t write_count ()
    {
        std::lock_guard lock (_mutex);
        return _written.size ();
    }

    boost::system::error_code shutdown_and_close () override
    {
        std::lock_guard lock (_mutex);
        _pending_read = {};
        return {};
    }

  private:
    std::mutex _mutex;
    std::condition_variable _write_started;
    std::vector<std::uint8_t> _inbound;
    std::vector<std::vector<std::uint8_t>> _written;
    std::function<void (boost::system::error_code, std::vector<std::uint8_t>)> _pending_read;
};
} // namespace stream_connector_fixture
