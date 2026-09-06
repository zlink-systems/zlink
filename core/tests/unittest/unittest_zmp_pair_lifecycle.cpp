/* SPDX-License-Identifier: MPL-2.0 */
#include "testutil.hpp"
#include "testutil_unity.hpp"
#include "contract_zmp_engine_fixture.hpp"
#include "core/ctx.hpp"
#include "transports/tcp/asio_tcp_connecter.hpp"
SETUP_TEARDOWN_TESTCONTEXT

// Expose protected base lifecycle operations through ordinary C++ member
// pointers. No object is cast to a fabricated derived instance.
class worker_access_t : public zlink::worker_poller_base_t
{
  public:
    static void join (zlink::worker_poller_base_t *worker_)
    {
        const auto stop = &worker_access_t::stop_worker;
        (worker_->*stop) ();
    }
};
class timer_access_t : public zlink::asio_engine_t
{
  public:
    static void expire (zlink::asio_engine_t *engine_)
    {
        const auto timer = &timer_access_t::on_timer;
        (engine_->*timer) (handshake_timer_id, boost::system::error_code ());
    }
};
struct active_driver_t
{
    struct peer_t
    {
        std::shared_ptr<contract_zmp_transport_state_t> transport;
        zlink::asio_zmp_engine_t *engine;
        zlink::session_base_t *session;
        zlink::fd_t descriptor;
    };
    explicit active_driver_t (void *socket_) : core (as_socket_handle (socket_).socket)
    {
        // Stop both actual context IO workers before any connect intent exists.
        // Their registered mailboxes and normal affinity selection are retained.
        for (size_t i = 0; i != 2; ++i) {
            zlink::io_thread_t *io = core->get_ctx ()->choose_io_thread (uint64_t (1) << i);
            TEST_ASSERT_NOT_NULL (io);
            io->stop ();
            worker_access_t::join (io->get_poller ());
            io->get_io_context ().restart ();
            ios.push_back (io);
        }
        TEST_ASSERT_NOT_EQUAL (ios[0], ios[1]);
    }
    ~active_driver_t ()
    {
        for (size_t i = 0; i != peers.size (); ++i) {
            TEST_ASSERT_FALSE (peers[i].transport->opened);
            close (peers[i].descriptor);
        }
        // All sessions were disconnected and drained while the socket was
        // alive. Context teardown destroys the already joined IO workers.
    }
    size_t drain_mailboxes ()
    {
        size_t work = 0;
        size_t turn;
        do {
            turn = 0;
            for (size_t i = 0; i != ios.size (); ++i) {
                zlink::command_t command;
                while (ios[i]->get_mailbox ()->recv (&command, 0) == 0) {
                    ++work;
                    ++turn;
                    zlink::asio_tcp_connecter_t *connector =
                      command.type == zlink::command_t::plug
                        ? dynamic_cast<zlink::asio_tcp_connecter_t *> (command.destination)
                        : NULL;
                    if (connector) {
                        // Substitute only the connector's transport result. Normal
                        // connect/session/child ownership and the reserved plug
                        // seqnum remain in the real Core.
                        zlink::session_base_t *session = static_cast<zlink::session_base_t *> (
                          zlink::session_termination_test_access_t::owner_of (connector));
                        zlink::session_termination_test_access_t::finish_plug (connector);
                        attach (session);
                        zlink::session_termination_test_access_t::terminate (connector);
                    } else
                        command.destination->process_command (command);
                }
            }
        } while (turn);
        return work;
    }
    void pump ()
    {
        size_t work;
        do {
            work = 0;
            for (size_t i = 0; i != ios.size (); ++i) {
                // Every handler can create a connector on either context.
                // Consume its plug before running another mailbox callback.
                work += contract_socket_pair_t::pump_owner (core);
                work += drain_mailboxes ();
                ios[i]->get_io_context ().restart ();
                work += ios[i]->get_io_context ().poll_one ();
            }
        } while (work);
    }
    void attach (zlink::session_base_t *session_)
    {
        peer_t peer;
        peer.session = session_;
        peer.transport.reset (new contract_zmp_transport_state_t (false, false));
        peer.descriptor = zlink::open_socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
        zlink::options_t options = zlink::session_termination_test_access_t::options_for (session_);
        options.handshake_ivl = 0;
        std::unique_ptr<zlink::i_asio_transport> transport (
          new contract_zmp_transport_t (peer.transport));
        peer.engine = new zlink::asio_zmp_engine_t (
          peer.descriptor, options,
          zlink::make_unconnected_connect_endpoint_pair ("tcp://127.0.0.1:1"),
          std::move (transport));
        zlink::command_t command = {};
        command.destination = session_;
        command.type = zlink::command_t::attach;
        command.args.attach.engine = peer.engine;
        session_->inc_seqnum ();
        session_->process_command (command);
        peers.push_back (peer);
    }
    void hello (size_t index_)
    {
        std::vector<unsigned char> hello;
        hello.push_back (1);
        hello.push_back (test_zmp_wire::socket_router);
        const char rid[] = "owner-timeout-peer";
        hello.push_back (sizeof (rid) - 1);
        hello.insert (hello.end (), rid, rid + sizeof (rid) - 1);
        peers[index_].transport->feed (test_zmp_wire::control_frame (hello));
        pump ();
    }
    zlink::socket_base_t *core;
    std::vector<zlink::io_thread_t *> ios;
    std::vector<peer_t> peers;
};

struct owner_after_claim_gate_t
{
    owner_after_claim_gate_t () : entered (false), connection (0), pair (0), generation (0) {}
    bool entered;
    uint64_t connection, pair, generation;
};
bool defer_first_owner_after_claim (uint64_t connection_,
                                    uint64_t pair_,
                                    uint64_t generation_,
                                    void *data_)
{
    owner_after_claim_gate_t *gate = static_cast<owner_after_claim_gate_t *> (data_);
    if (gate->entered)
        return false;
    gate->entered = true;
    gate->connection = connection_;
    gate->pair = pair_;
    gate->generation = generation_;
    return true;
}

void assert_hello (active_driver_t::peer_t &peer_)
{
    const std::vector<contract_zmp_wire_frame_t> frames =
      contract_zmp_take_output (*peer_.transport);
    TEST_ASSERT_EQUAL_UINT (1, frames.size ());
    TEST_ASSERT_TRUE ((frames[0].flags & test_zmp_wire::zmp_flag_control) != 0);
    TEST_ASSERT_FALSE (frames[0].body.empty ());
    TEST_ASSERT_EQUAL_UINT8 (test_zmp_wire::zmp_control_hello, frames[0].body[0]);
}

unsigned char ready_lane (active_driver_t::peer_t &peer_)
{
    const std::vector<contract_zmp_wire_frame_t> frames =
      contract_zmp_take_output (*peer_.transport);
    for (size_t i = 0; i != frames.size (); ++i) {
        if (!(frames[i].flags & test_zmp_wire::zmp_flag_control) || frames[i].body.empty ()
            || frames[i].body[0] != test_zmp_wire::zmp_control_ready)
            continue;
        test_zmp_wire::zmp_metadata::properties_t properties;
        TEST_ASSERT_SUCCESS_ERRNO (test_zmp_wire::zmp_metadata::parse (
          &frames[i].body[1], frames[i].body.size () - 1, properties));
        TEST_ASSERT_EQUAL_UINT (1, properties["Zlink-Lane-Count"].size ());
        TEST_ASSERT_EQUAL_UINT (1, properties["Zlink-Lane"].size ());
        TEST_ASSERT_EQUAL_UINT8 (2, properties["Zlink-Lane-Count"][0]);
        return static_cast<unsigned char> (properties["Zlink-Lane"][0]);
    }
    TEST_FAIL_MESSAGE ("READY reply was not emitted");
    return 0xff;
}

void send_ready (active_driver_t &driver_, size_t index_, unsigned char lane_)
{
    std::vector<unsigned char> body (1, test_zmp_wire::zmp_control_ready);
    const char rid[] = "owner-timeout-peer";
    const unsigned char lanes = 2;
    test_zmp_wire::zmp_metadata::append_property (body, "Socket-Type", "ROUTER", 6);
    test_zmp_wire::zmp_metadata::append_property (body, "Routing-Id", rid, sizeof (rid) - 1);
    test_zmp_wire::zmp_metadata::append_property (body, "Zlink-Lane-Count", &lanes, 1);
    test_zmp_wire::zmp_metadata::append_property (body, "Zlink-Lane", &lane_, 1);
    driver_.peers[index_].transport->feed (test_zmp_wire::control_frame (body));
    driver_.pump ();
}

void test_owner_timeout_before_commit_leaves_no_stale_completion_child ()
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_set (get_test_context (), ZLINK_IO_THREADS, 2));
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    const int zero = 0;
    const int handshake_ivl = 1000;
    const int reconnect_ivl = 10;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_option (router, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (router, ZLINK_OPT_HANDSHAKE_IVL, &handshake_ivl, sizeof (handshake_ivl)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (router, ZLINK_OPT_RECONNECT_IVL, &reconnect_ivl, sizeof (reconnect_ivl)));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_set_routing_id (router, "owner-timeout-local",
                                                                  strlen ("owner-timeout-local")));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_set_router_option (
                                              router, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
                                              "owner-timeout-peer", strlen ("owner-timeout-peer")));
    {
        active_driver_t driver (router);
        contract_zmp_monitor_t monitor (router, ZLINK_EVENT_CONNECTION_READY);
        TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (router, "tcp://127.0.0.1:1"));
        driver.pump ();
        TEST_ASSERT_EQUAL_UINT (1, driver.peers.size ());
        assert_hello (driver.peers[0]);
        owner_after_claim_gate_t gate;
        zlink::test_set_transport_pair_owner_after_claim_hook (&defer_first_owner_after_claim,
                                                               &gate);
        driver.hello (0);
        TEST_ASSERT_TRUE (gate.entered);
        TEST_ASSERT_EQUAL_UINT64 (gate.connection,
                                  driver.peers[0].engine->get_endpoint ().connection_id.load ());
        TEST_ASSERT_EQUAL_UINT64 (gate.pair, driver.peers[0].session->transport_pair_id ());
        TEST_ASSERT_EQUAL_UINT64 (gate.generation,
                                  driver.peers[0].session->transport_pair_generation ());
        // Invoke the real expiry callback at the exact after-claim/before-commit
        // boundary. The connector result is then supplied by the same fixture.
        timer_access_t::expire (driver.peers[0].engine);
        driver.pump ();
        TEST_ASSERT_FALSE (driver.peers[0].transport->opened);
        TEST_ASSERT_EQUAL_UINT (2, driver.peers.size ());
        TEST_ASSERT_TRUE (driver.core->test_resume_deferred_transport_pair_owner_request ());
        zlink::test_set_transport_pair_owner_after_claim_hook (NULL, NULL);
        driver.pump ();
        // With the replacement HELLO unanswered, an extra connector here could
        // only belong to the canceled generation's stale Completion child.
        TEST_ASSERT_EQUAL_UINT (2, driver.peers.size ());
        assert_hello (driver.peers[1]);
        driver.hello (1);
        TEST_ASSERT_EQUAL_UINT (3, driver.peers.size ());
        assert_hello (driver.peers[2]);
        driver.hello (2);
        const unsigned char application_lane = ready_lane (driver.peers[1]);
        const unsigned char completion_lane = ready_lane (driver.peers[2]);
        TEST_ASSERT_TRUE ((application_lane == 0 && completion_lane == 1)
                          || (application_lane == 1 && completion_lane == 0));
        send_ready (driver, 1, application_lane);
        send_ready (driver, 2, completion_lane);
        zlink::socket_monitor_event_record_t ready;
        TEST_ASSERT_TRUE (monitor.next (&ready));
        TEST_ASSERT_EQUAL_UINT64 (ZLINK_EVENT_CONNECTION_READY, ready.event);
        TEST_ASSERT_EQUAL_UINT (3, driver.peers.size ());
        TEST_ASSERT_TRUE (
          zlink::session_termination_test_access_t::attached_pipe_connection_ids_are_live (
            driver.core, 2));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_disconnect (router, "tcp://127.0.0.1:1"));
        driver.pump ();
    }
    test_context_socket_close_zero_linger (router);
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_owner_timeout_before_commit_leaves_no_stale_completion_child);
    return UNITY_END ();
}
