/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <zlink/json.hpp>
#include <cstdint>
#include <limits>
struct Dto
{
    static constexpr const char *packet_name = "diagnostic.boundaries";
    enum class Reason
    {
        Ready,
        Closed
    };
    Reason state = Reason::Closed;
    std::int64_t signed64 = std::numeric_limits<std::int64_t>::min ();
    std::uint64_t unsigned64 = std::numeric_limits<std::uint64_t>::max ();
    std::int32_t int32 = std::numeric_limits<std::int32_t>::min ();
};
template <> struct zlink::json::dto_traits<Dto>
{
    static constexpr auto fields = std::tuple{zlink::json::field ("state", &Dto::state),
                                              zlink::json::field ("signed64", &Dto::signed64),
                                              zlink::json::field ("unsigned64", &Dto::unsigned64),
                                              zlink::json::field ("int32", &Dto::int32)};
};
template <> struct zlink::json::enum_traits<Dto::Reason>
{
    static constexpr auto values =
      std::array{zlink::json::enum_value (Dto::Reason::Ready, "Ready"),
                 zlink::json::enum_value (Dto::Reason::Closed, "Closed")};
};
#include <zlink/stream_connector/contracts/connector.hpp>
#ifdef WITH_EXTENSION
#include <zlink/framework/codecs/json_stream_connector.hpp>
#endif
#include <boost/asio.hpp>
#include <array>
#include <cassert>
#include <zlink/json_profile.hpp>
#include <iostream>
#include <thread>
namespace sc = zlink::stream_connector;
using tcp = boost::asio::ip::tcp;
int main ()
{
    boost::asio::io_context io;
    tcp::acceptor acceptor (io, {boost::asio::ip::address_v4::loopback (), 0});
    constexpr std::array invalid_fields{"state", "signed64", "unsigned64"};
    constexpr int request_count = 3 + invalid_fields.size ();
    std::thread peer ([&] {
        tcp::socket socket (io);
        acceptor.accept (socket);
        for (int i = 0; i < request_count; ++i) {
            std::array<std::uint8_t, 6> prefix{};
            boost::asio::read (socket, boost::asio::buffer (prefix));
            auto hs = (prefix[0] << 8) | prefix[1];
            auto ps = (std::uint32_t (prefix[2]) << 24) | (std::uint32_t (prefix[3]) << 16)
                      | (prefix[4] << 8) | prefix[5];
            std::vector<std::uint8_t> request (hs + ps);
            boost::asio::read (socket, boost::asio::buffer (request));
            std::cout << "wire[" << i << "]=" << std::string (request.begin () + hs, request.end ())
                      << '\n';
            const auto actual =
              nlohmann::json::parse (std::string (request.begin () + hs, request.end ()));
            assert (actual == zlink::detail::json_profile::encode_value (Dto{}));
            if (i == 0)
                continue;
            std::string payload (request.begin () + hs, request.end ());
            if (i == 2)
                payload =
                  R"({"state":"Closed","signed64":"-9223372036854775808","unsigned64":"18446744073709551615","int32":-2147483648})";
            if (i >= 3) {
                auto invalid = actual;
                if (i == 3)
                    invalid[invalid_fields[i - 3]] = 99;
                else if (i == 4)
                    invalid[invalid_fields[i - 3]] = std::numeric_limits<std::int64_t>::max ();
                else
                    invalid[invalid_fields[i - 3]] = std::numeric_limits<std::uint64_t>::max ();
                payload = invalid.dump ();
            }
            std::vector<std::uint8_t> header{0xF2, 3, 1, 1};
            header.insert (header.end (), request.begin () + 4, request.begin () + 12);
            header.push_back (0);
            std::vector<std::uint8_t> reply{
              0, std::uint8_t (header.size ()),       0,
              0, std::uint8_t (payload.size () >> 8), std::uint8_t (payload.size ())};
            reply.insert (reply.end (), header.begin (), header.end ());
            reply.insert (reply.end (), payload.begin (), payload.end ());
            boost::asio::write (socket, boost::asio::buffer (reply));
        }
        boost::system::error_code error;
        std::array<char, 64> drained{};
        while (!error)
            socket.read_some (boost::asio::buffer (drained), error);
    });
    sc::connector_options_t options;
    options.endpoint = "tcp://127.0.0.1:" + std::to_string (acceptor.local_endpoint ().port ());
    options.heartbeat.enabled = false;
    options.reconnect.enabled = false;
    auto connector = sc::connector_factory_t::create (options);
    auto connected = connector.connect ();
    if (!connected) {
        std::cerr << "connect failed\n";
        acceptor.close ();
        peer.join ();
        return 1;
    }
    connector.send (Dto{}).submit ();
    std::cout << "typed_send=submitted\n";
    for (int i = 1; i < request_count; ++i) {
        auto result = connector.request (Dto{}).submit<Dto> ();
        assert (bool (result) == (i < 3));
        if (result) {
            assert (result.value ().state == Dto::Reason::Closed);
            assert (result.value ().signed64 == Dto{}.signed64);
            assert (result.value ().unsigned64 == Dto{}.unsigned64);
        } else {
            assert (result.error_code () == sc::error_code_t::frame_decode_failed);
            std::cout << "invalid_field=" << invalid_fields[i - 3] << ' ';
        }
        std::cout << "typed_request[" << i << "]=" << (result ? "accepted" : "rejected");
        if (result)
            std::cout << " state=" << static_cast<int> (result.value ().state)
                      << " signed64=" << result.value ().signed64
                      << " unsigned64=" << result.value ().unsigned64;
        else
            std::cout << " code=" << int (*result.error_code ());
        std::cout << '\n';
    }
    connector.close ();
    peer.join ();
}