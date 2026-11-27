/* SPDX-License-Identifier: Elastic-2.0 */
#include "../support/stream_liveness_fixture.hpp"

#include <future>

namespace
{
using namespace stream_liveness_test;

void check_reader_expiry (std::string_view transport, bool partial)
{
    with_stream_host (
      transport, 0ms,
      [&] (auto &host, const auto &runtime, const auto &endpoint, auto &, auto &, auto &) {
          auto paused = std::make_shared<std::promise<void>> ();
          std::promise<void> release;
          const auto reader_released = release.get_future ().share ();
          auto reader_paused = paused->get_future ();
          rt::stream_host_service_test_access_t::observe_shutdown (
            host, {[paused, reader_released] (auto, auto) {
                       paused->set_value ();
                       reader_released.wait ();
                   },
                   {}});
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

          ping_only_client_t client (endpoint, runtime);
          require (reader_paused.wait_for (5s) == std::future_status::ready,
                   "Reader did not reach the registered hook");
          const auto deadline = steady_clock::now () + receive_timeout;
          const auto &peer = client.observation ();
          client.send_ping (partial);
          while (!peer.writes && !peer.write_error && steady_clock::now () < deadline)
              client.advance ();
          require (peer.writes == 1 && !peer.pongs && !peer.closing,
                   "Ping bytes were not written while the reader was paused");

          // The timer was armed before the registered hook. Releasing after the
          // full timeout puts both the timer and the prewritten bytes in scope.
          std::this_thread::sleep_until (deadline);
          release.set_value ();
          reader_guard.pending = false;
          const auto result_deadline = steady_clock::now () + 5s;
          while (!peer.pongs && !peer.reason && !peer.closed
                 && steady_clock::now () < result_deadline)
              client.advance ();
          std::cout << transport << (partial ? " partial" : " complete") << " pongs=" << peer.pongs
                    << " close_reason="
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
        check_reader_expiry (argv[1], std::string_view (argv[2]) == "partial");
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << "FAIL " << error.what () << std::endl;
        return 1;
    }
}
