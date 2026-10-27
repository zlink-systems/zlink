#pragma once
#include "server/shared/config.hpp"
#include <zlink/framework.hpp>
#include <boost/asio.hpp>
#include <charconv>
#include <thread>

namespace MyGame
{
namespace fx = zlink::framework;
// Application Redis data is separate from Framework Location and Relocation stores.
// Each command runs on this object's own I/O thread and completes a Framework task, so
// callers await it without blocking their thread. It never handles Framework frames.
class PlayerData
{
    using Reply = std::optional<std::string>;
    const NodeConfig &config_;
    std::string host_, port_;
    boost::asio::io_context io_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_;
    std::thread worker_;

    boost::asio::awaitable<Reply> exchange (std::string command) const
    {
        namespace net = boost::asio;
        const auto executor = co_await net::this_coro::executor;
        net::ip::tcp::resolver resolver (executor);
        net::ip::tcp::socket socket (executor);
        co_await net::async_connect (
          socket, co_await resolver.async_resolve (host_, port_, net::use_awaitable),
          net::use_awaitable);
        co_await net::async_write (socket, net::buffer (command), net::use_awaitable);
        net::streambuf buffer;
        co_await net::async_read_until (socket, buffer, "\r\n", net::use_awaitable);
        std::istream input (&buffer);
        std::string header;
        std::getline (input, header);
        if (header.empty () || header.back () != '\r')
            throw std::runtime_error ("Invalid Redis response");
        header.pop_back ();
        if (header == "+OK")
            co_return std::string ("OK");
        if (header == "$-1")
            co_return std::nullopt;
        if (!header.starts_with ('$'))
            throw std::runtime_error ("Redis command failed: " + header);
        std::size_t size = 0;
        const auto parsed =
          std::from_chars (header.data () + 1, header.data () + header.size (), size);
        constexpr std::size_t maximum_gold_bytes = 20;
        if (parsed.ec != std::errc{} || parsed.ptr != header.data () + header.size ()
            || size > maximum_gold_bytes)
            throw std::runtime_error ("Invalid Redis gold length");
        if (buffer.size () < size + 2)
            co_await net::async_read (socket, buffer,
                                      net::transfer_exactly (size + 2 - buffer.size ()),
                                      net::use_awaitable);
        std::string value (size, '\0');
        input.read (value.data (), static_cast<std::streamsize> (size));
        if (input.get () != '\r' || input.get () != '\n')
            throw std::runtime_error ("Invalid Redis terminator");
        co_return value;
    }
    fx::task_t<Reply> execute (const std::vector<std::string> &arguments)
    {
        std::string command = "*" + std::to_string (arguments.size ()) + "\r\n";
        for (const auto &arg : arguments)
            command += "$" + std::to_string (arg.size ()) + "\r\n" + arg + "\r\n";
        auto completion = std::make_shared<fx::task_completion_source_t<Reply>> ();
        boost::asio::co_spawn (
          io_, exchange (std::move (command)),
          [completion] (std::exception_ptr error, Reply reply) {
              if (!error) {
                  completion->complete (fx::result_t<Reply>::success (std::move (reply)));
                  return;
              }
              try {
                  std::rethrow_exception (error);
              }
              catch (const std::exception &failure) {
                  completion->complete (fx::result_t<Reply>::failure (
                    fx::framework_error_kind_t::internal_failure, failure.what ()));
              }
          });
        return completion->task ();
    }
    std::string key (std::string_view id) const
    {
        auto result = config_.KeyPrefix + Names::player_key;
        result.append (id);
        return result;
    }

  public:
    explicit PlayerData (const NodeConfig &config) :
        config_ (config), work_ (boost::asio::make_work_guard (io_))
    {
        const auto separator = config_.RedisEndpoint.rfind (':');
        if (separator == std::string::npos)
            throw std::invalid_argument ("Redis requires host:port");
        host_ = config_.RedisEndpoint.substr (0, separator);
        port_ = config_.RedisEndpoint.substr (separator + 1);
        worker_ = std::thread ([this] { io_.run (); });
    }
    PlayerData (const PlayerData &) = delete;
    PlayerData &operator= (const PlayerData &) = delete;
    // Commands already started finish before the I/O thread exits.
    ~PlayerData ()
    {
        work_.reset ();
        worker_.join ();
    }
    fx::task_t<std::int64_t> load (std::string id)
    {
        auto command = execute ({"GET", key (id)});
        const auto stored = co_await command;
        if (!stored)
            co_return 0;
        std::int64_t value = 0;
        const auto parsed =
          std::from_chars (stored->data (), stored->data () + stored->size (), value);
        if (parsed.ec != std::errc{} || parsed.ptr != stored->data () + stored->size ()
            || value < 0)
            throw std::runtime_error ("Invalid stored gold");
        co_return value;
    }
    fx::task_t<void> save (std::string id, std::int64_t gold)
    {
        auto command = execute ({"SET", key (id), std::to_string (gold)});
        if (co_await command != std::optional<std::string> ("OK"))
            throw std::runtime_error ("Redis save was not acknowledged");
    }
};
}
