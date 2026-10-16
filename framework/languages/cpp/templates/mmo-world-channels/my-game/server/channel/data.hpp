#pragma once
#include "server/shared/config.hpp"
#include <boost/asio.hpp>
#include <charconv>

namespace MyGame
{
// Application Redis data is separate from Framework Location and Relocation stores.
// The RESP boundary follows the public Bingo sample; it never handles Framework frames.
class PlayerData
{
    const NodeConfig &config_;
    std::optional<std::string> execute (const std::vector<std::string> &arguments) const
    {
        const auto separator = config_.RedisEndpoint.rfind (':');
        if (separator == std::string::npos)
            throw std::invalid_argument ("Redis requires host:port");
        boost::asio::io_context io;
        boost::asio::ip::tcp::resolver resolver (io);
        boost::asio::ip::tcp::socket socket (io);
        boost::asio::connect (socket,
                              resolver.resolve (config_.RedisEndpoint.substr (0, separator),
                                                config_.RedisEndpoint.substr (separator + 1)));
        std::string command = "*" + std::to_string (arguments.size ()) + "\r\n";
        for (const auto &arg : arguments)
            command += "$" + std::to_string (arg.size ()) + "\r\n" + arg + "\r\n";
        boost::asio::write (socket, boost::asio::buffer (command));
        boost::asio::streambuf buffer;
        boost::asio::read_until (socket, buffer, "\r\n");
        std::istream input (&buffer);
        std::string header;
        std::getline (input, header);
        if (header.empty () || header.back () != '\r')
            throw std::runtime_error ("Invalid Redis response");
        header.pop_back ();
        if (header == "+OK")
            return std::string ("OK");
        if (header == "$-1")
            return std::nullopt;
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
            boost::asio::read (socket, buffer,
                               boost::asio::transfer_exactly (size + 2 - buffer.size ()));
        std::string value (size, '\0');
        input.read (value.data (), static_cast<std::streamsize> (size));
        if (input.get () != '\r' || input.get () != '\n')
            throw std::runtime_error ("Invalid Redis terminator");
        return value;
    }
    std::string key (std::string_view id) const
    {
        auto result = config_.KeyPrefix + Names::player_key;
        result.append (id);
        return result;
    }

  public:
    explicit PlayerData (const NodeConfig &config) : config_ (config) {}
    std::int64_t load (std::string_view id) const
    {
        const auto stored = execute ({"GET", key (id)});
        if (!stored)
            return 0;
        std::int64_t value = 0;
        const auto parsed =
          std::from_chars (stored->data (), stored->data () + stored->size (), value);
        if (parsed.ec != std::errc{} || parsed.ptr != stored->data () + stored->size ()
            || value < 0)
            throw std::runtime_error ("Invalid stored gold");
        return value;
    }
    void save (std::string_view id, std::int64_t gold) const
    {
        if (execute ({"SET", key (id), std::to_string (gold)}) != std::optional<std::string> ("OK"))
            throw std::runtime_error ("Redis save was not acknowledged");
    }
};
}
