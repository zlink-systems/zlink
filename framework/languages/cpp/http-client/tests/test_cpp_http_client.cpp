/* SPDX-License-Identifier: Apache-2.0 */

#include <zlink/http_client.hpp>

#include <boost/asio/ip/tcp.hpp>
#ifdef ZLINK_HTTP_CLIENT_TEST_WITH_OPENSSL
#include <boost/asio/ssl/stream.hpp>
#endif
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/zlib/deflate_stream.hpp>
#include <boost/beast/zlib/error.hpp>
#ifdef ZLINK_HTTP_CLIENT_TEST_WITH_OPENSSL
#include <boost/beast/ssl.hpp>
#endif

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

static_assert (std::is_same_v<
               decltype (std::declval<zlink::http_client::server_request_builder_t> ().submit ()),
               zlink::framework::task_t<void>>);

struct create_game_request_t
{
    std::string name;
};

struct create_game_reply_t
{
    std::string method;
    std::string name;
};

struct header_echo_reply_t
{
    std::string defaultHeader;
    std::string overrideHeader;
};

class queued_execute_scheduler_t final : public zlink::http_client::coroutine_execute_scheduler_t
{
  public:
    void execute (std::function<void ()> work) override
    {
        const std::lock_guard lock (_mutex);
        ++calls;
        _work.push_back (std::move (work));
    }

    bool run_one ()
    {
        std::function<void ()> work;
        {
            const std::lock_guard lock (_mutex);
            if (_work.empty ()) {
                return false;
            }
            work = std::move (_work.front ());
            _work.pop_front ();
        }
        work ();
        return true;
    }

    std::size_t pending () const
    {
        const std::lock_guard lock (_mutex);
        return _work.size ();
    }

    std::atomic_int calls{0};

  private:
    mutable std::mutex _mutex;
    std::deque<std::function<void ()>> _work;
};

class rejecting_execute_scheduler_t final : public zlink::http_client::coroutine_execute_scheduler_t
{
  public:
    void execute (std::function<void ()>) override { throw std::runtime_error ("closed queue"); }
};

class queued_resume_scheduler_t final : public zlink::http_client::coroutine_resume_scheduler_t
{
  public:
    void resume (std::function<void ()> continuation) override
    {
        const std::lock_guard lock (_mutex);
        ++calls;
        _work.push_back (std::move (continuation));
    }

    bool run_one ()
    {
        std::function<void ()> work;
        {
            const std::lock_guard lock (_mutex);
            if (_work.empty ()) {
                return false;
            }
            work = std::move (_work.front ());
            _work.pop_front ();
        }
        last_resume_thread = std::this_thread::get_id ();
        work ();
        return true;
    }

    void drain ()
    {
        while (run_one ()) {
        }
    }

    std::size_t pending () const
    {
        const std::lock_guard lock (_mutex);
        return _work.size ();
    }

    std::atomic_int calls{0};
    std::thread::id last_resume_thread{};

  private:
    mutable std::mutex _mutex;
    std::deque<std::function<void ()>> _work;
};

class recording_execution_turn_t final : public zlink::http_client::execution_turn_t
{
  public:
    zlink::framework::detail::task_scheduler_t prepare (bool release_turn) override
    {
        ++prepare_calls;
        last_release.store (release_turn);
        return [this] (std::function<void ()> continuation) {
            ++resume_calls;
            continuation ();
        };
    }

    zlink::framework::detail::task_scheduler_t callback_scheduler () override
    {
        ++callback_scheduler_calls;
        return [this] (std::function<void ()> continuation) {
            ++callback_resume_calls;
            continuation ();
        };
    }

    std::atomic_int prepare_calls{0};
    std::atomic_int resume_calls{0};
    std::atomic_int callback_scheduler_calls{0};
    std::atomic_int callback_resume_calls{0};
    std::atomic_bool last_release{false};
};

void to_json (nlohmann::json &json, const create_game_request_t &value)
{
    json = nlohmann::json{{"name", value.name}};
}

void from_json (const nlohmann::json &json, create_game_reply_t &value)
{
    value.method = json.at ("method").get<std::string> ();
    value.name = json.at ("name").get<std::string> ();
}

void from_json (const nlohmann::json &json, header_echo_reply_t &value)
{
    value.defaultHeader = json.at ("defaultHeader").get<std::string> ();
    value.overrideHeader = json.at ("overrideHeader").get<std::string> ();
}

std::string gzip_compress (const std::string &payload)
{
    beast::zlib::deflate_stream deflater;
    beast::zlib::z_params zs;
    zs.next_in = payload.data ();
    zs.avail_in = payload.size ();

    std::string out ("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff", 10);
    char chunk[16384];
    for (;;) {
        zs.next_out = chunk;
        zs.avail_out = sizeof chunk;
        beast::error_code ec;
        deflater.write (zs, beast::zlib::Flush::finish, ec);
        out.append (chunk, sizeof chunk - zs.avail_out);
        if (ec == beast::zlib::error::end_of_stream) {
            break;
        }
        if (ec) {
            throw std::runtime_error ("test gzip compression failed");
        }
    }
    out.append (8, '\0'); // CRC32 + ISIZE trailer is not validated by the client
    return out;
}

std::string big_download_body ()
{
    return std::string (200000, 'x') + "END";
}

http::response<http::string_body> make_response (const http::request<http::string_body> &request)
{
    const auto full_target = std::string (request.target ());
    const auto target = full_target.substr (0, full_target.find ('?'));
    http::response<http::string_body> response{http::status::ok, request.version ()};
    response.set (http::field::content_type, "application/json");

    if (target == "/echo-target") {
        response.body () = nlohmann::json{{"target", full_target}}.dump ();
        response.prepare_payload ();
        return response;
    }
    if (target == "/echo-content-type") {
        response.body () = nlohmann::json{
          {"method", std::string (request.method_string ())},
          {"contentType", std::string (request[http::field::content_type])},
          {"requestBody",
           request.body ()}}.dump ();
        response.prepare_payload ();
        return response;
    }
    if (target == "/echo-auth") {
        response.body () =
          nlohmann::json{{"authorization", std::string (request[http::field::authorization])}}
            .dump ();
        response.prepare_payload ();
        return response;
    }
    if (target == "/deflate") {
        response.set (http::field::content_encoding, "deflate");
        // zlib wrapper: 0x78 0x9C header + raw deflate + unvalidated trailer
        auto compressed = gzip_compress ("hello deflate payload");
        response.body () = std::string ("\x78\x9c") + compressed.substr (10);
        response.prepare_payload ();
        return response;
    }
    if (target == "/echo-cookie" || target == "/admin/echo-cookie"
        || target == "/administrator/echo-cookie") {
        response.body () =
          nlohmann::json{{"cookie", std::string (request[http::field::cookie])}}.dump ();
        response.prepare_payload ();
        return response;
    }
    if (target == "/set-cookie") {
        response.insert (http::field::set_cookie, "session=abc123; Path=/; Max-Age=300");
        response.insert (http::field::set_cookie, "theme=dark; Path=/games");
        response.body () = R"({"ok":true})";
        response.prepare_payload ();
        return response;
    }
    if (target == "/set-cookie-overlap") {
        response.insert (http::field::set_cookie, "scope=root; Path=/");
        response.insert (http::field::set_cookie, "scope=admin; Path=/admin");
        response.insert (http::field::set_cookie, "scope=games; Path=/games");
        response.body () = R"({"ok":true})";
        response.prepare_payload ();
        return response;
    }
    if (target == "/redirect-once") {
        response.result (http::status::found);
        response.set (http::field::location, "/games");
        response.body () = R"({"redirect":true})";
        response.prepare_payload ();
        return response;
    }
    if (target == "/redirect-absolute") {
        response.result (http::status::found);
        response.set (http::field::location,
                      "http://" + std::string (request[http::field::host]) + "/games");
        response.prepare_payload ();
        return response;
    }
    if (target == "/redirect-custom") {
        response.result (http::status::found);
        response.set (http::field::location, request["X-ZLink-Redirect-Location"]);
        response.prepare_payload ();
        return response;
    }
    if (target == "/redirect-loop") {
        response.result (http::status::found);
        response.set (http::field::location, "/redirect-loop");
        response.prepare_payload ();
        return response;
    }
    if (target == "/redirect-stream-307") {
        response.result (http::status::temporary_redirect);
        response.set (http::field::location, "/echo-content-type");
        response.prepare_payload ();
        return response;
    }
    if (target == "/redirect-stream-303") {
        response.result (http::status::see_other);
        response.set (http::field::location, "/echo-content-type");
        response.prepare_payload ();
        return response;
    }
    if (target == "/gzip") {
        response.set (http::field::content_encoding, "gzip");
        response.body () = gzip_compress ("hello gzip payload");
        response.prepare_payload ();
        return response;
    }
    if (target == "/gzip-large") {
        response.set (http::field::content_encoding, "gzip");
        response.body () = gzip_compress (std::string (200000, 'z'));
        response.prepare_payload ();
        return response;
    }
    if (target == "/big") {
        response.set (http::field::content_type, "application/octet-stream");
        response.body () = big_download_body ();
        response.prepare_payload ();
        return response;
    }

    if (target == "/missing") {
        response.result (http::status::not_found);
        response.body () = R"({"error":"missing"})";
    } else if (target == "/bad-request") {
        response.result (http::status::bad_request);
        response.body () = R"({"error":"bad request"})";
    } else if (target == "/server-error") {
        response.result (http::status::internal_server_error);
        response.body () = R"({"error":"server error"})";
    } else if (target == "/invalid-json") {
        response.body () = "{";
    } else if (target == "/slow" || target == "/flaky-slow") {
        std::this_thread::sleep_for (std::chrono::milliseconds (250));
        response.body () = R"({"method":"GET","name":"slow"})";
    } else if (target == "/headers") {
        response.body () =
          nlohmann::json{{"defaultHeader", std::string (request[http::field::from])},
                         {"overrideHeader", std::string (request["X-ZLink-Override"])}}
            .dump ();
    } else {
        const auto method = std::string (request.method_string ());
        std::string name = method;
        if (!request.body ().empty ()) {
            name = nlohmann::json::parse (request.body ()).at ("name").get<std::string> ();
        }
        response.body () = nlohmann::json{{"method", method}, {"name", name}}.dump ();
    }

    response.prepare_payload ();
    if (request.method () == http::verb::head) {
        const auto size = response.body ().size ();
        response.body ().clear ();
        response.content_length (size);
    }
    return response;
}

class loopback_http_server_t
{
  public:
    loopback_http_server_t () :
        _acceptor (_io, tcp::endpoint (asio::ip::make_address ("127.0.0.1"), 0))
    {
        _port = _acceptor.local_endpoint ().port ();
        _thread = std::thread ([this] { run (); });
    }

    ~loopback_http_server_t ()
    {
        _stop = true;
        beast::error_code ignored;
        _acceptor.close (ignored);
        try {
            tcp::socket wakeup (_io);
            wakeup.connect (tcp::endpoint (asio::ip::make_address ("127.0.0.1"), _port), ignored);
        }
        catch (...) {
        }
        if (_thread.joinable ()) {
            _thread.join ();
        }
        for (auto &worker : _workers) {
            if (worker.joinable ()) {
                worker.join ();
            }
        }
    }

    std::string base_url () const { return "http://127.0.0.1:" + std::to_string (_port); }
    int connections () const { return _connections; }

  private:
    void run ()
    {
        while (!_stop) {
            beast::error_code ec;
            tcp::socket socket (_io);
            _acceptor.accept (socket, ec);
            if (ec) {
                continue;
            }
            ++_connections;
            auto shared_socket = std::make_shared<tcp::socket> (std::move (socket));
            _workers.emplace_back ([this, shared_socket] { handle (*shared_socket); });
        }
    }

    void handle (tcp::socket &socket)
    {
        beast::flat_buffer buffer;
        for (;;) {
            http::request<http::string_body> request;
            beast::error_code ec;
            http::read (socket, buffer, request, ec);
            if (ec) {
                return;
            }

            if ((request.target () == "/flaky" || request.target () == "/flaky-slow")
                && ++_flaky_hits == 1) {
                socket.shutdown (tcp::socket::shutdown_both, ec); // drop without a response
                return;
            }

            auto response = make_response (request);
            http::write (socket, response, ec);
            if (ec || !response.keep_alive () || request.need_eof ()) {
                socket.shutdown (tcp::socket::shutdown_send, ec);
                return;
            }
        }
    }

    asio::io_context _io;
    tcp::acceptor _acceptor;
    std::uint16_t _port = 0;
    std::atomic_bool _stop{false};
    std::atomic<int> _flaky_hits{0};
    std::atomic<int> _connections{0};
    std::thread _thread;
    std::vector<std::thread> _workers;
};

class loopback_proxy_t
{
  public:
    explicit loopback_proxy_t (std::optional<std::string> required_authorization = std::nullopt) :
        _required_authorization (std::move (required_authorization)),
        _acceptor (_io, tcp::endpoint (asio::ip::make_address ("127.0.0.1"), 0))
    {
        _port = _acceptor.local_endpoint ().port ();
        _thread = std::thread ([this] { run (); });
    }

    ~loopback_proxy_t ()
    {
        _stop = true;
        beast::error_code ignored;
        _acceptor.close (ignored);
        try {
            tcp::socket wakeup (_io);
            wakeup.connect (tcp::endpoint (asio::ip::make_address ("127.0.0.1"), _port), ignored);
        }
        catch (...) {
        }
        if (_thread.joinable ()) {
            _thread.join ();
        }
        for (auto &worker : _workers) {
            if (worker.joinable ()) {
                worker.join ();
            }
        }
    }

    std::string url () const { return "http://127.0.0.1:" + std::to_string (_port); }
    int forwarded_requests () const { return _forwarded; }
    int connect_tunnels () const { return _tunnels; }
    int rejected_requests () const { return _rejected; }

  private:
    void run ()
    {
        while (!_stop) {
            beast::error_code ec;
            tcp::socket socket (_io);
            _acceptor.accept (socket, ec);
            if (ec) {
                continue;
            }
            auto shared_socket = std::make_shared<tcp::socket> (std::move (socket));
            _workers.emplace_back ([this, shared_socket] {
                try {
                    handle (std::move (*shared_socket));
                }
                catch (...) {
                }
            });
        }
    }

    static std::pair<std::string, std::string> split_authority (const std::string &authority)
    {
        const auto colon = authority.rfind (':');
        return {authority.substr (0, colon), authority.substr (colon + 1)};
    }

    void handle (tcp::socket client)
    {
        beast::flat_buffer buffer;
        http::request<http::string_body> request;
        beast::error_code ec;
        http::read (client, buffer, request, ec);
        if (ec) {
            return;
        }

        if (_required_authorization
            && std::string (request[http::field::proxy_authorization])
                 != *_required_authorization) {
            ++_rejected;
            http::response<http::string_body> denied{http::status::proxy_authentication_required,
                                                     request.version ()};
            denied.prepare_payload ();
            http::write (client, denied, ec);
            client.shutdown (tcp::socket::shutdown_send, ec);
            return;
        }

        if (request.method () == http::verb::connect) {
            tunnel (std::move (client), buffer, request);
            return;
        }

        // absolute-form: http://host:port/path
        auto target = std::string (request.target ());
        if (target.rfind ("http://", 0) != 0) {
            return;
        }
        target = target.substr (7);
        const auto slash = target.find ('/');
        const auto [host, port] = split_authority (target.substr (0, slash));
        const auto origin_form = target.substr (slash);

        tcp::socket upstream (_io);
        tcp::resolver resolver (_io);
        asio::connect (upstream, resolver.resolve (host, port));
        request.target (origin_form);
        http::write (upstream, request);

        beast::flat_buffer upstream_buffer;
        http::response<http::string_body> response;
        http::read (upstream, upstream_buffer, response);
        ++_forwarded;
        http::write (client, response, ec);
        client.shutdown (tcp::socket::shutdown_send, ec);
    }

    void tunnel (tcp::socket client,
                 beast::flat_buffer &buffer,
                 const http::request<http::string_body> &request)
    {
        const auto [host, port] = split_authority (std::string (request.target ()));
        tcp::socket upstream (_io);
        tcp::resolver resolver (_io);
        asio::connect (upstream, resolver.resolve (host, port));

        beast::error_code ec;
        asio::write (client, asio::buffer (std::string ("HTTP/1.1 200 OK\r\n\r\n")), ec);
        if (ec) {
            return;
        }
        ++_tunnels;

        // Bytes already read past the CONNECT header belong to the tunnel.
        if (buffer.size () > 0) {
            asio::write (upstream, buffer.data (), ec);
        }

        const auto pump = [] (tcp::socket &from, tcp::socket &to) {
            char chunk[8192];
            beast::error_code pump_ec;
            for (;;) {
                const auto count = from.read_some (asio::buffer (chunk), pump_ec);
                if (pump_ec) {
                    break;
                }
                asio::write (to, asio::buffer (chunk, count), pump_ec);
                if (pump_ec) {
                    break;
                }
            }
            beast::error_code ignored;
            to.shutdown (tcp::socket::shutdown_send, ignored);
        };

        std::thread downstream ([&] { pump (upstream, client); });
        pump (client, upstream);
        downstream.join ();
    }

    std::optional<std::string> _required_authorization;
    asio::io_context _io;
    tcp::acceptor _acceptor;
    std::uint16_t _port = 0;
    std::atomic_bool _stop{false};
    std::atomic<int> _forwarded{0};
    std::atomic<int> _tunnels{0};
    std::atomic<int> _rejected{0};
    std::thread _thread;
    std::vector<std::thread> _workers;
};

#ifdef ZLINK_HTTP_CLIENT_TEST_WITH_OPENSSL
class loopback_https_server_t
{
  public:
    explicit loopback_https_server_t (bool require_client_certificate = false) :
        _context (asio::ssl::context::tls_server),
        _acceptor (_io, tcp::endpoint (asio::ip::make_address ("127.0.0.1"), 0))
    {
        _context.use_certificate_chain_file (ZLINK_HTTP_CLIENT_TEST_CERT);
        _context.use_private_key_file (ZLINK_HTTP_CLIENT_TEST_KEY, asio::ssl::context::pem);
        if (require_client_certificate) {
            _context.load_verify_file (ZLINK_HTTP_CLIENT_TEST_CERT);
            _context.set_verify_mode (asio::ssl::verify_peer
                                      | asio::ssl::verify_fail_if_no_peer_cert);
        }
        _port = _acceptor.local_endpoint ().port ();
        _thread = std::thread ([this] { run (); });
    }

    ~loopback_https_server_t ()
    {
        _stop = true;
        beast::error_code ignored;
        _acceptor.close (ignored);
        try {
            tcp::socket wakeup (_io);
            wakeup.connect (tcp::endpoint (asio::ip::make_address ("127.0.0.1"), _port), ignored);
        }
        catch (...) {
        }
        if (_thread.joinable ()) {
            _thread.join ();
        }
        for (auto &worker : _workers) {
            if (worker.joinable ()) {
                worker.join ();
            }
        }
    }

    std::string base_url () const { return "https://localhost:" + std::to_string (_port); }

    std::string mismatched_base_url () const
    {
        return "https://127.0.0.1:" + std::to_string (_port);
    }

  private:
    void run ()
    {
        while (!_stop) {
            beast::error_code ec;
            tcp::socket socket (_io);
            _acceptor.accept (socket, ec);
            if (ec) {
                continue;
            }
            auto shared_socket = std::make_shared<tcp::socket> (std::move (socket));
            _workers.emplace_back ([this, shared_socket] { handle (*shared_socket); });
        }
    }

    void handle (tcp::socket &socket)
    {
        asio::ssl::stream<tcp::socket &> stream (socket, _context);
        beast::error_code ec;
        stream.handshake (asio::ssl::stream_base::server, ec);
        if (ec) {
            return;
        }

        beast::flat_buffer buffer;
        for (;;) {
            http::request<http::string_body> request;
            http::read (stream, buffer, request, ec);
            if (ec) {
                return;
            }

            auto response = make_response (request);
            http::write (stream, response, ec);
            if (ec || !response.keep_alive () || request.need_eof ()) {
                stream.shutdown (ec);
                return;
            }
        }
    }

    asio::io_context _io;
    asio::ssl::context _context;
    tcp::acceptor _acceptor;
    std::uint16_t _port = 0;
    std::atomic_bool _stop{false};
    std::thread _thread;
    std::vector<std::thread> _workers;
};
#endif

zlink::http_client::client_t
make_json_client (std::string base_url,
                  std::chrono::milliseconds timeout = std::chrono::milliseconds (500),
                  std::optional<std::string> trust_certificate_file = std::nullopt)
{
    auto builder = zlink::http_client::client_t::create ()
                     .base_url (std::move (base_url))
                     .timeout (timeout);
    if (trust_certificate_file) {
        builder.trust_certificate_file (std::move (*trust_certificate_file));
    }
    return builder.build ();
}

zlink::framework::task_t<zlink::http_client::http_response_t<create_game_reply_t>>
create_game_with_coroutine_submit (const zlink::http_client::client_t &client)
{
    auto response = co_await client.post ("/games")
                      .body (create_game_request_t{.name = "coawait"})
                      .submit<create_game_reply_t> ();
    co_return response;
}

template <typename TAction>
bool throws_protocol_error (TAction &&action, const std::string &message_part)
{
    try {
        action ();
    }
    catch (const zlink::framework::framework_exception_t &error) {
        return error.kind () == zlink::framework::framework_error_kind_t::protocol_error
               && std::string (error.what ()).find (message_part) != std::string::npos;
    }
    return false;
}

} // namespace

TEST (ZLinkHttpClient, ValidatesFluentInputAsProtocolErrors)
{
    EXPECT_TRUE (throws_protocol_error (
      [] { (void) zlink::http_client::client_t::create ().build (); }, "base_url"));

    EXPECT_TRUE (throws_protocol_error (
      [] { (void) zlink::http_client::client_t::create ().base_url ("ftp://host").build (); },
      "http:// or https://"));

    EXPECT_TRUE (throws_protocol_error (
      [] {
          (void) zlink::http_client::client_t::create ()
            .base_url ("http://127.0.0.1")
            .timeout (std::chrono::milliseconds (0));
      },
      "timeout"));

    EXPECT_TRUE (throws_protocol_error (
      [] {
          (void) zlink::http_client::client_t::create ()
            .base_url ("http://127.0.0.1")
            .max_response_body_size (0);
      },
      "response body size"));

    EXPECT_TRUE (throws_protocol_error (
      [] {
          (void) zlink::http_client::client_t::create ()
            .base_url ("http://127.0.0.1")
            .default_header (" ", "value");
      },
      "header name"));

    EXPECT_TRUE (throws_protocol_error (
      [] {
          (void) zlink::http_client::client_t::create ()
            .base_url ("http://127.0.0.1")
            .trust_certificate_file (" ");
      },
      "trust certificate"));

    auto client =
      zlink::http_client::client_t::create ().base_url ("http://127.0.0.1").build ();
    EXPECT_TRUE (
      throws_protocol_error ([&client] { (void) client.get ("missing-leading-slash"); }, "path"));

    EXPECT_TRUE (throws_protocol_error (
      [&client] { (void) client.get ("/games").header (" ", "value"); }, "header name"));
}

TEST (ZLinkHttpClient, ContractBuilderSubmitsTypedJsonRequests)
{
    loopback_http_server_t server;
    auto client = make_json_client (server.base_url ());

    auto result = client.post ("/games")
                    .body (create_game_request_t{.name = "match-1"})
                    .submit<create_game_reply_t> ()
                    .result ();

    ASSERT_TRUE (result) << result.error ()->what ();
    EXPECT_EQ (result.value ().status, 200);
    EXPECT_EQ (result.value ().body.method, "POST");
    EXPECT_EQ (result.value ().body.name, "match-1");
}

TEST (ZLinkHttpClient, BuilderRequestShortcutOmitsExplicitBuild)
{
    loopback_http_server_t server;

    // The builder is a temporary and `build()` is never called explicitly.
    // The request must keep the on-demand client (and its runtime) alive for
    // the whole request, otherwise this would be a use-after-free.
    auto result = zlink::http_client::client_t::create ()
                    .base_url (server.base_url ())
                    .timeout (std::chrono::milliseconds (500))
                    .post ("/games")
                    .body (create_game_request_t{.name = "shorthand"})
                    .submit<create_game_reply_t> ()
                    .result ();

    ASSERT_TRUE (result) << result.error ()->what ();
    EXPECT_EQ (result.value ().status, 200);
    EXPECT_EQ (result.value ().body.method, "POST");
    EXPECT_EQ (result.value ().body.name, "shorthand");
}

TEST (ZLinkHttpClient, AsyncReturnsTypedResponse)
{
    loopback_http_server_t server;
    auto client = make_json_client (server.base_url ());

    auto response = client.post ("/games")
                      .body (create_game_request_t{.name = "async"})
                      .submit<create_game_reply_t> ()
                      .result ();

    ASSERT_TRUE (response);
    EXPECT_EQ (response.value ().body.method, "POST");
    EXPECT_EQ (response.value ().body.name, "async");
}

TEST (ZLinkHttpClient, AsyncMapsFailureStatus)
{
    loopback_http_server_t server;
    auto client = make_json_client (server.base_url ());

    const auto response = client.get ("/bad-request").submit<create_game_reply_t> ().result ();
    ASSERT_FALSE (response);
    EXPECT_EQ (response.error_kind (),
               zlink::framework::framework_error_kind_t::internal_failure);
}

TEST (ZLinkHttpClient, SupportsCoroutineSubmit)
{
    loopback_http_server_t server;
    auto client = make_json_client (server.base_url ());

    auto result = create_game_with_coroutine_submit (client).result ();

    ASSERT_TRUE (result) << result.error ()->what ();
    EXPECT_EQ (result.value ().status, 200);
    EXPECT_EQ (result.value ().body.method, "POST");
    EXPECT_EQ (result.value ().body.name, "coawait");
}

TEST (ZLinkHttpClient, CoroutineClientDoesNotBlockCallerWhileResponseIsPending)
{
    loopback_http_server_t server;
    auto client = zlink::http_client::client_t::create (server.base_url ())
                    .timeout (500ms)
                    .coroutines ()
                    .build ();

    const auto started = std::chrono::steady_clock::now ();
    auto task = client.get ("/slow").submit<create_game_reply_t> ();
    const auto submit_elapsed = std::chrono::steady_clock::now () - started;

    EXPECT_LT (submit_elapsed, 100ms);
    auto result = task.result ();
    ASSERT_TRUE (result) << result.error ()->what ();
    EXPECT_EQ (result.value ().body.name, "slow");
}

TEST (ZLinkHttpClient, RejectsNullCoroutineSchedulers)
{
    EXPECT_TRUE (throws_protocol_error (
      [] {
          std::shared_ptr<zlink::http_client::coroutine_resume_scheduler_t> resume;
          (void) zlink::http_client::client_t::create ("http://127.0.0.1:18080")
            .coroutines (resume);
      },
      "resume scheduler"));

    EXPECT_TRUE (throws_protocol_error (
      [] {
          std::shared_ptr<zlink::http_client::coroutine_execute_scheduler_t> execute;
          auto resume = std::make_shared<queued_resume_scheduler_t> ();
          (void) zlink::http_client::client_t::create ("http://127.0.0.1:18080")
            .coroutines (execute, resume);
      },
      "execute and resume schedulers"));

    EXPECT_TRUE (throws_protocol_error (
      [] {
          (void) zlink::http_client::framework_resume_scheduler_t (
            zlink::http_client::framework_resume_scheduler_t::post_t{});
      },
      "post function"));
}

TEST (ZLinkHttpClient, CustomExecuteAndResumeSchedulersControlCoroutineResume)
{
    loopback_http_server_t server;
    auto execute = std::make_shared<queued_execute_scheduler_t> ();
    auto resume = std::make_shared<queued_resume_scheduler_t> ();
    auto client = zlink::http_client::client_t::create (server.base_url ())
                    .timeout (500ms)
                    .coroutines (execute, resume)
                    .build ();

    auto task = create_game_with_coroutine_submit (client);
    EXPECT_EQ (execute->calls.load (), 1);
    EXPECT_EQ (execute->pending (), 1U);
    EXPECT_FALSE (task.await_ready ());

    ASSERT_TRUE (execute->run_one ());
    EXPECT_EQ (resume->pending (), 1U);
    EXPECT_FALSE (task.await_ready ());

    ASSERT_TRUE (resume->run_one ());
    EXPECT_EQ (resume->last_resume_thread, std::this_thread::get_id ());
    auto result = task.result ();
    ASSERT_TRUE (result) << result.error ()->what ();
    EXPECT_EQ (result.value ().body.name, "coawait");
    EXPECT_EQ (resume->calls.load (), 1);
}

TEST (ZLinkHttpClient, CustomResumeSchedulerControlsCallbackExecution)
{
    loopback_http_server_t server;
    auto resume = std::make_shared<queued_resume_scheduler_t> ();
    auto client = zlink::http_client::client_t::create (server.base_url ())
                    .timeout (500ms)
                    .coroutines (resume)
                    .build ();

    bool callback_called = false;
    std::thread::id callback_thread;
    client.post ("/games")
      .body (create_game_request_t{.name = "callback-scheduler"})
      .submit<create_game_reply_t> ([&] (const auto &result) {
          callback_called = true;
          callback_thread = std::this_thread::get_id ();
          ASSERT_TRUE (result);
          EXPECT_EQ (result.value ().body.name, "callback-scheduler");
      });

    for (int attempt = 0; attempt < 100 && !callback_called; ++attempt) {
        resume->drain ();
        std::this_thread::sleep_for (5ms);
    }

    ASSERT_TRUE (callback_called);
    EXPECT_EQ (callback_thread, std::this_thread::get_id ());
    EXPECT_GE (resume->calls.load (), 1);
}

TEST (ZLinkHttpClient, FrameworkResumeSchedulerAdapterUsesProvidedQueue)
{
    loopback_http_server_t server;
    std::deque<std::function<void ()>> framework_queue;
    auto resume = std::make_shared<zlink::http_client::framework_resume_scheduler_t> (
      [&framework_queue] (std::function<void ()> continuation) {
          framework_queue.push_back (std::move (continuation));
      });
    auto client = zlink::http_client::client_t::create (server.base_url ())
                    .timeout (500ms)
                    .coroutines (resume)
                    .build ();

    auto task = create_game_with_coroutine_submit (client);
    for (int attempt = 0; attempt < 100 && !task.await_ready (); ++attempt) {
        while (!framework_queue.empty ()) {
            auto continuation = std::move (framework_queue.front ());
            framework_queue.pop_front ();
            continuation ();
        }
        std::this_thread::sleep_for (5ms);
    }

    ASSERT_TRUE (task.await_ready ());
    auto result = task.result ();
    ASSERT_TRUE (result) << result.error ()->what ();
    EXPECT_EQ (result.value ().body.name, "coawait");
}

TEST (ZLinkHttpClient, ExecuteSchedulerRejectionCompletesTaskAsClosed)
{
    auto execute = std::make_shared<rejecting_execute_scheduler_t> ();
    auto resume = std::make_shared<queued_resume_scheduler_t> ();
    auto client = zlink::http_client::client_t::create ("http://127.0.0.1:1")
                    .coroutines (execute, resume)
                    .build ();

    const auto result = client.get ("/games").submit_raw ().result ();

    ASSERT_FALSE (result);
    ASSERT_NE (result.error (), nullptr);
    EXPECT_EQ (zlink::framework::detail::boundary_state (*result.error ()),
               zlink::framework::detail::boundary_error_t::closed);
}

TEST (ZLinkHttpClient, QueueTimeoutCompletesBeforeStartingHttpExchange)
{
    loopback_http_server_t server;
    auto execute = std::make_shared<queued_execute_scheduler_t> ();
    auto resume = std::make_shared<queued_resume_scheduler_t> ();
    auto client = zlink::http_client::client_t::create (server.base_url ())
                    .timeout (10ms)
                    .coroutines (execute, resume)
                    .build ();

    auto task = client.get ("/games").submit_raw ();
    ASSERT_EQ (execute->pending (), 1U);
    std::this_thread::sleep_for (30ms);
    ASSERT_TRUE (execute->run_one ());
    resume->drain ();

    const auto result = task.result ();
    ASSERT_FALSE (result);
    ASSERT_NE (result.error (), nullptr);
    EXPECT_EQ (zlink::framework::detail::boundary_state (*result.error ()),
               zlink::framework::detail::boundary_error_t::timed_out);
    EXPECT_EQ (server.connections (), 0);
}

TEST (ZLinkHttpClient, QueueDeadlineAppliesAcrossCoroutineRetries)
{
    /* /flaky-slow는 첫 요청을 응답 없이 끊고 그다음부터 250ms를 잔다. 첫 시도는 즉시 실패하고,
     * 재시도는 느린 응답에 걸려 최초 deadline을 넘겨 끝난다. 그러면 남은 예산이 없으므로 세
     * 번째 시도는 없어야 한다(retry(2)는 최대 3회 시도를 허용한다). 예산이 시도마다 갱신되면
     * 세 번째 시도가 열려 connection이 하나 더 생긴다 — 시도 횟수가 판별자다.
     *
     * timeout은 재시도 지연(full jitter 0~50ms)보다 커야 한다. 예산이 지연보다 작으면 두 번째
     * 시도가 시작되는지 자체가 지터에 좌우되어 결과가 흔들린다. */
    loopback_http_server_t server;
    auto client = zlink::http_client::client_t::create (server.base_url ())
                    .timeout (150ms)
                    .retry (2)
                    .coroutines ()
                    .build ();

    const auto started = std::chrono::steady_clock::now ();
    const auto result = client.get ("/flaky-slow").submit_raw ().result ();
    const auto elapsed = std::chrono::steady_clock::now () - started;

    ASSERT_FALSE (result);
    ASSERT_NE (result.error (), nullptr);
    EXPECT_EQ (zlink::framework::detail::boundary_state (*result.error ()),
               zlink::framework::detail::boundary_error_t::timed_out);
    EXPECT_EQ (server.connections (), 2);
    EXPECT_LT (elapsed, 450ms);
}

TEST (ZLinkHttpClient, ScheduledRequestOwnsTemporaryBuilderStateUntilCompletion)
{
    loopback_http_server_t server;
    auto execute = std::make_shared<queued_execute_scheduler_t> ();
    auto resume = std::make_shared<queued_resume_scheduler_t> ();

    auto task = zlink::http_client::client_t::create (server.base_url ())
                  .timeout (500ms)
                  .coroutines (execute, resume)
                  .post ("/games")
                  .body (create_game_request_t{.name = "temporary-builder"})
                  .submit<create_game_reply_t> ();

    ASSERT_EQ (execute->pending (), 1U);
    ASSERT_TRUE (execute->run_one ());
    ASSERT_TRUE (resume->run_one ());
    auto result = task.result ();
    ASSERT_TRUE (result) << result.error ()->what ();
    EXPECT_EQ (result.value ().body.name, "temporary-builder");
}

TEST (ZLinkHttpClient, SupportsCommonMethodsAndCallbackSubmit)
{
    loopback_http_server_t server;
    auto client = make_json_client (server.base_url ());

    EXPECT_EQ (client.get ("/games").fetch<create_game_reply_t> ().method,
               "GET");
    EXPECT_EQ (client.put ("/games")
                 .body (create_game_request_t{.name = "put"})
                 .submit<create_game_reply_t> ()
                 .result ()
                 .value ()
                 .body.method,
               "PUT");
    EXPECT_EQ (
      client.delete_ ("/games").fetch<create_game_reply_t> ().method,
      "DELETE");

    std::promise<zlink::framework::result_t<
      zlink::http_client::http_response_t<create_game_reply_t>>>
      callback_result;
    client.post ("/games")
      .body (create_game_request_t{.name = "callback"})
      .submit<create_game_reply_t> ([&callback_result] (const auto &result) {
          callback_result.set_value (result);
      });
    const auto callback = callback_result.get_future ().get ();
    ASSERT_TRUE (callback);
    EXPECT_EQ (callback.value ().body.name, "callback");

    std::promise<zlink::framework::result_t<
      zlink::http_client::http_response_t<create_game_reply_t>>>
      failure_callback_result;
    client.get ("/invalid-json")
      .submit<create_game_reply_t> ([&failure_callback_result] (const auto &result) {
          failure_callback_result.set_value (result);
      });
    const auto failure_callback = failure_callback_result.get_future ().get ();
    ASSERT_FALSE (failure_callback);
    EXPECT_EQ (failure_callback.error_kind (),
               zlink::framework::framework_error_kind_t::protocol_error);
}

TEST (ZLinkHttpClient, ServerClientDelegatesTurnPolicyToInjectedExecutionTurn)
{
    loopback_http_server_t server;
    auto turn = std::make_shared<recording_execution_turn_t> ();
    auto client = zlink::http_client::client_t::create (server.base_url ()).build_server (turn);

    const auto async_response =
      client.get ("/games").submit<create_game_reply_t> ().result ();
    ASSERT_TRUE (async_response);
    EXPECT_FALSE (turn->last_release.load ());

    const auto one_way = client.post ("/games").submit ().result ();
    ASSERT_TRUE (one_way);
    EXPECT_FALSE (turn->last_release.load ());

    const auto yield_response =
      client.get ("/games").yield<create_game_reply_t> ().result ();
    ASSERT_TRUE (yield_response);
    EXPECT_TRUE (turn->last_release.load ());
    EXPECT_EQ (turn->prepare_calls.load (), 3);
    EXPECT_EQ (turn->resume_calls.load (), 3);

    std::promise<zlink::framework::result_t<
      zlink::http_client::http_response_t<create_game_reply_t>>>
      callback_result;
    client.get ("/games").submit<create_game_reply_t> (
      [&callback_result] (const auto &result) { callback_result.set_value (result); });
    ASSERT_TRUE (callback_result.get_future ().get ());
    EXPECT_EQ (turn->callback_scheduler_calls.load (), 1);
    EXPECT_EQ (turn->callback_resume_calls.load (), 1);
}

TEST (ZLinkHttpClient, SendsDefaultHeadersAndRequestOverride)
{
    loopback_http_server_t server;
    auto client = zlink::http_client::client_t::create ()
                    .base_url (server.base_url ())
                    .default_header ("From", "default@example.test")
                    .default_header ("X-ZLink-Override", "default")
                    .build ();

    auto result = client.get ("/headers")
                    .header ("X-ZLink-Override", "request")
                    .submit<header_echo_reply_t> ()
                    .result ();

    ASSERT_TRUE (result) << result.error ()->what ();
    EXPECT_EQ (result.value ().body.defaultHeader, "default@example.test");
    EXPECT_EQ (result.value ().body.overrideHeader, "request");
}

TEST (ZLinkHttpClient, MapsStatusDecodeAndTimeoutFailures)
{
    loopback_http_server_t server;
    auto client = make_json_client (server.base_url (), std::chrono::milliseconds (50));

    const auto missing = client.get ("/missing").submit<create_game_reply_t> ().result ();
    ASSERT_FALSE (missing);
    EXPECT_EQ (missing.error_kind (), zlink::framework::framework_error_kind_t::internal_failure);

    const auto bad_request = client.get ("/bad-request").submit<create_game_reply_t> ().result ();
    ASSERT_FALSE (bad_request);
    EXPECT_EQ (bad_request.error_kind (), zlink::framework::framework_error_kind_t::internal_failure);

    const auto server_error = client.get ("/server-error").submit<create_game_reply_t> ().result ();
    ASSERT_FALSE (server_error);
    EXPECT_EQ (server_error.error_kind (),
               zlink::framework::framework_error_kind_t::internal_failure);

    const auto invalid_json = client.get ("/invalid-json").submit<create_game_reply_t> ().result ();
    ASSERT_FALSE (invalid_json);
    EXPECT_EQ (invalid_json.error_kind (),
               zlink::framework::framework_error_kind_t::protocol_error);

    const auto timeout = client.get ("/slow").submit_raw ().result ();
    ASSERT_FALSE (timeout);
    ASSERT_NE (timeout.error (), nullptr);
    EXPECT_EQ (zlink::framework::detail::boundary_state (*timeout.error ()),
               zlink::framework::detail::boundary_error_t::timed_out);
}

TEST (ZLinkHttpClient, SupportsPatchHeadAndOptionsMethods)
{
    loopback_http_server_t server;
    auto client = make_json_client (server.base_url ());

    EXPECT_EQ (
      client.patch ("/games").fetch<create_game_reply_t> ().method,
      "PATCH");
    EXPECT_EQ (
      client.options ("/games").fetch<create_game_reply_t> ().method,
      "OPTIONS");

    const auto head = client.head ("/games").submit_raw ().result ();
    ASSERT_TRUE (head) << head.error ()->what ();
    EXPECT_EQ (head.value ().status, 200);
    EXPECT_TRUE (head.value ().body.empty ());
}

TEST (ZLinkHttpClient, EncodesQueryParameters)
{
    loopback_http_server_t server;
    auto client = make_json_client (server.base_url ());

    const auto result = client.get ("/echo-target")
                          .query ("name", "hello world")
                          .query ("tag", "a&b")
                          .submit_raw ()
                          .result ();

    ASSERT_TRUE (result) << result.error ()->what ();
    const auto echoed = nlohmann::json::parse (result.value ().body);
    EXPECT_EQ (echoed.at ("target").get<std::string> (),
               "/echo-target?name=hello%20world&tag=a%26b");
}

TEST (ZLinkHttpClient, SendsRawBodyWithContentType)
{
    loopback_http_server_t server;
    auto client = make_json_client (server.base_url ());

    const auto result = client.post ("/echo-content-type")
                          .body ("<game name=\"match-1\"/>", "application/xml")
                          .submit_raw ()
                          .result ();

    ASSERT_TRUE (result) << result.error ()->what ();
    const auto echoed = nlohmann::json::parse (result.value ().body);
    EXPECT_EQ (echoed.at ("contentType").get<std::string> (), "application/xml");
    EXPECT_EQ (echoed.at ("requestBody").get<std::string> (), "<game name=\"match-1\"/>");
}

TEST (ZLinkHttpClient, SendsFormUrlencodedBody)
{
    loopback_http_server_t server;
    auto client = make_json_client (server.base_url ());

    const auto result = client.post ("/echo-content-type")
                          .form ("user", "kim lee")
                          .form ("role", "admin&ops")
                          .submit_raw ()
                          .result ();

    ASSERT_TRUE (result) << result.error ()->what ();
    const auto echoed = nlohmann::json::parse (result.value ().body);
    EXPECT_EQ (echoed.at ("contentType").get<std::string> (), "application/x-www-form-urlencoded");
    EXPECT_EQ (echoed.at ("requestBody").get<std::string> (), "user=kim%20lee&role=admin%26ops");
}

TEST (ZLinkHttpClient, SendsMultipartBody)
{
    loopback_http_server_t server;
    auto client = make_json_client (server.base_url ());

    const auto result = client.post ("/echo-content-type")
                          .multipart ("name", "avatar")
                          .multipart_file ("file", "avatar.png", "PNGDATA", "image/png")
                          .submit_raw ()
                          .result ();

    ASSERT_TRUE (result) << result.error ()->what ();
    const auto echoed = nlohmann::json::parse (result.value ().body);
    const auto content_type = echoed.at ("contentType").get<std::string> ();
    const auto body = echoed.at ("requestBody").get<std::string> ();
    EXPECT_EQ (content_type.rfind ("multipart/form-data; boundary=zlink-boundary-", 0), 0U);
    EXPECT_NE (body.find ("Content-Disposition: form-data; name=\"name\""), std::string::npos);
    EXPECT_NE (body.find ("Content-Disposition: form-data; name=\"file\"; "
                          "filename=\"avatar.png\""),
               std::string::npos);
    EXPECT_NE (body.find ("Content-Type: image/png"), std::string::npos);
    EXPECT_NE (body.find ("PNGDATA"), std::string::npos);
}

TEST (ZLinkHttpClient, RejectsMultipleBodySources)
{
    loopback_http_server_t server;
    auto client = make_json_client (server.base_url ());

    EXPECT_TRUE (throws_protocol_error (
      [&client] {
          (void) client.post ("/games")
            .body (create_game_request_t{.name = "match-1"})
            .form ("user", "kim")
            .submit_raw ();
      },
      "single body source"));
}

TEST (ZLinkHttpClient, FollowsRedirectsWhenEnabled)
{
    loopback_http_server_t server;

    auto following = zlink::http_client::client_t::create (server.base_url ())
                       .follow_redirects ()
                       .build ();
    const auto followed = following.get ("/redirect-once").submit<create_game_reply_t> ().result ();
    ASSERT_TRUE (followed) << followed.error ()->what ();
    EXPECT_EQ (followed.value ().status, 200);
    EXPECT_EQ (followed.value ().body.method, "GET");

    auto direct = make_json_client (server.base_url ());
    const auto raw = direct.get ("/redirect-once").submit_raw ().result ();
    ASSERT_TRUE (raw) << raw.error ()->what ();
    EXPECT_EQ (raw.value ().status, 302);
}

TEST (ZLinkHttpClient, FollowsAbsoluteRedirectLocations)
{
    loopback_http_server_t server;
    auto client = zlink::http_client::client_t::create (server.base_url ())
                    .follow_redirects ()
                    .build ();

    const auto result = client.get ("/redirect-absolute").submit<create_game_reply_t> ().result ();
    ASSERT_TRUE (result) << result.error ()->what ();
    EXPECT_EQ (result.value ().status, 200);
}

TEST (ZLinkHttpClient, RedirectStripsAuthorizationAcrossHosts)
{
    loopback_http_server_t origin;
    loopback_http_server_t redirected;
    const auto location = redirected.base_url () + "/echo-auth";

    auto default_auth_client = zlink::http_client::client_t::create (origin.base_url ())
                                 .bearer_token ("secret-token")
                                 .follow_redirects ()
                                 .build ();
    const auto default_auth = default_auth_client.get ("/redirect-custom")
                                .header ("X-ZLink-Redirect-Location", location)
                                .submit_raw ()
                                .result ();
    ASSERT_TRUE (default_auth) << default_auth.error ()->what ();
    EXPECT_EQ (nlohmann::json::parse (default_auth.value ().body).at ("authorization"), "");

    auto request_auth_client = zlink::http_client::client_t::create (origin.base_url ())
                                 .follow_redirects ()
                                 .build ();
    const auto request_auth = request_auth_client.get ("/redirect-custom")
                                .header ("X-ZLink-Redirect-Location", location)
                                .header ("authorization", "Bearer request-token")
                                .submit_raw ()
                                .result ();
    ASSERT_TRUE (request_auth) << request_auth.error ()->what ();
    EXPECT_EQ (nlohmann::json::parse (request_auth.value ().body).at ("authorization"), "");
}

TEST (ZLinkHttpClient, RedirectKeepsAuthorizationForSameOrigin)
{
    loopback_http_server_t server;
    const auto location = server.base_url () + "/echo-auth";
    auto client = zlink::http_client::client_t::create (server.base_url ())
                    .bearer_token ("same-origin-token")
                    .follow_redirects ()
                    .build ();

    const auto result = client.get ("/redirect-custom")
                          .header ("X-ZLink-Redirect-Location", location)
                          .submit_raw ()
                          .result ();

    ASSERT_TRUE (result) << result.error ()->what ();
    EXPECT_EQ (nlohmann::json::parse (result.value ().body).at ("authorization"),
               "Bearer same-origin-token");
}

TEST (ZLinkHttpClient, RedirectTransformsPostIntoGet)
{
    loopback_http_server_t server;
    auto client = zlink::http_client::client_t::create (server.base_url ())
                    .follow_redirects ()
                    .build ();

    const auto result = client.post ("/redirect-once")
                          .body (create_game_request_t{.name = "match-1"})
                          .submit<create_game_reply_t> ()
                          .result ();
    ASSERT_TRUE (result) << result.error ()->what ();
    EXPECT_EQ (result.value ().body.method, "GET");
}

TEST (ZLinkHttpClient, StopsAtTheRedirectLimit)
{
    loopback_http_server_t server;
    auto client = zlink::http_client::client_t::create (server.base_url ())
                    .follow_redirects (3)
                    .build ();

    const auto result = client.get ("/redirect-loop").submit_raw ().result ();
    ASSERT_FALSE (result);
    EXPECT_NE (std::string (result.error ()->what ()).find ("redirect limit"), std::string::npos);
}

TEST (ZLinkHttpClient, RetriesRetriableTransportFailures)
{
    loopback_http_server_t server;
    auto client =
      zlink::http_client::client_t::create (server.base_url ()).retry (2).build ();

    // The first /flaky connection is dropped without a response; the retry
    // must succeed against the recovered server.
    const auto result = client.get ("/flaky").submit<create_game_reply_t> ().result ();
    ASSERT_TRUE (result) << result.error ()->what ();
    EXPECT_EQ (result.value ().status, 200);
}

TEST (ZLinkHttpClient, DoesNotInternallyRetryNonIdempotentRequestsOnStaleConnection)
{
    loopback_http_server_t server;
    auto client = zlink::http_client::client_t::create (server.base_url ()).build ();

    ASSERT_TRUE (client.get ("/games").submit_raw ().result ());

    const auto result = client.post ("/flaky")
                          .body (create_game_request_t{"post-on-stale-connection"})
                          .submit_raw ()
                          .result ();

    ASSERT_FALSE (result);
    ASSERT_NE (result.error (), nullptr);
    EXPECT_EQ (result.error_kind (),
               zlink::framework::framework_error_kind_t::unavailable);
}

TEST (ZLinkHttpClient, StoresAndSendsCookies)
{
    loopback_http_server_t server;
    auto client =
      zlink::http_client::client_t::create (server.base_url ()).cookies ().build ();

    ASSERT_TRUE (client.get ("/set-cookie").submit_raw ().result ());

    const auto echoed = client.get ("/echo-cookie").submit_raw ().result ();
    ASSERT_TRUE (echoed) << echoed.error ()->what ();
    const auto cookie =
      nlohmann::json::parse (echoed.value ().body).at ("cookie").get<std::string> ();
    EXPECT_NE (cookie.find ("session=abc123"), std::string::npos);
    // theme is scoped to Path=/games and must not be sent for /echo-cookie.
    EXPECT_EQ (cookie.find ("theme=dark"), std::string::npos);

    auto without_jar = make_json_client (server.base_url ());
    const auto plain = without_jar.get ("/echo-cookie").submit_raw ().result ();
    ASSERT_TRUE (plain);
    EXPECT_EQ (nlohmann::json::parse (plain.value ().body).at ("cookie").get<std::string> (), "");
}

TEST (ZLinkHttpClient, MatchesCookiePathsWithSegmentBoundaries)
{
    loopback_http_server_t server;
    auto client =
      zlink::http_client::client_t::create (server.base_url ()).cookies ().build ();

    ASSERT_TRUE (client.get ("/set-cookie-overlap").submit_raw ().result ());

    const auto administrator = client.get ("/administrator/echo-cookie").submit_raw ().result ();
    ASSERT_TRUE (administrator) << administrator.error ()->what ();
    const auto administrator_cookie =
      nlohmann::json::parse (administrator.value ().body).at ("cookie").get<std::string> ();
    EXPECT_NE (administrator_cookie.find ("scope=root"), std::string::npos);
    EXPECT_EQ (administrator_cookie.find ("scope=admin"), std::string::npos);
    EXPECT_EQ (administrator_cookie.find ("scope=games"), std::string::npos);

    const auto admin = client.get ("/admin/echo-cookie").submit_raw ().result ();
    ASSERT_TRUE (admin) << admin.error ()->what ();
    const auto admin_cookie =
      nlohmann::json::parse (admin.value ().body).at ("cookie").get<std::string> ();
    EXPECT_NE (admin_cookie.find ("scope=root"), std::string::npos);
    EXPECT_NE (admin_cookie.find ("scope=admin"), std::string::npos);
    EXPECT_EQ (admin_cookie.find ("scope=games"), std::string::npos);
}

TEST (ZLinkHttpClient, RoutesPlainRequestsThroughHttpProxy)
{
    loopback_http_server_t server;
    loopback_proxy_t proxy;
    auto client = zlink::http_client::client_t::create (server.base_url ())
                    .proxy (proxy.url ())
                    .build ();

    const auto result = client.get ("/games").submit<create_game_reply_t> ().result ();
    ASSERT_TRUE (result) << result.error ()->what ();
    EXPECT_EQ (result.value ().status, 200);
    EXPECT_GT (proxy.forwarded_requests (), 0);
}

TEST (ZLinkHttpClient, DecompressesGzipResponses)
{
    loopback_http_server_t server;
    auto client =
      zlink::http_client::client_t::create (server.base_url ()).compression ().build ();

    const auto result = client.get ("/gzip").submit_raw ().result ();
    ASSERT_TRUE (result) << result.error ()->what ();
    EXPECT_EQ (result.value ().body, "hello gzip payload");
    EXPECT_EQ (result.value ().headers.count ("content-encoding"), 0U);
    EXPECT_EQ (result.value ().headers.count ("content-length"), 0U);
}

TEST (ZLinkHttpClient, RejectsDecompressedResponseAboveBodyLimit)
{
    loopback_http_server_t server;
    auto client = zlink::http_client::client_t::create (server.base_url ())
                    .compression ()
                    .max_response_body_size (1024)
                    .build ();

    const auto result = client.get ("/gzip-large").submit_raw ().result ();

    ASSERT_FALSE (result);
    EXPECT_EQ (result.error_kind (),
               zlink::framework::framework_error_kind_t::capacity_exceeded);
}

TEST (ZLinkHttpClient, DownloadStreamsResponseBody)
{
    loopback_http_server_t server;
    auto client = make_json_client (server.base_url (), std::chrono::milliseconds (2000));

    std::string received;
    int chunks = 0;
    const auto result = client.get ("/big")
                          .download ([&] (std::string_view chunk) {
                              received.append (chunk);
                              ++chunks;
                          })
                          .result ();

    ASSERT_TRUE (result) << result.error ()->what ();
    EXPECT_EQ (result.value ().status, 200);
    EXPECT_TRUE (result.value ().body.empty ());
    EXPECT_GT (chunks, 1);
    EXPECT_EQ (received, big_download_body ());
}

TEST (ZLinkHttpClient, RejectsBufferedResponseAboveBodyLimit)
{
    loopback_http_server_t server;
    auto client = zlink::http_client::client_t::create (server.base_url ())
                    .timeout (2000ms)
                    .max_response_body_size (32)
                    .build ();

    const auto result = client.get ("/big").submit_raw ().result ();

    ASSERT_FALSE (result);
    EXPECT_EQ (result.error_kind (),
               zlink::framework::framework_error_kind_t::capacity_exceeded);
}

TEST (ZLinkHttpClient, RejectsDownloadResponseAboveBodyLimit)
{
    loopback_http_server_t server;
    auto client = zlink::http_client::client_t::create (server.base_url ())
                    .timeout (2000ms)
                    .max_response_body_size (32)
                    .build ();

    std::string received;
    const auto result = client.get ("/big")
                          .download ([&] (std::string_view chunk) { received.append (chunk); })
                          .result ();

    ASSERT_FALSE (result);
    EXPECT_EQ (result.error_kind (),
               zlink::framework::framework_error_kind_t::capacity_exceeded);
    EXPECT_LT (received.size (), big_download_body ().size ());
}

TEST (ZLinkHttpClient, CoroutineDownloadSinkRunsOnExecuteSchedulerWorker)
{
    loopback_http_server_t server;
    auto client = zlink::http_client::client_t::create (server.base_url ())
                    .timeout (2000ms)
                    .coroutines ()
                    .build ();

    const auto caller_thread = std::this_thread::get_id ();
    std::mutex mutex;
    std::thread::id sink_thread;
    std::string received;
    const auto result = client.get ("/big")
                          .download ([&] (std::string_view chunk) {
                              const std::lock_guard lock (mutex);
                              sink_thread = std::this_thread::get_id ();
                              received.append (chunk);
                          })
                          .result ();

    ASSERT_TRUE (result) << result.error ()->what ();
    EXPECT_EQ (received, big_download_body ());
    EXPECT_NE (sink_thread, caller_thread);
}

TEST (ZLinkHttpClient, DownloadFollowsRedirectsWithoutLeakingIntermediateBodies)
{
    loopback_http_server_t server;
    auto client = zlink::http_client::client_t::create (server.base_url ())
                    .follow_redirects ()
                    .build ();

    std::string received;
    const auto result = client.get ("/redirect-once")
                          .download ([&] (std::string_view chunk) { received.append (chunk); })
                          .result ();

    ASSERT_TRUE (result) << result.error ()->what ();
    EXPECT_EQ (result.value ().status, 200);
    EXPECT_EQ (nlohmann::json::parse (received).at ("method").get<std::string> (), "GET");
}

TEST (ZLinkHttpClient, SendsBasicAuthAndBearerToken)
{
    loopback_http_server_t server;

    const auto basic = zlink::http_client::client_t::create (server.base_url ())
                         .basic_auth ("user", "pass")
                         .get ("/echo-auth")
                         .submit_raw ()
                         .result ();
    ASSERT_TRUE (basic) << basic.error ()->what ();
    EXPECT_EQ (nlohmann::json::parse (basic.value ().body).at ("authorization"),
               "Basic dXNlcjpwYXNz");

    const auto bearer = zlink::http_client::client_t::create (server.base_url ())
                          .bearer_token ("token-123")
                          .get ("/echo-auth")
                          .submit_raw ()
                          .result ();
    ASSERT_TRUE (bearer) << bearer.error ()->what ();
    EXPECT_EQ (nlohmann::json::parse (bearer.value ().body).at ("authorization"),
               "Bearer token-123");
}

TEST (ZLinkHttpClient, PerRequestTimeoutOverridesClientTimeout)
{
    loopback_http_server_t server;
    auto client = make_json_client (server.base_url (), std::chrono::milliseconds (2000));

    const auto timed_out =
      client.get ("/slow").timeout (std::chrono::milliseconds (100)).submit_raw ().result ();
    ASSERT_FALSE (timed_out);
    EXPECT_EQ (zlink::framework::detail::boundary_state (*timed_out.error ()),
               zlink::framework::detail::boundary_error_t::timed_out);

    // The same client without the override completes within its own timeout.
    const auto completed = client.get ("/slow").submit_raw ().result ();
    ASSERT_TRUE (completed) << completed.error ()->what ();
    EXPECT_EQ (completed.value ().status, 200);
}

TEST (ZLinkHttpClient, ReusesKeptAliveConnections)
{
    loopback_http_server_t server;
    auto client = make_json_client (server.base_url ());

    ASSERT_TRUE (client.get ("/games").submit_raw ().result ());
    ASSERT_TRUE (client.get ("/games").submit_raw ().result ());
    ASSERT_TRUE (client.get ("/games").submit_raw ().result ());

    EXPECT_EQ (server.connections (), 1);
}

TEST (ZLinkHttpClient, UploadsStreamedRequestBody)
{
    loopback_http_server_t server;
    auto client = make_json_client (server.base_url ());

    std::vector<std::string> chunks{"chunk-one|", "chunk-two|", "chunk-three"};
    std::size_t next = 0;
    const auto result = client.post ("/echo-content-type")
                          .body_stream (
                            [&] () -> std::optional<std::string> {
                                if (next >= chunks.size ()) {
                                    return std::nullopt;
                                }
                                return chunks[next++];
                            },
                            "application/octet-stream")
                          .submit_raw ()
                          .result ();

    ASSERT_TRUE (result) << result.error ()->what ();
    const auto echoed = nlohmann::json::parse (result.value ().body);
    EXPECT_EQ (echoed.at ("contentType").get<std::string> (), "application/octet-stream");
    EXPECT_EQ (echoed.at ("requestBody").get<std::string> (), "chunk-one|chunk-two|chunk-three");
}

TEST (ZLinkHttpClient, DropsConsumedStreamBodyOn307Redirect)
{
    loopback_http_server_t server;
    auto client = zlink::http_client::client_t::create (server.base_url ())
                    .follow_redirects ()
                    .build ();

    int calls = 0;
    const auto result = client.post ("/redirect-stream-307")
                          .body_stream (
                            [&] () -> std::optional<std::string> {
                                ++calls;
                                return calls == 1 ? std::optional<std::string> ("streamed")
                                                  : std::nullopt;
                            },
                            "application/octet-stream")
                          .submit_raw ()
                          .result ();

    ASSERT_TRUE (result) << result.error ()->what ();
    const auto echoed = nlohmann::json::parse (result.value ().body);
    EXPECT_EQ (echoed.at ("method"), "POST");
    EXPECT_EQ (echoed.at ("requestBody"), "");
    EXPECT_EQ (echoed.at ("contentType"), "");
    EXPECT_EQ (calls, 2);
}

TEST (ZLinkHttpClient, DropsConsumedStreamBodyOn303Redirect)
{
    loopback_http_server_t server;
    auto client = zlink::http_client::client_t::create (server.base_url ())
                    .follow_redirects ()
                    .build ();

    int calls = 0;
    const auto result = client.post ("/redirect-stream-303")
                          .body_stream (
                            [&] () -> std::optional<std::string> {
                                ++calls;
                                return calls == 1 ? std::optional<std::string> ("streamed")
                                                  : std::nullopt;
                            },
                            "application/octet-stream")
                          .submit_raw ()
                          .result ();

    ASSERT_TRUE (result) << result.error ()->what ();
    const auto echoed = nlohmann::json::parse (result.value ().body);
    EXPECT_EQ (echoed.at ("method"), "GET");
    EXPECT_EQ (echoed.at ("requestBody"), "");
    EXPECT_EQ (echoed.at ("contentType"), "");
    EXPECT_EQ (calls, 2);
}

TEST (ZLinkHttpClient, CoroutineBodyStreamProviderRunsOnExecuteSchedulerWorker)
{
    loopback_http_server_t server;
    auto client = zlink::http_client::client_t::create (server.base_url ())
                    .timeout (500ms)
                    .coroutines ()
                    .build ();

    const auto caller_thread = std::this_thread::get_id ();
    std::mutex mutex;
    std::thread::id provider_thread;
    int index = 0;
    const auto result = client.post ("/echo-content-type")
                          .body_stream (
                            [&] () -> std::optional<std::string> {
                                const std::lock_guard lock (mutex);
                                provider_thread = std::this_thread::get_id ();
                                if (index++ == 0) {
                                    return std::string ("chunked");
                                }
                                return std::nullopt;
                            },
                            "text/plain")
                          .submit_raw ()
                          .result ();

    ASSERT_TRUE (result) << result.error ()->what ();
    EXPECT_NE (provider_thread, caller_thread);
    EXPECT_NE (result.value ().body.find ("chunked"), std::string::npos);
}

TEST (ZLinkHttpClient, DecompressesDeflateResponses)
{
    loopback_http_server_t server;
    auto client =
      zlink::http_client::client_t::create (server.base_url ()).compression ().build ();

    const auto result = client.get ("/deflate").submit_raw ().result ();
    ASSERT_TRUE (result) << result.error ()->what ();
    EXPECT_EQ (result.value ().body, "hello deflate payload");
    EXPECT_EQ (result.value ().headers.count ("content-encoding"), 0U);
    EXPECT_EQ (result.value ().headers.count ("content-length"), 0U);
}

TEST (ZLinkHttpClient, RejectsUnauthorizedProxyAndAcceptsProxyBasicAuth)
{
    loopback_http_server_t server;
    loopback_proxy_t proxy (std::string ("Basic cHJveHk6c2VjcmV0")); // proxy:secret

    const auto denied = zlink::http_client::client_t::create (server.base_url ())
                          .proxy (proxy.url ())
                          .get ("/games")
                          .submit_raw ()
                          .result ();
    ASSERT_TRUE (denied);
    EXPECT_EQ (denied.value ().status, 407);

    const auto allowed = zlink::http_client::client_t::create (server.base_url ())
                           .proxy (proxy.url ())
                           .proxy_basic_auth ("proxy", "secret")
                           .get ("/games")
                           .submit_raw ()
                           .result ();
    ASSERT_TRUE (allowed) << allowed.error ()->what ();
    EXPECT_EQ (allowed.value ().status, 200);
    EXPECT_GT (proxy.rejected_requests (), 0);
    EXPECT_GT (proxy.forwarded_requests (), 0);
}

#ifdef ZLINK_HTTP_CLIENT_TEST_WITH_OPENSSL
TEST (ZLinkHttpClient, PresentsClientCertificateForMutualTls)
{
    loopback_https_server_t server (/*require_client_certificate=*/true);

    const auto with_certificate =
      zlink::http_client::client_t::create (server.base_url ())
        .trust_certificate_file (ZLINK_HTTP_CLIENT_TEST_CERT)
        .client_certificate_file (ZLINK_HTTP_CLIENT_TEST_CERT, ZLINK_HTTP_CLIENT_TEST_KEY)
        .get ("/games")
        .submit_raw ()
        .result ();
    ASSERT_TRUE (with_certificate) << with_certificate.error ()->what ();
    EXPECT_EQ (with_certificate.value ().status, 200);

    const auto without_certificate = zlink::http_client::client_t::create (server.base_url ())
                                       .trust_certificate_file (ZLINK_HTTP_CLIENT_TEST_CERT)
                                       .get ("/games")
                                       .submit_raw ()
                                       .result ();
    EXPECT_FALSE (without_certificate);
}
#endif

#ifdef ZLINK_HTTP_CLIENT_TEST_WITH_OPENSSL
TEST (ZLinkHttpClient, TunnelsHttpsThroughProxyConnect)
{
    loopback_https_server_t server;
    loopback_proxy_t proxy;
    auto client = zlink::http_client::client_t::create (server.base_url ())
                    .trust_certificate_file (ZLINK_HTTP_CLIENT_TEST_CERT)
                    .proxy (proxy.url ())
                    .build ();

    const auto result = client.get ("/games").submit<create_game_reply_t> ().result ();
    ASSERT_TRUE (result) << result.error ()->what ();
    EXPECT_EQ (result.value ().status, 200);
    EXPECT_GT (proxy.connect_tunnels (), 0);
}
#endif

#ifdef ZLINK_HTTP_CLIENT_TEST_WITH_OPENSSL
TEST (ZLinkHttpClient, SupportsHttpsWithExplicitTrust)
{
    loopback_https_server_t server;
    auto client = make_json_client (server.base_url (), std::chrono::milliseconds (500),
                                    std::string (ZLINK_HTTP_CLIENT_TEST_CERT));

    auto result = client.post ("/games")
                    .body (create_game_request_t{.name = "secure"})
                    .submit<create_game_reply_t> ()
                    .result ();

    ASSERT_TRUE (result) << result.error ()->what ();
    EXPECT_EQ (result.value ().body.method, "POST");
    EXPECT_EQ (result.value ().body.name, "secure");
}

TEST (ZLinkHttpClient, RejectsUntrustedHttpsCertificate)
{
    loopback_https_server_t server;
    auto client = make_json_client (server.base_url ());

    auto result = client.get ("/games").submit<create_game_reply_t> ().result ();

    ASSERT_FALSE (result);
    EXPECT_EQ (result.error_kind (), zlink::framework::framework_error_kind_t::internal_failure);
}

TEST (ZLinkHttpClient, RejectsHttpsHostnameMismatch)
{
    loopback_https_server_t server;
    auto client = make_json_client (server.mismatched_base_url (), std::chrono::milliseconds (500),
                                    std::string (ZLINK_HTTP_CLIENT_TEST_CERT));

    auto result = client.get ("/games").submit<create_game_reply_t> ().result ();

    ASSERT_FALSE (result);
    EXPECT_EQ (result.error_kind (), zlink::framework::framework_error_kind_t::internal_failure);
}
#endif

int main (int argc, char **argv)
{
    testing::InitGoogleTest (&argc, argv);
    return RUN_ALL_TESTS ();
}
