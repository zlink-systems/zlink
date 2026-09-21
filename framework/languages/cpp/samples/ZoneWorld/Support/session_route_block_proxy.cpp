// Transparent ZMP proxy that drops command 44 (session relocation route
// commit) while the B8 arm file exists. run_sample.{sh,ps1} places one in front
// of each zone node's mesh endpoint for the ZW-B8 fault scenario. It is built
// with the sample so the runner needs no other runtime than the sample's own.

#include <boost/asio.hpp>

#include <array>
#include <format>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

constexpr std::uint8_t zmp_magic = 0x5A;
constexpr std::uint8_t zmp_version = 0x01;
constexpr std::size_t zmp_header_size = 8;
constexpr std::size_t zmp_request_sequence_size = 8;
constexpr std::uint8_t zmp_flag_more = 0x01;
constexpr std::uint8_t session_relocation_route = 44;

struct frame_t
{
    std::vector<std::uint8_t> raw;
    std::uint8_t flags;
    std::vector<std::uint8_t> body;
};

class frame_parser_t
{
  public:
    std::vector<frame_t> feed (const std::uint8_t *data, std::size_t size)
    {
        _buffer.insert (_buffer.end (), data, data + size);
        std::vector<frame_t> frames;
        while (_buffer.size () >= zmp_header_size) {
            if (_buffer[0] != zmp_magic || _buffer[1] != zmp_version)
                throw std::runtime_error ("unexpected ZMP frame header");
            const auto flags = _buffer[2];
            const auto kind = _buffer[3];
            const std::size_t body_size = (std::size_t (_buffer[4]) << 24)
                                          | (std::size_t (_buffer[5]) << 16)
                                          | (std::size_t (_buffer[6]) << 8) | _buffer[7];
            std::size_t header_size = zmp_header_size;
            if (kind == 0x01 || kind == 0x02 || kind == 0x03)
                header_size += zmp_request_sequence_size;
            const auto total = header_size + body_size;
            if (_buffer.size () < total)
                break;
            frame_t frame;
            frame.raw.assign (_buffer.begin (), _buffer.begin () + total);
            frame.flags = flags;
            frame.body.assign (frame.raw.begin () + header_size, frame.raw.end ());
            _buffer.erase (_buffer.begin (), _buffer.begin () + total);
            frames.push_back (std::move (frame));
        }
        return frames;
    }

  private:
    std::vector<std::uint8_t> _buffer;
};

struct command_44_t
{
    std::string actor;
    std::uint8_t action;
    std::uint64_t previous_authority;
    std::uint64_t target_authority;
};

// Mirrors the wire layout the .NET/Node runners parse: a bounds violation means
// "not a command 44 we understand", never a crash.
class body_reader_t
{
  public:
    explicit body_reader_t (const std::vector<std::uint8_t> &body) : _body (body) {}

    std::uint8_t u8 ()
    {
        need (1);
        return _body[_offset++];
    }
    std::uint16_t u16 ()
    {
        need (2);
        const auto value = (std::uint16_t (_body[_offset]) << 8) | _body[_offset + 1];
        _offset += 2;
        return value;
    }
    std::uint64_t u64 ()
    {
        need (8);
        std::uint64_t value = 0;
        for (int i = 0; i < 8; ++i)
            value = (value << 8) | _body[_offset + i];
        _offset += 8;
        return value;
    }
    std::string text8 ()
    {
        const auto size = u8 ();
        need (size);
        std::string value (_body.begin () + _offset, _body.begin () + _offset + size);
        _offset += size;
        return value;
    }
    void skip_text16 ()
    {
        const auto size = u16 ();
        skip (size);
    }
    void skip (std::size_t count)
    {
        need (count);
        _offset += count;
    }

  private:
    void need (std::size_t count) const
    {
        if (_offset + count > _body.size ())
            throw std::out_of_range ("command 44 body is shorter than its layout");
    }

    const std::vector<std::uint8_t> &_body;
    std::size_t _offset = 0;
};

std::optional<command_44_t> command_44_identity (const std::vector<std::uint8_t> &body)
{
    if (body.size () < 5 || body[0] != 90 || body[1] != 77 || body[3] != session_relocation_route)
        return std::nullopt;
    try {
        body_reader_t reader (body);
        reader.skip (5 + 16);
        (void) reader.text8 ();
        reader.skip (8);
        (void) reader.text8 ();
        reader.skip (8);
        reader.skip_text16 ();
        reader.skip (1);
        auto actor = reader.text8 ();
        reader.skip (8);
        (void) reader.text8 ();
        reader.skip (8);
        (void) reader.text8 ();
        reader.skip (8);
        (void) reader.text8 ();
        reader.skip (8);
        const auto action = reader.u8 ();
        const auto route_size = reader.u16 ();
        std::uint64_t previous = 0;
        std::uint64_t target = 0;
        if (action == 1 && route_size >= 16) {
            previous = reader.u64 ();
            target = reader.u64 ();
        }
        return command_44_t{std::move (actor), action, previous, target};
    }
    catch (const std::out_of_range &) {
        return std::nullopt;
    }
}

void pump (tcp::socket &source,
           tcp::socket &sink,
           const char *direction,
           const std::filesystem::path &arm_file)
{
    frame_parser_t parser;
    std::vector<std::uint8_t> message;
    std::size_t message_frames = 0;
    std::optional<command_44_t> command_44;
    std::array<std::uint8_t, 65536> chunk{};
    try {
        for (;;) {
            boost::system::error_code error;
            const auto received = source.read_some (asio::buffer (chunk), error);
            if (error || received == 0)
                break;
            for (auto &frame : parser.feed (chunk.data (), received)) {
                message.insert (message.end (), frame.raw.begin (), frame.raw.end ());
                ++message_frames;
                if (auto identity = command_44_identity (frame.body))
                    command_44 = std::move (identity);
                if (frame.flags & zmp_flag_more)
                    continue;
                const bool blocked = message_frames == 1 && std::filesystem::exists (arm_file)
                                     && command_44 && command_44->action == 1;
                if (blocked) {
                    std::ofstream (arm_file.string () + ".blocked").put ('\n');
                    std::cout << "blocked-command-44 direction=" << direction
                              << " actor=" << command_44->actor << " action=commit"
                              << " previous-authority=" << command_44->previous_authority
                              << " target-authority=" << command_44->target_authority << std::endl;
                } else {
                    asio::write (sink, asio::buffer (message));
                }
                message.clear ();
                message_frames = 0;
                command_44.reset ();
            }
        }
    }
    catch (const std::exception &error) {
        std::cout << "proxy-pump-ended direction=" << direction << " error=" << error.what ()
                  << std::endl;
    }
    boost::system::error_code ignored;
    sink.shutdown (tcp::socket::shutdown_send, ignored);
}

void serve (tcp::socket client, const tcp::endpoint target, const std::filesystem::path arm_file)
{
    asio::io_context io;
    tcp::socket upstream (io);
    boost::system::error_code error;
    upstream.connect (target, error);
    if (error)
        return;
    std::thread downstream ([&] { pump (upstream, client, "gateway-to-peer", arm_file); });
    pump (client, upstream, "peer-to-gateway", arm_file);
    downstream.join ();
    boost::system::error_code ignored;
    upstream.close (ignored);
}

struct options_t
{
    std::string listen_host;
    unsigned short listen_port = 0;
    std::string target_host;
    unsigned short target_port = 0;
    std::filesystem::path arm_file;
};

options_t parse_options (int argc, char **argv)
{
    options_t options;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string name = argv[i];
        const std::string value = argv[i + 1];
        if (name == "--listen-host")
            options.listen_host = value;
        else if (name == "--listen-port")
            options.listen_port = static_cast<unsigned short> (std::stoi (value));
        else if (name == "--target-host")
            options.target_host = value;
        else if (name == "--target-port")
            options.target_port = static_cast<unsigned short> (std::stoi (value));
        else if (name == "--arm-file")
            options.arm_file = value;
        else
            throw std::invalid_argument ("unknown option: " + name);
    }
    if (options.listen_host.empty () || options.listen_port == 0 || options.target_host.empty ()
        || options.target_port == 0 || options.arm_file.empty ())
        throw std::invalid_argument (
          "usage: --listen-host H --listen-port P --target-host H --target-port P --arm-file F");
    return options;
}
} // namespace

int main (int argc, char **argv)
{
    try {
        const auto options = parse_options (argc, argv);
        asio::io_context io;
        const tcp::endpoint listen (asio::ip::make_address (options.listen_host),
                                    options.listen_port);
        const tcp::endpoint target (asio::ip::make_address (options.target_host),
                                    options.target_port);
        tcp::acceptor acceptor (io);
        acceptor.open (listen.protocol ());
        acceptor.set_option (asio::socket_base::reuse_address (true));
        acceptor.bind (listen);
        acceptor.listen ();
        std::cout << "proxy-ready listen=" << options.listen_host << ':' << options.listen_port
                  << " target=" << options.target_host << ':' << options.target_port << std::endl;
        for (;;) {
            tcp::socket client (io);
            acceptor.accept (client);
            std::thread (serve, std::move (client), target, options.arm_file).detach ();
        }
    }
    catch (const std::exception &error) {
        const std::string line = std::format ("session_route_block_proxy: {}\n", error.what ());
        std::cerr << line;
        return 1;
    }
}
