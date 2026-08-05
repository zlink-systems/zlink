// STREAM multi server benchmark: callback-based echo relay.
// Topology: client STREAM(connect, N) -> server STREAM(bind, 1)
// Measurement role: echo incoming framed payloads back to the originating peer.

#include "../common/perf_common.hpp"
#include "../common/perf_entry.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace
{

static const char *k_pattern = "MULTI_STREAM";
static const char k_stop_token[] = "__zlink_perf_stop__";
static std::atomic<bool> g_stop_requested (false);
static std::mutex g_stop_mutex;
static std::condition_variable g_stop_cv;

struct queued_packet_t
{
    zlink::routing_id_t routing_id;
    zlink::message_t packet;

    queued_packet_t (const zlink::routing_id_t &routing_id_, zlink::message_t &&packet_) :
        routing_id (routing_id_), packet (std::move (packet_))
    {
    }

    queued_packet_t (queued_packet_t &&) = default;
    queued_packet_t &operator= (queued_packet_t &&) = default;
};

struct stream_handler_context_t
{
    zlink::stream_socket_t *server;
    std::mutex pending_mutex;
    std::deque<queued_packet_t> pending_packets;

    stream_handler_context_t () : server (NULL), pending_packets () {}
};

inline void request_stop ()
{
    g_stop_requested.store (true, std::memory_order_release);
    g_stop_cv.notify_all ();
}

inline void on_signal (int)
{
    request_stop ();
}

inline void install_signal_handlers ()
{
    std::signal (SIGINT, on_signal);
#if defined(SIGTERM)
    std::signal (SIGTERM, on_signal);
#endif
}

inline bool is_stop_payload (const zlink::message_t &body_)
{
    const void *data = body_.data ();
    const size_t size = body_.size ();
    return data && size == std::strlen (k_stop_token)
           && std::memcmp (data, k_stop_token, size) == 0;
}

inline void store_u32_be (unsigned char *dst_, uint32_t value_)
{
    dst_[0] = static_cast<unsigned char> ((value_ >> 24) & 0xFF);
    dst_[1] = static_cast<unsigned char> ((value_ >> 16) & 0xFF);
    dst_[2] = static_cast<unsigned char> ((value_ >> 8) & 0xFF);
    dst_[3] = static_cast<unsigned char> (value_ & 0xFF);
}

inline zlink::message_t build_packet_frame (const zlink::message_t &header_,
                                            const zlink::message_t &body_)
{
    const size_t header_size = header_.size ();
    const size_t body_size = body_.size ();
    const size_t packet_size = 6 + header_size + body_size;
    zlink::message_t packet (packet_size);
    if (!packet.valid ())
        return packet;

    unsigned char *frame = reinterpret_cast<unsigned char *> (packet.data ());
    frame[0] = static_cast<unsigned char> ((header_size >> 8) & 0xFF);
    frame[1] = static_cast<unsigned char> (header_size & 0xFF);
    store_u32_be (frame + 2, static_cast<uint32_t> (body_size));
    if (header_size > 0)
        std::memcpy (frame + 6, header_.data (), header_size);
    if (body_size > 0)
        std::memcpy (frame + 6 + header_size, body_.data (), body_size);
    return packet;
}

bool try_send_packet (stream_handler_context_t &ctx_,
                      const zlink::routing_id_t &source_rid_,
                      zlink::message_t &packet_)
{
    try {
        const bool sent = std::move (ctx_.server->send (source_rid_))
                            .message (packet_)
                            .flags (zlink::send_flags_t::dontwait)
                            .submit ();
        if (sent)
            return true;
        errno = EAGAIN;
        return false;
    }
    catch (const zlink::submit_error_t &err) {
        errno = err.internal_errno ();
        request_stop ();
        return false;
    }
}

void enqueue_packet (stream_handler_context_t &ctx_,
                     const zlink::routing_id_t &source_rid_,
                     zlink::message_t &&packet_)
{
    {
        std::lock_guard<std::mutex> lock (ctx_.pending_mutex);
        ctx_.pending_packets.emplace_back (source_rid_, std::move (packet_));
    }
    g_stop_cv.notify_all ();
}

size_t pending_packet_count (stream_handler_context_t &ctx_)
{
    std::lock_guard<std::mutex> lock (ctx_.pending_mutex);
    return ctx_.pending_packets.size ();
}

void drain_pending_packets (stream_handler_context_t &ctx_)
{
    for (;;) {
        std::optional<queued_packet_t> packet;
        {
            std::lock_guard<std::mutex> lock (ctx_.pending_mutex);
            if (ctx_.pending_packets.empty ())
                return;
            packet.emplace (std::move (ctx_.pending_packets.front ()));
            ctx_.pending_packets.pop_front ();
        }

        if (try_send_packet (ctx_, packet->routing_id, packet->packet)) {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) {
            std::lock_guard<std::mutex> lock (ctx_.pending_mutex);
            ctx_.pending_packets.push_front (std::move (*packet));
            return;
        }

        return;
    }
}

void handle_packet (stream_handler_context_t &ctx_,
                    const zlink::routing_id_t &source_rid_,
                    const zlink::message_t &header_,
                    const zlink::message_t &body_)
{
    if (is_stop_payload (body_)) {
        request_stop ();
        return;
    }

    zlink::message_t packet = build_packet_frame (header_, body_);
    if (!packet.valid ()) {
        request_stop ();
        return;
    }

    if (try_send_packet (ctx_, source_rid_, packet))
        return;

    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT)
        enqueue_packet (ctx_, source_rid_, std::move (packet));
}

void wait_for_stop_stdin ()
{
    std::string line;
    while (std::getline (std::cin, line)) {
        if (line == "STOP" || line == "QUIT") {
            request_stop ();
            return;
        }
    }

    request_stop ();
}

void run_server_event_loop (stream_handler_context_t &handler_context_)
{
    while (!g_stop_requested.load (std::memory_order_acquire)) {
        if (pending_packet_count (handler_context_) > 0)
            drain_pending_packets (handler_context_);

        if (pending_packet_count (handler_context_) == 0) {
            std::unique_lock<std::mutex> stop_lock (g_stop_mutex);
            g_stop_cv.wait (stop_lock, [&handler_context_] () {
                return g_stop_requested.load (std::memory_order_acquire)
                       || pending_packet_count (handler_context_) > 0;
            });
            continue;
        }

        zlink::poller_t poller;
        try {
            poller.add (*handler_context_.server, zlink::poll_event_flag_t::pollout, 0);
            zlink::poll_event_t event;
            const size_t event_count = poller.wait (&event, 1, std::chrono::milliseconds (-1));
            for (size_t i = 0; i < event_count; ++i) {
                const short revents = static_cast<short> (event.revents);
                if (revents & static_cast<short> (zlink::poll_event_flag_t::pollout)) {
                    drain_pending_packets (handler_context_);
                }
            }
        }
        catch (const zlink::recv_error_t &err) {
            const int err_no = err.internal_errno ();
            if (err_no != EINTR && err_no != EAGAIN) {
                request_stop ();
                return;
            }
        }
    }
}

} // namespace

bool perf_stream_server (const std::string &lib_name, const std::string &transport, size_t msg_size)
{
    perf::multi::set_perf_pattern_env (k_pattern);

    if (!perf::multi::is_supported_transport (transport)) {
        std::cout << "UNSUPPORTED," << lib_name << "," << k_pattern << "," << transport
                  << std::endl;
        return true;
    }

    try {
        const perf::multi::multi_bench_settings_t settings =
          perf::multi::resolve_multi_bench_settings ();

        perf::multi::ctx_guard_t ctx;
        zlink::stream_socket_t server (ctx.ctx ());
        if (!server.valid ())
            return false;

        zlink::stream_socket_options_t options = server.options ();
        if (perf::multi::manual_socket_overrides_enabled ()) {
            options.send_hwm (
              zlink::byte_count_t::bytes (
                static_cast<uint64_t> (settings.sndhwm > 0 ? settings.sndhwm : 1)));
            options.recv_hwm (
              zlink::byte_count_t::bytes (
                static_cast<uint64_t> (settings.rcvhwm > 0 ? settings.rcvhwm : 1)));
        }
        const int io_timeout_ms = perf::multi::parse_positive_env (
          "PERF_STREAM_TIMEOUT_MS", std::max (settings.sndtimeo_ms, settings.rcvtimeo_ms));
        options.send_timeout (std::chrono::milliseconds (io_timeout_ms));
        options.recv_timeout (std::chrono::milliseconds (io_timeout_ms));
        options.linger (std::chrono::milliseconds (0));
        options.tcp_no_delay (true);
        if (!perf::multi::apply_benchmark_auto_hwm_msg_unit (ctx, msg_size)
            || !perf::multi::recalculate_auto_hwm (ctx))
            return false;

        if (!perf::multi::setup_tls_server (server, transport))
            return false;

        const std::string bind_endpoint =
          perf::multi::make_endpoint (transport, "cpp_multi_stream", settings.server_bind_port);
        server.bind (bind_endpoint);
        ctx.ctx ().recalculate_auto_hwm ();

        const std::string endpoint =
          transport == "inproc"
            ? bind_endpoint
            : perf::multi::normalize_endpoint_host (server.options ().last_endpoint ());
        if (endpoint.empty ())
            return false;
        perf::multi::emit_auto_hwm_detail (server, "server", "server", transport, msg_size,
                                           "stream");

        g_stop_requested.store (false, std::memory_order_release);
        install_signal_handlers ();

        stream_handler_context_t handler_context;
        handler_context.server = &server;
        server.set_packet_handler ([&handler_context] (const zlink::routing_id_t &source_rid_,
                                                       zlink::message_t header_,
                                                       zlink::message_t body_) {
            handle_packet (handler_context, source_rid_, header_, body_);
        });

        std::thread stdin_watcher (&wait_for_stop_stdin);
        stdin_watcher.detach ();

        std::thread event_loop_thread (
          [&handler_context] () { run_server_event_loop (handler_context); });
        perf::multi::print_ready (endpoint);
        event_loop_thread.join ();
        return true;
    }
    catch (const zlink::binding_error_t &) {
        return false;
    }
}

int main (int argc, char **argv)
{
    if (argc < 3) {
        std::cerr << "usage: <lib_name> <transport> [size]" << std::endl;
        return 1;
    }

    const std::string lib_name = argv[1];
    const std::string transport = argv[2];
    const size_t size = argc >= 4 ? static_cast<size_t> (std::strtoull (argv[3], NULL, 10)) : 0;

    return perf_stream_server (lib_name, transport, size) ? 0 : 1;
}
