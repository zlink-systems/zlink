// STREAM multi server benchmark: callback-to-dispatcher async echo relay.
// Topology: client STREAM(connect, N) -> server STREAM(bind, 1)
// Measurement role: echo incoming framed payloads back to the originating peer.

#include "../common/perf_common.hpp"
#include "../common/perf_entry.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
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
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::deque<queued_packet_t> pending_packets;
    bool accepting_packets{true};
    std::atomic<bool> draining{false};
    std::atomic<unsigned long long> handlers_in_flight{0};
    std::atomic<unsigned long long> sends_in_flight{0};
    std::atomic<bool> failed{false};
    std::atomic<bool> drain_timed_out{false};

    stream_handler_context_t () : server (NULL) {}

    void begin_drain ()
    {
        {
            std::lock_guard<std::mutex> lock (queue_mutex);
            accepting_packets = false;
            draining.store (true, std::memory_order_release);
        }
        // Wake a dispatcher that is waiting for either the queue to close or
        // its last already-admitted send to reach a terminal state.
        queue_cv.notify_all ();
    }

    void complete_send ()
    {
        sends_in_flight.fetch_sub (1, std::memory_order_acq_rel);
        // During the active phase the dispatcher never waits for send
        // terminals: enqueue_packet() wakes it for new work. Draining is the
        // only lifecycle state that waits on this count, so avoid a contended
        // condition-variable broadcast for every active completion.
        if (draining.load (std::memory_order_acquire))
            queue_cv.notify_all ();
    }
};

struct handler_guard_t
{
    explicit handler_guard_t (stream_handler_context_t &ctx_) : ctx (ctx_)
    {
        ctx.handlers_in_flight.fetch_add (1, std::memory_order_acq_rel);
    }
    ~handler_guard_t ()
    {
        ctx.handlers_in_flight.fetch_sub (1, std::memory_order_acq_rel);
    }

    stream_handler_context_t &ctx;
};

inline void request_stop ()
{
    g_stop_requested.store (true, std::memory_order_release);
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

inline bool stale_stream_route (const zlink::submit_error_t &error_)
{
    return error_.result () == zlink::submit_result_t::not_connected
           || error_.result () == zlink::submit_result_t::not_found;
}

inline bool perf_debug_enabled ()
{
    return std::getenv ("PERF_DEBUG") != NULL;
}

inline void debug_send_failure (int err_)
{
    if (perf_debug_enabled ())
        std::cerr << k_pattern << " async echo failed errno=" << err_ << std::endl;
}

void close_packet_queue (const std::shared_ptr<stream_handler_context_t> &ctx_)
{
    ctx_->begin_drain ();
}

bool enqueue_packet (const std::shared_ptr<stream_handler_context_t> &ctx_,
                     const zlink::routing_id_t &source_rid_,
                     zlink::message_t &&packet_)
{
    {
        std::lock_guard<std::mutex> lock (ctx_->queue_mutex);
        if (!ctx_->accepting_packets
            || g_stop_requested.load (std::memory_order_acquire))
            return false;
        ctx_->pending_packets.emplace_back (source_rid_, std::move (packet_));
    }
    ctx_->queue_cv.notify_one ();
    return true;
}

perf::detached_async_task_t send_packet_async (
  const std::shared_ptr<stream_handler_context_t> &ctx_,
  zlink::routing_id_t source_rid_, zlink::message_t packet_)
{
    try {
        co_await std::move (ctx_->server->send (source_rid_))
          .message (std::move (packet_))
          .async ();
    }
    catch (const zlink::submit_error_t &error) {
        if (!stale_stream_route (error)) {
            debug_send_failure (error.internal_errno ());
            ctx_->failed.store (true, std::memory_order_release);
            close_packet_queue (ctx_);
            request_stop ();
        }
    }
    catch (const zlink::binding_error_t &error) {
        debug_send_failure (error.internal_errno ());
        ctx_->failed.store (true, std::memory_order_release);
        close_packet_queue (ctx_);
        request_stop ();
    }
    catch (...) {
        debug_send_failure (EIO);
        ctx_->failed.store (true, std::memory_order_release);
        close_packet_queue (ctx_);
        request_stop ();
    }
    ctx_->complete_send ();
}

void handle_packet (const std::shared_ptr<stream_handler_context_t> &ctx_,
                    const zlink::routing_id_t &source_rid_,
                    const zlink::message_t &header_,
                    const zlink::message_t &body_)
{
    handler_guard_t guard (*ctx_);
    if (g_stop_requested.load (std::memory_order_acquire))
        return;
    if (is_stop_payload (body_)) {
        close_packet_queue (ctx_);
        request_stop ();
        return;
    }

    zlink::message_t packet = build_packet_frame (header_, body_);
    if (!packet.valid ()) {
        ctx_->failed.store (true, std::memory_order_release);
        close_packet_queue (ctx_);
        request_stop ();
        return;
    }

    try {
        enqueue_packet (ctx_, source_rid_, std::move (packet));
    }
    catch (...) {
        debug_send_failure (EIO);
        ctx_->failed.store (true, std::memory_order_release);
        close_packet_queue (ctx_);
        request_stop ();
    }
}

void run_packet_dispatcher (const std::shared_ptr<stream_handler_context_t> &ctx_,
                            std::chrono::milliseconds drain_timeout_)
{
    std::optional<std::chrono::steady_clock::time_point> drain_deadline;
    for (;;) {
        std::optional<queued_packet_t> packet;
        {
            std::unique_lock<std::mutex> lock (ctx_->queue_mutex);
            ctx_->queue_cv.wait (lock, [&ctx_] () {
                return !ctx_->pending_packets.empty () || !ctx_->accepting_packets;
            });

            if (!ctx_->accepting_packets && !drain_deadline.has_value ())
                drain_deadline = std::chrono::steady_clock::now () + drain_timeout_;

            const bool deadline_expired =
              drain_deadline.has_value ()
              && std::chrono::steady_clock::now () >= *drain_deadline;
            if (!ctx_->pending_packets.empty () && !deadline_expired) {
                packet.emplace (std::move (ctx_->pending_packets.front ()));
                ctx_->pending_packets.pop_front ();
            }
            else if (!ctx_->accepting_packets) {
                if (deadline_expired
                    || ctx_->sends_in_flight.load (std::memory_order_acquire) == 0) {
                    if (!ctx_->pending_packets.empty ()
                        || ctx_->sends_in_flight.load (std::memory_order_acquire) != 0)
                        ctx_->drain_timed_out.store (true, std::memory_order_release);
                    ctx_->pending_packets.clear ();
                    return;
                }

                ctx_->queue_cv.wait_until (lock, *drain_deadline, [&ctx_] () {
                    return !ctx_->pending_packets.empty ()
                           || ctx_->sends_in_flight.load (std::memory_order_acquire) == 0;
                });
                continue;
            }
        }

        if (!packet.has_value ())
            continue;

        ctx_->sends_in_flight.fetch_add (1, std::memory_order_acq_rel);
        try {
            // Starting the public async terminal on this application thread is
            // required: Core packet/completion callback scopes reject submit
            // re-entry with EDEADLK.
            send_packet_async (ctx_, std::move (packet->routing_id),
                               std::move (packet->packet));
        }
        catch (...) {
            ctx_->sends_in_flight.fetch_sub (1, std::memory_order_acq_rel);
            debug_send_failure (EIO);
            ctx_->failed.store (true, std::memory_order_release);
            close_packet_queue (ctx_);
            request_stop ();
        }
    }
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

bool run_server_pull_loop (const std::shared_ptr<stream_handler_context_t> &ctx_)
{
    zlink::poller_t poller;
    poller.add (*ctx_->server, zlink::poll_event_flag_t::pollin, 0);
    zlink::poll_event_t event;

    while (!g_stop_requested.load (std::memory_order_acquire)
           && !ctx_->failed.load (std::memory_order_acquire)) {
        try {
            if (poller.wait (&event, 1, std::chrono::milliseconds (200)) == 0)
                continue;
        }
        catch (const zlink::binding_error_t &error) {
            if (error.internal_errno () == EINTR)
                continue;
            debug_send_failure (error.internal_errno ());
            ctx_->failed.store (true, std::memory_order_release);
            break;
        }

        if ((static_cast<short> (event.revents)
             & static_cast<short> (zlink::poll_event_flag_t::pollin)) == 0)
            continue;

        while (!g_stop_requested.load (std::memory_order_acquire)) {
            zlink::stream_packet_t packet;
            try {
                if (!ctx_->server->recv_packet (packet, zlink::recv_flags_t::dontwait))
                    break;
            }
            catch (const zlink::recv_error_t &error) {
                const int err = error.internal_errno ();
                if (error.result () == zlink::recv_result_t::no_data || err == EAGAIN
                    || err == EWOULDBLOCK || err == EINTR)
                    break;
                debug_send_failure (err);
                ctx_->failed.store (true, std::memory_order_release);
                request_stop ();
                break;
            }

            if (!packet.routing_id ().has_value () || packet.routing_id ()->size () == 0) {
                debug_send_failure (EPROTO);
                ctx_->failed.store (true, std::memory_order_release);
                request_stop ();
                break;
            }
            handle_packet (ctx_, *packet.routing_id (), packet.header (), packet.body ());
        }
    }

    return !ctx_->failed.load (std::memory_order_acquire);
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
        const size_t effective_msg_size =
          msg_size > 0 ? msg_size : perf::multi::resolve_case_msg_sizes (64).front ();

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
        options.recv_mode (zlink::stream_recv_mode_t::packet);
        if (!perf::multi::recalculate_auto_hwm (ctx))
            return false;

        if (!perf::multi::setup_tls_server (server, transport))
            return false;

        perf::multi::connect_monitor_t connect_monitor;
        if (!perf::multi::open_connect_monitor (server, settings.monitor_hwm,
                                                connect_monitor))
            return false;

        const std::string bind_endpoint =
          perf::multi::make_endpoint (transport, "cpp_multi_stream", settings.server_bind_port);
        server.bind (bind_endpoint);

        const std::string endpoint =
          transport == "inproc"
            ? bind_endpoint
            : perf::multi::normalize_endpoint_host (server.options ().last_endpoint ());
        if (endpoint.empty ())
            return false;

        g_stop_requested.store (false, std::memory_order_release);
        install_signal_handlers ();

        const std::shared_ptr<stream_handler_context_t> handler_context =
          std::make_shared<stream_handler_context_t> ();
        handler_context->server = &server;
        perf::multi::print_ready (endpoint);
        // CLIENT_READY means the shared raw peer connected every requested
        // session and applied this size. Consume the server-side monitor
        // barrier as independent proof that all target routes are usable,
        // then recalculate on the server owner path before active traffic.
        if (!perf::multi::wait_for_start_from_stdin (effective_msg_size))
            return false;
        if (!perf::multi::wait_connect_ready_count (
              connect_monitor, settings.clients, settings.connect_ready_timeout_ms)) {
            perf::multi::close_connect_monitor (connect_monitor);
            return false;
        }
        perf::multi::close_connect_monitor (connect_monitor);
        if (!perf::multi::recalculate_auto_hwm (ctx))
            return false;
        perf::multi::emit_auto_hwm_detail (server, "server", "server-connected",
                                           transport, effective_msg_size, "stream");
        std::cout << "SERVER_START_READY," << effective_msg_size << std::endl;

        const std::chrono::milliseconds drain_timeout (
          std::max (1000, io_timeout_ms * 4));
        std::thread dispatcher_thread (&run_packet_dispatcher, handler_context,
                                       drain_timeout);

        std::thread stdin_watcher (&wait_for_stop_stdin);
        stdin_watcher.detach ();

        run_server_pull_loop (handler_context);
        close_packet_queue (handler_context);
        dispatcher_thread.join ();

        const auto handler_deadline = std::chrono::steady_clock::now () + drain_timeout;
        while (handler_context->handlers_in_flight.load (std::memory_order_acquire) != 0
               && std::chrono::steady_clock::now () < handler_deadline) {
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
        return !handler_context->failed.load (std::memory_order_acquire)
               && !handler_context->drain_timed_out.load (std::memory_order_acquire)
               && handler_context->handlers_in_flight.load (std::memory_order_acquire) == 0
               && handler_context->sends_in_flight.load (std::memory_order_acquire) == 0;
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
