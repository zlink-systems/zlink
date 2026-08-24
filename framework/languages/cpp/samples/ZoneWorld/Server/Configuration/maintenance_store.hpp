/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "configuration.hpp"

#include <boost/asio.hpp>

#include <istream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace zlink::samples::zoneworld
{

// The application desired-state store is deliberately separate from the
// Framework Location and Relocation Store providers.  It uses the same Redis
// deployment but its own key namespace, as required by the sample contract.
class maintenance_store_t
{
  public:
    explicit maintenance_store_t (const configuration_t &configuration) :
        _endpoint (configuration.redis_endpoint),
        _prefix (configuration.redis_key_prefix + "maintenance:")
    {
    }

    void write (const std::string &node_id, bool enabled) const
    {
        const auto reply = execute ({"SET", _prefix + node_id, enabled ? "1" : "0"});
        if (reply.type != '+' || reply.value != "OK")
            throw std::runtime_error ("Redis did not persist ZoneWorld maintenance state");
    }

    bool read (const std::string &node_id) const
    {
        const auto reply = execute ({"GET", _prefix + node_id});
        if (reply.type == '$' && reply.nil) return false;
        if (reply.type != '$')
            throw std::runtime_error ("Redis returned an invalid ZoneWorld maintenance value");
        return reply.value == "1";
    }

  private:
    struct reply_t
    {
        char type = 0;
        std::string value;
        bool nil = false;
    };

    static std::pair<std::string, std::string> split_endpoint (std::string endpoint)
    {
        if (endpoint.rfind ("tcp://", 0) == 0) endpoint.erase (0, 6);
        else if (endpoint.rfind ("redis://", 0) == 0) endpoint.erase (0, 8);
        const auto separator = endpoint.rfind (':');
        if (separator == std::string::npos || separator == 0
            || separator + 1 == endpoint.size ())
            throw std::runtime_error ("Redis endpoint must use host:port");
        return {endpoint.substr (0, separator), endpoint.substr (separator + 1)};
    }

    static std::string encode (const std::vector<std::string> &arguments)
    {
        std::string command = "*" + std::to_string (arguments.size ()) + "\r\n";
        for (const auto &argument : arguments)
            command += "$" + std::to_string (argument.size ()) + "\r\n" + argument + "\r\n";
        return command;
    }

    static std::string read_line (boost::asio::ip::tcp::socket &socket,
                                  boost::asio::streambuf &buffer)
    {
        boost::asio::read_until (socket, buffer, "\r\n");
        std::istream input (&buffer);
        std::string line;
        std::getline (input, line);
        if (!line.empty () && line.back () == '\r') line.pop_back ();
        return line;
    }

    reply_t execute (const std::vector<std::string> &arguments) const
    {
        const auto [host, port] = split_endpoint (_endpoint);
        boost::asio::io_context io;
        boost::asio::ip::tcp::resolver resolver (io);
        boost::asio::ip::tcp::socket socket (io);
        boost::asio::connect (socket, resolver.resolve (host, port));
        const auto request = encode (arguments);
        boost::asio::write (socket, boost::asio::buffer (request));
        boost::asio::streambuf buffer;
        const auto header = read_line (socket, buffer);
        if (header.empty ()) throw std::runtime_error ("Redis returned an empty response");
        if (header.front () == '-') throw std::runtime_error ("Redis error: " + header.substr (1));
        if (header.front () == '+') return {'+', header.substr (1), false};
        if (header.front () != '$')
            throw std::runtime_error ("Redis returned an unsupported response");
        const auto size = std::stoll (header.substr (1));
        if (size < 0) return {'$', {}, true};
        while (buffer.size () < static_cast<std::size_t> (size + 2))
            boost::asio::read (socket, buffer,
              boost::asio::transfer_at_least (static_cast<std::size_t> (size + 2) - buffer.size ()));
        std::string value (static_cast<std::size_t> (size), '\0');
        std::istream input (&buffer);
        input.read (value.data (), size);
        char cr = 0, lf = 0;
        input.get (cr);
        input.get (lf);
        if (cr != '\r' || lf != '\n')
            throw std::runtime_error ("Redis bulk response terminator is invalid");
        return {'$', std::move (value), false};
    }

    std::string _endpoint;
    std::string _prefix;
};

} // namespace zlink::samples::zoneworld
