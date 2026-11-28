/* SPDX-License-Identifier: Elastic-2.0 */
#include "../support/stream_wire_peer.hpp"

#include "runtime/host/app_test_access.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/streams/admission_queue_messages.hpp"

#include <zlink/framework.hpp>

#include <cstdlib>
#include <atomic>
#include <future>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <syncstream>
#include <thread>

namespace
{
namespace fw = zlink::framework;
namespace queue = fw::runtime::admission_queue;
using fw::detail::stream_header_flags_t;
using fw::detail::stream_header_t;
using fw::detail::stream_message_kind_t;
using fw::tests::stream_wire::require;
using fw::tests::stream_wire::wire_peer_t;
using fw::tests::stream_wire::wire_read_deadline_t;
using queue::json_t;
using namespace std::chrono_literals;

constexpr char mesh_name[] = "queue-test-mesh";
constexpr char stream_name[] = "queue-test-stream";
constexpr char authentication[] = "login";
constexpr char probe[] = "probe";
constexpr auto host_startup_timeout = 10s;

class session_t final : public fw::packet_stream_session_t
{
  public:
    static inline std::atomic_int entries{0};
    static inline std::atomic_int active{0};
    static inline std::atomic_int peak{0};
    static inline std::promise<void> first_entered;
    static inline fw::task_completion_source_t<void> release_first;

    fw::task_t<void> on_connected (fw::stream_t &) override { co_return; }
    fw::task_t<void> on_disconnected (fw::stream_t &) override { co_return; }
    fw::task_t<void> on_error (fw::stream_t &, const fw::stream_error_t &) override { co_return; }

    fw::task_t<void> on_packet (fw::stream_t &stream,
                                const fw::session_message_context_t &context,
                                const fw::message_t &) override
    {
        require (context.packet_name == authentication || context.packet_name == probe,
                 "queue packet reached application");
        const bool authenticating = context.packet_name == authentication;
        const auto finish = std::shared_ptr<void> (nullptr, [authenticating] (void *) {
            if (authenticating)
                --active;
        });
        if (authenticating) {
            const auto entry = ++entries;
            const auto count = ++active;
            auto maximum = peak.load ();
            while (maximum < count && !peak.compare_exchange_weak (maximum, count)) {
            }
            if (entry == 1) {
                first_entered.set_value ();
                co_await release_first.task ();
            }
        }
        json_t payload = {{"admitted", true}};
        const auto response = fw::message_t::from (std::move (payload));
        auto reply = stream.reply_packet (response);
        co_await reply.async ();
    }
};

class host_t
{
  public:
    host_t (const char *rid,
            fw::object_role_t role,
            std::shared_ptr<fw::location_store_t> store,
            std::shared_ptr<fw::relocation_store_t> relocations,
            bool queue_enabled)
    {
        auto &options = app.add_zlink_framework ();
        options.add_location_store (std::move (store));
        options.add_relocation_store (std::move (relocations));
        auto &locations = options.configure_locations ();
        locations.polling_interval = 10ms;
        locations.owner_lease_renew_interval = 10ms;
        locations.owner_lease_fencing_margin = 1ms;
        if (std::getenv ("ZLINK_CPP_ADMISSION_TEST_TRACE")) {
            auto &dispatch = options.configure_dispatch ();
            dispatch.message_flow (fw::message_flow_log_mode_t::normal);
            fw::detail::dispatch_options_access_t::set_observer_for_tests (
              dispatch, [rid] (const auto &event) {
                  std::osyncstream (std::clog)
                    << rid << ' ' << event.packet_name.value_or ("")
                    << " outcome=" << static_cast<int> (event.outcome)
                    << " flow=" << event.flow_id.value_or ("")
                    << " corr=" << event.correlation_id.value_or ("") << '\n';
              });
            fw::detail::dispatch_options_access_t::set_dispatch_error_observer_for_tests (
              dispatch, [rid] (const auto &event) {
                  if (event.exception) {
                      try {
                          std::rethrow_exception (event.exception);
                      }
                      catch (const std::exception &error) {
                          std::osyncstream (std::clog)
                            << rid << ' ' << event.packet_name.value_or ("")
                            << " error=" << error.what () << '\n';
                      }
                  }
              });
        }
        options.add_route_mesh (mesh_name)
          .set_object_role (role)
          .set_routing_id (zlink::routing_id_t::from (rid))
          .listen ("tcp://127.0.0.1:0");
        options.add_stream_node (stream_name)
          .bind (fw::tests::loopback_tcp_any_port)
          .set_heartbeat (10s, 30s)
          .set_authentication_packet_name (authentication)
          .set_max_concurrent_authentications (3)
          .set_admission_queue_enabled (queue_enabled)
          .set_admission_queue_key ("AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8")
          .set_admission_global_limit (1)
          .register_session<session_t> ();
        _thread = std::thread ([this, rid] {
            std::string command (rid);
            char *argv[] = {command.data (), nullptr};
            (void) app.run (1, argv);
        });
    }

    ~host_t ()
    {
        app.request_stop ();
        if (_thread.joinable ())
            _thread.join ();
    }

    host_t (const host_t &) = delete;
    host_t &operator= (const host_t &) = delete;

    fw::app_t app = fw::app_t::create ();

  private:
    std::thread _thread;
};

template <typename Predicate>
void await (Predicate predicate, const char *message, std::chrono::seconds timeout = 5s)
{
    const auto stop = std::chrono::steady_clock::now () + timeout;
    while (!predicate ()) {
        require (std::chrono::steady_clock::now () < stop, message);
        std::this_thread::sleep_for (10ms);
    }
}

void await_serving (host_t &host, const char *message)
{
    try {
        await ([&] { return host.app.is_ready (); }, message, host_startup_timeout);
    }
    catch (const std::runtime_error &) {
        auto services = host.app.advanced ().services ().build_provider ();
        if (auto query = services.get<fw::location_runtime_query_t> ()) {
            const auto status = query->get ().get_status ().result ();
            if (status && status.value ().last_error)
                throw std::runtime_error (std::string (message) + ": "
                                          + *status.value ().last_error);
        }
        throw;
    }
}

class login_peer_t
{
  public:
    login_peer_t (const std::string &endpoint, const fw::detail::stream_runtime_t &runtime) :
        _peer (endpoint, wire_read_deadline_t::enclosing_test), _runtime (runtime)
    {
    }

    void enqueue ()
    {
        send (1, authentication);
        const auto rejected = _peer.read (_runtime);
        require (rejected.header.kind () == stream_message_kind_t::error
                   && rejected.header.metadata (queue::client_queue_required_key) == "1",
                 "authentication without ticket did not return queueRequired");

        send (2, queue::client_enqueue_packet);
        for (;;) {
            const auto frame = _peer.read (_runtime);
            require (frame.header.kind () != stream_message_kind_t::error,
                     ("enqueue returned an error: " + frame.payload).c_str ());
            if (frame.header.packet_name () == queue::client_ticket_packet) {
                read_ticket (frame.payload);
            } else if (frame.header.kind () == stream_message_kind_t::response
                       && frame.header.request_seq () == 2) {
                if (json_t::parse (frame.payload).contains ("ticket"))
                    read_ticket (frame.payload);
                return;
            }
        }
    }

    void authenticate (std::optional<int> expected_position = std::nullopt)
    {
        bool saw_position = !expected_position;
        while (_ticket.empty ()) {
            const auto frame = _peer.read (_runtime);
            require (frame.header.kind () != stream_message_kind_t::error,
                     ("waiting for ticket returned an error: " + frame.payload).c_str ());
            if (frame.header.packet_name () == queue::client_ticket_packet) {
                require (frame.header.kind () == stream_message_kind_t::send,
                         "report delivery did not arrive as Send");
                read_ticket (frame.payload);
            } else if (expected_position
                       && frame.header.packet_name () == queue::client_position_packet) {
                require (frame.header.kind () == stream_message_kind_t::send,
                         "position report did not arrive as Send");
                saw_position |= json_t::parse (frame.payload).at ("position") == *expected_position;
            }
        }
        require (saw_position, "queued session did not receive its new position before its ticket");
        send (3, authentication, std::move (_ticket));
    }

    void expect_admitted () { expect_response (3); }

    void expect_no_extra_delivery ()
    {
        send (4, probe);
        expect_response (4);
    }

    bool has_ticket () const { return !_ticket.empty (); }

  private:
    void expect_response (std::uint64_t seq)
    {
        for (;;) {
            const auto frame = _peer.read (_runtime);
            require (frame.header.packet_name () != queue::client_ticket_packet
                       && frame.header.packet_name () != queue::client_position_packet,
                     "admitted session received an additional queue delivery");
            if (frame.header.request_seq () != seq)
                continue;
            require (frame.header.kind () == stream_message_kind_t::response
                       && json_t::parse (frame.payload).at ("admitted") == true,
                     ("session request failed: " + frame.payload).c_str ());
            return;
        }
    }

    void send (std::uint64_t seq, const char *name, std::string ticket = {})
    {
        fw::detail::stream_metadata_t metadata;
        if (!ticket.empty ())
            metadata.with (queue::client_ticket_key, std::move (ticket));
        const stream_header_t request (stream_message_kind_t::request, fw::stream_codec_t::json,
                                       stream_header_flags_t::has_request_seq, seq, name,
                                       std::move (metadata));
        _peer.send (_runtime, request, "{}");
    }

    void read_ticket (const std::string &payload)
    {
        require (_ticket.empty (), "session received multiple tickets before authentication");
        _ticket = json_t::parse (payload).at ("ticket").get<std::string> ();
    }

    wire_peer_t _peer;
    const fw::detail::stream_runtime_t &_runtime;
    std::string _ticket;
};

} // namespace

int main (int argc, char **argv)
{
    const bool phase_one_only = argc == 2 && std::string_view (argv[1]) == "--phase-one-control";
    try {
        auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
        auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
        host_t server ("queue-server", fw::object_role_t::server, store, relocations,
                       !phase_one_only);
        await_serving (server, "Object Server did not reach Serving");
        host_t client ("queue-client", fw::object_role_t::client, store, relocations,
                       !phase_one_only);
        await_serving (client, "Object Client did not reach Serving");
        auto client_services = client.app.advanced ().services ().build_provider ();
        auto &mesh = client_services.get_required<fw::route_mesh_runtime_t> ();
        await ([&] { return mesh.snapshot (mesh_name).ready_peer_count == 1; },
               "Object Client cannot route to Object Server");
        if (!phase_one_only) {
            await (
              [&] {
                  const auto server_epoch =
                    fw::detail::app_test_access_t::admission_queue_epoch (server.app);
                  const auto client_epoch =
                    fw::detail::app_test_access_t::admission_queue_epoch (client.app);
                  return server_epoch != 0 && server_epoch == client_epoch;
              },
              "Admission Queue report replies did not establish a shared epoch");
        }
        const auto endpoint = [] (host_t &host) {
            const auto value =
              fw::detail::app_test_access_t::stream_endpoint (host.app, stream_name);
            require (value.has_value (), "missing STREAM listener endpoint");
            return *value;
        };
        const auto server_endpoint = endpoint (server);
        const auto client_endpoint = endpoint (client);
        const auto wire = fw::detail::stream_runtime_t::from (server.app.advanced ().zlink ());
        login_peer_t first (client_endpoint, wire);
        login_peer_t second (server_endpoint, wire);
        login_peer_t third (server_endpoint, wire);
        if (phase_one_only)
            session_t::release_first.complete (fw::result_t<void>::success ());
        const auto entered = session_t::first_entered.get_future ();
        try {
            first.enqueue ();
            first.authenticate ();
            require (entered.wait_for (5s) == std::future_status::ready,
                     "first authentication did not enter the handler");
            second.enqueue ();
            third.enqueue ();
            require (!second.has_ticket () && !third.has_ticket () && session_t::active == 1,
                     "queued sessions received a ticket while the only permit was held");
        }
        catch (const std::exception &) {
            session_t::release_first.complete (fw::result_t<void>::success ());
            throw;
        }
        session_t::release_first.complete (fw::result_t<void>::success ());
        second.authenticate ();
        first.expect_admitted ();
        third.authenticate (1);
        second.expect_admitted ();
        third.expect_admitted ();
        first.expect_no_extra_delivery ();
        second.expect_no_extra_delivery ();
        third.expect_no_extra_delivery ();
        require (session_t::entries == 3 && session_t::peak == 1,
                 "G=1 did not admit all three clients at a peak concurrency of one");
        std::cout
          << "PASS two node G=1: 3 authentications, peak 1, 2 session-bound ticket pushes\n";
    }
    catch (const std::exception &error) {
        std::cerr << "FAIL " << error.what () << '\n';
        return 1;
    }
}
