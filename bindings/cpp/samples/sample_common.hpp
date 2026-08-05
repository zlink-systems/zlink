/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_SAMPLES_COMMON_SAMPLE_COMMON_HPP_INCLUDED
#define ZLINK_CPP_SAMPLES_COMMON_SAMPLE_COMMON_HPP_INCLUDED

#include <boost/asio.hpp>
#include <zlink.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <functional>
#include <future>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(ZLINK_HAVE_WINDOWS)
#include <process.h>
#endif

namespace detail
{

inline const char *const k_pair_payload = "hello-pair";
inline const char *const k_dealer_router_request = "ping";
inline const char *const k_dealer_router_reply = "pong";
inline const char *const k_stream_payload = "hello-stream";
inline const char *const k_pubsub_topic = "prices";
inline const char *const k_pubsub_payload = "101.25";
inline const char *const k_spot_topic = "room:lobby";
inline const char *const k_spot_payload = "hello-spot";

inline std::string unique_tcp (const char *base_)
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor acceptor (io);
    boost::asio::ip::tcp::endpoint endpoint (boost::asio::ip::make_address ("127.0.0.1"), 0);
    acceptor.open (endpoint.protocol ());
    acceptor.set_option (boost::asio::socket_base::reuse_address (true));
    acceptor.bind (endpoint);
    const unsigned short port = acceptor.local_endpoint ().port ();
    acceptor.close ();

    std::ostringstream stream;
    (void) base_;
    stream << "tcp://127.0.0.1:" << port;
    return stream.str ();
}

inline zlink::message_t make_message (const std::string &text_)
{
    return zlink::message_t::from (text_);
}

inline bool wait_until (std::condition_variable &cv_,
                        std::unique_lock<std::mutex> &lock_,
                        bool &ready_,
                        int timeout_ms_)
{
    return cv_.wait_for (lock_, std::chrono::milliseconds (timeout_ms_),
                         [&ready_] { return ready_; });
}

template <typename MonitorLike>
inline bool wait_for_monitor_readable (MonitorLike &monitor_, int timeout_ms_)
{
    zlink::poller_t poller;
    if (!poller.valid ())
        return false;
    try {
        poller.add (monitor_, zlink::poll_event_flag_t::pollin, 1);
    }
    catch (const zlink::binding_error_t &) {
        return false;
    }

    zlink::poll_event_t event;
    return poller.wait (&event, 1, std::chrono::milliseconds (timeout_ms_)) == 1;
}

inline bool wait_for_socket_monitor_event (zlink::socket_monitor_t &monitor_,
                                           uint64_t event_type_,
                                           int timeout_ms_,
                                           int64_t value_ = -1)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (timeout_ms_);

    while (std::chrono::steady_clock::now () < deadline) {
        const std::chrono::steady_clock::duration remaining =
          deadline - std::chrono::steady_clock::now ();
        const int remaining_ms = static_cast<int> (
          std::chrono::duration_cast<std::chrono::milliseconds> (remaining).count ());
        if (!wait_for_monitor_readable (monitor_, remaining_ms))
            continue;

        const std::optional<zlink::monitor_event_t> event =
          monitor_.recv (zlink::recv_flags_t::dontwait);
        if (!event)
            continue;
        if (static_cast<uint64_t> (event->event) != event_type_)
            continue;
        if (value_ >= 0 && static_cast<int64_t> (event->value) != value_)
            continue;
        return true;
    }

    return false;
}

inline bool wait_connected (zlink::socket_monitor_t &server_monitor_,
                            zlink::socket_monitor_t &client_monitor_,
                            int timeout_ms_ = 2000)
{
    return wait_for_socket_monitor_event (
             server_monitor_, static_cast<uint64_t> (zlink::monitor_event::connection_ready),
             timeout_ms_)
           && wait_for_socket_monitor_event (
             client_monitor_, static_cast<uint64_t> (zlink::monitor_event::connection_ready),
             timeout_ms_);
}

inline bool wait_stream_connected (zlink::socket_monitor_t &server_monitor_, int timeout_ms_ = 2000)
{
    return wait_for_socket_monitor_event (
      server_monitor_, static_cast<uint64_t> (zlink::monitor_event::accepted), timeout_ms_);
}

template <typename T> inline T wait_future (std::future<T> &future_, int timeout_ms_)
{
    assert (future_.wait_for (std::chrono::milliseconds (timeout_ms_))
            == std::future_status::ready);
    return future_.get ();
}

template <typename T, typename ProgressFn>
inline T wait_future_with_progress (std::future<T> &future_, int timeout_ms_, ProgressFn progress_)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (timeout_ms_);

    while (std::chrono::steady_clock::now () < deadline) {
        if (future_.wait_for (std::chrono::milliseconds (0)) == std::future_status::ready)
            return future_.get ();

        progress_ ();
        if (future_.wait_for (std::chrono::milliseconds (1)) == std::future_status::ready)
            return future_.get ();
    }

    assert (false && "future did not become ready before timeout");
    return future_.get ();
}

inline bool
parse_tcp_endpoint (const std::string &endpoint_, std::string &host_, std::string &port_)
{
    char proto[8] = {0};
    char host[64] = {0};
    int port = 0;
    if (std::sscanf (endpoint_.c_str (), "%7[^:]://%63[^:]:%d", proto, host, &port) != 3)
        return false;

    if (std::strcmp (proto, "tcp") != 0 || port <= 0 || port > 65535)
        return false;

    host_ = host;
    std::ostringstream stream;
    stream << port;
    port_ = stream.str ();
    return true;
}

class raw_tcp_client_t
{
  public:
    explicit raw_tcp_client_t (const std::string &endpoint_) : _socket (_io)
    {
        std::string host;
        std::string port;
        assert (parse_tcp_endpoint (endpoint_, host, port));

        boost::asio::ip::tcp::resolver resolver (_io);
        boost::system::error_code ec;
        const boost::asio::ip::tcp::resolver::results_type endpoints =
          resolver.resolve (host, port, ec);
        assert (!ec);
        boost::asio::connect (_socket, endpoints, ec);
        assert (!ec);
    }

    ~raw_tcp_client_t ()
    {
        boost::system::error_code ec;
        _socket.close (ec);
    }

    void send_all (const char *data_, size_t size_)
    {
        boost::system::error_code ec;
        const size_t written = boost::asio::write (_socket, boost::asio::buffer (data_, size_), ec);
        assert (!ec);
        assert (written == size_);
    }

    int recv_exact (char *buffer_, size_t size_)
    {
        boost::system::error_code ec;
        const size_t received =
          boost::asio::read (_socket, boost::asio::buffer (buffer_, size_), ec);
        assert (!ec);
        assert (received == size_);
        return static_cast<int> (received);
    }

    void close ()
    {
        boost::system::error_code ec;
        _socket.close (ec);
    }

  private:
    boost::asio::io_context _io;
    boost::asio::ip::tcp::socket _socket;
};

//  RouteMesh service helpers used to live here. Core 11 removed the service
//  contracts from the binding headers, so these samples use the raw socket and
//  socket monitor public API only. The Actor, Spot, session binding, timer and
//  Logical Multicast scenarios are owned by the C++ Framework samples under
//  framework/languages/cpp/samples.

//  Convenience: wrap a single text payload as a one-part message vector.
inline std::vector<zlink::message_t> make_parts (const std::string &text_)
{
    std::vector<zlink::message_t> parts;
    parts.push_back (zlink::message_t::from (text_));
    return parts;
}

} // namespace detail

#endif
