/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_TESTS_CONTRACT_SUPPORT_HPP_INCLUDED
#define ZLINK_CPP_TESTS_CONTRACT_SUPPORT_HPP_INCLUDED

#include <zlink.hpp>

#include <boost/asio.hpp>

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <future>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#if defined(ZLINK_HAVE_WINDOWS)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace zlink_cpp_contract
{

inline std::string unique_name (const char *base_)
{
    static unsigned counter = 0;
    std::ostringstream stream;
    stream << (base_ ? base_ : "cpp-contract") << "-" << ++counter;
    return stream.str ();
}

inline std::string unique_inproc (const char *base_)
{
    return std::string ("inproc://") + unique_name (base_);
}

inline std::string unique_ipc (const char *base_)
{
    static unsigned counter = 0;
    const unsigned pid =
#if defined(ZLINK_HAVE_WINDOWS)
      static_cast<unsigned> (_getpid ());
#else
      static_cast<unsigned> (getpid ());
#endif
    std::ostringstream stream;
    stream << "ipc:///tmp/" << (base_ ? base_ : "cpp-contract") << "-" << pid << "-" << ++counter
           << ".ipc";
    return stream.str ();
}

inline std::string unique_tcp (const char *base_)
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor acceptor (io);
    acceptor.open (boost::asio::ip::tcp::v4 ());
    acceptor.bind (boost::asio::ip::tcp::endpoint (boost::asio::ip::address_v4::loopback (), 0));
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

inline std::vector<unsigned char> encode_stream_packet_frame (const std::string &payload_)
{
    std::vector<unsigned char> frame (6u + payload_.size (), 0u);
    const uint32_t body_size = static_cast<uint32_t> (payload_.size ());
    frame[2] = static_cast<unsigned char> ((body_size >> 24) & 0xFFu);
    frame[3] = static_cast<unsigned char> ((body_size >> 16) & 0xFFu);
    frame[4] = static_cast<unsigned char> ((body_size >> 8) & 0xFFu);
    frame[5] = static_cast<unsigned char> (body_size & 0xFFu);
    if (!payload_.empty ())
        std::memcpy (frame.data () + 6u, payload_.data (), payload_.size ());
    return frame;
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

inline bool wait_stream_connected (zlink::socket_monitor_t &monitor_, int timeout_ms_ = 2000)
{
    return wait_for_socket_monitor_event (
      monitor_, static_cast<uint64_t> (zlink::monitor_event::accepted), timeout_ms_);
}

template <typename T> inline T wait_future (std::future<T> &future_, int timeout_ms_)
{
    assert (future_.wait_for (std::chrono::milliseconds (timeout_ms_))
            == std::future_status::ready);
    return future_.get ();
}

inline bool parse_tcp_endpoint (const std::string &endpoint_,
                                std::string &host_,
                                std::string &port_)
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

    size_t drain_available ()
    {
        boost::system::error_code ec;
        const size_t available = _socket.available (ec);
        if (ec || available == 0)
            return 0;
        std::vector<char> buffer (available);
        const size_t received = _socket.read_some (boost::asio::buffer (buffer), ec);
        assert (!ec || ec == boost::asio::error::would_block);
        return received;
    }

  private:
    boost::asio::io_context _io;
    boost::asio::ip::tcp::socket _socket;
};

} // namespace zlink_cpp_contract

#endif
