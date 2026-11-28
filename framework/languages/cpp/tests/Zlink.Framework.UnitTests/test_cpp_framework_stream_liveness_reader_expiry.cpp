/* SPDX-License-Identifier: Elastic-2.0 */
#include "../support/stream_liveness_fixture.hpp"

#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/streams/stream_host_service_internal.hpp"

#include <boost/asio/post.hpp>

#include <future>

namespace
{
using namespace stream_liveness_test;

void check_reader_expiry (std::string_view transport, std::string_view frame_kind)
{
    const bool trace = rt::stream_host_detail::stream_trace_enabled ();
    const bool partial = frame_kind.starts_with ("partial");
    const bool slow_read = frame_kind.ends_with ("-slow-read");
    const bool check_available = frame_kind.ends_with ("-available");
    constexpr auto prefix_size = ping_only_client_t::prefix_size;
    with_stream_host (
      transport, 0ms,
      [&] (auto &host, const auto &runtime, const auto &endpoint, auto &, auto &, auto &) {
          auto paused = std::make_shared<std::promise<std::weak_ptr<tcp::socket>>> ();
          std::promise<void> release;
          const auto reader_released = release.get_future ().share ();
          auto reader_paused = paused->get_future ();
          struct reader_guard_t
          {
              std::promise<void> &release;
              ~reader_guard_t ()
              {
                  // Release the reader before the fixture stops its host.
                  if (pending)
                      release.set_value ();
              }
              bool pending = true;
          } reader_guard{release};
          auto checking_header = std::make_shared<std::promise<std::size_t>> ();
          auto header_remaining = checking_header->get_future ();
          std::promise<void> release_check;
          const auto check_released = release_check.get_future ().share ();
          reader_guard_t check_guard{release_check};
          rt::stream_host_service_test_access_t::observe_shutdown (
            host,
            {[paused, reader_released] (auto, auto connection) {
                 paused->set_value (connection.socket);
                 reader_released.wait ();
             },
             {},
             [checking_header, check_released, check_available] (std::size_t remaining) mutable {
                 if (check_available && checking_header && remaining > prefix_size) {
                     checking_header->set_value (remaining);
                     checking_header.reset ();
                     check_released.wait ();
                 }
             }});

          ping_only_client_t client (endpoint, runtime);
          require (reader_paused.wait_for (5s) == std::future_status::ready,
                   "Reader did not reach the registered hook");
          const auto deadline = steady_clock::now () + receive_timeout;
          const auto &peer = client.observation ();
          if (check_available)
              client.send_ping_bytes (0, prefix_size);
          else
              client.send_ping (partial);
          while (!peer.writes && !peer.write_error && steady_clock::now () < deadline)
              client.advance ();
          require (peer.writes == 1 && !peer.pongs && !peer.closing,
                   "Ping bytes were not written while the reader was paused");

          // The timer was armed before the registered hook. Releasing after the
          // full timeout puts both the timer and the prewritten bytes in scope.
          std::this_thread::sleep_until (deadline);
          auto delay_completed = std::make_shared<std::promise<void>> ();
          auto reader_delayed = delay_completed->get_future ();
          if (slow_read) {
              const auto socket = reader_paused.get ().lock ();
              require (socket && socket->is_open (), "Reader hook did not expose its TCP socket");
              require (socket->available () == (partial ? 1 : client.ping_size ()),
                       "Server TCP receive buffer does not contain the expected ping bytes");

              // The reader first services this queue after starting the async peek.
              asio::post (socket->get_executor (), [delay_completed] {
                  std::this_thread::sleep_until (steady_clock::now ()
                                                 + rt::dispatch_limits::receive_batch_time * 2);
                  delay_completed->set_value ();
              });
          }
          release.set_value ();
          reader_guard.pending = false;
          constexpr auto observation_window = 5s;
          const auto released_at = steady_clock::now ();
          const auto result_deadline = released_at + observation_window;
          steady_clock::time_point header_released_at;
          if (check_available) {
              require (header_remaining.wait_until (result_deadline) == std::future_status::ready,
                       "Reader did not reach the TCP header expiry check");
              const auto remaining = header_remaining.get ();
              require (remaining == client.ping_size () - prefix_size,
                       "The header read must be pending before its bytes are sent");
              const auto supplied = remaining - (partial ? 1 : 0);
              client.send_ping_bytes (prefix_size, supplied);
              while (peer.writes < 2 && !peer.write_error && steady_clock::now () < result_deadline)
                  client.advance ();
              const auto socket = reader_paused.get ().lock ();
              require (peer.writes == 2 && socket && socket->available () == supplied,
                       "The server socket did not hold the exact test byte count");
              release_check.set_value ();
              if (trace)
                  header_released_at = steady_clock::now ();
              check_guard.pending = false;
          }
          while (!peer.pongs && !peer.reason && !peer.closed
                 && steady_clock::now () < result_deadline)
              client.advance ();
          if (trace) {
              const auto observed_at = steady_clock::now ();
              const auto ns = [] (steady_clock::time_point at) {
                  return std::chrono::duration_cast<std::chrono::nanoseconds> (
                           at.time_since_epoch ())
                    .count ();
              };
              std::cout << "reader-expiry release_return_ns=" << ns (released_at);
              if (check_available)
                  std::cout << " header_release_return_ns=" << ns (header_released_at);
              std::cout << " observation_end_ns=" << ns (observed_at) << " outcome="
                        << (peer.pongs                     ? "pong"
                            : (peer.reason || peer.closed) ? "close"
                                                           : "timeout")
                        << '\n';
          }
          if (slow_read)
              require (reader_delayed.wait_until (result_deadline) == std::future_status::ready,
                       "TCP reader did not execute the read delay");
          std::cout << transport << ' ' << frame_kind << " pongs=" << peer.pongs << " close_reason="
                    << (peer.reason == fw::stream_close_reason_t::heartbeat_timeout
                          ? "HeartbeatTimeout"
                        : peer.reason ? "unexpected"
                                      : "none")
                    << std::endl;
          if (partial)
              require (peer.pongs == 0
                         && peer.reason == fw::stream_close_reason_t::heartbeat_timeout,
                       "Partial ping must still expire with HeartbeatTimeout");
          else
              require (peer.pongs == 1 && !peer.reason && !peer.closed,
                       "A complete ping ready before expiry must get a pong");
      });
}
}

int main (int argc, char **argv)
{
    try {
        const fw::completion_owner_t completion_owner;
        detail::configure_stream_dispatch_executor ();
        struct dispatch_executor_guard_t
        {
            ~dispatch_executor_guard_t () { detail::shutdown_stream_dispatch_executor (); }
        } dispatch_executor_guard;
        require (argc == 3, "Expected a transport and a frame kind");
        const std::string_view transport = argv[1];
        const std::string_view frame_kind = argv[2];
        require (
          frame_kind == "complete" || frame_kind == "partial"
            || (transport == "tcp"
                && (frame_kind == "complete-slow-read" || frame_kind == "partial-slow-read"
                    || frame_kind == "complete-available" || frame_kind == "partial-available")),
          "Expected complete or partial, or a TCP slow-read or available case");
        check_reader_expiry (transport, frame_kind);
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << "FAIL " << error.what () << std::endl;
        return 1;
    }
}
