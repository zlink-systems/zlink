/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_TEST_CONTRACT_ZMP_ENGINE_FIXTURE_HPP
#define ZLINK_TEST_CONTRACT_ZMP_ENGINE_FIXTURE_HPP

#include "contract_socket_pair_fixture.hpp"
#include "testutil_zmp_wire.hpp"
#include "core/io_thread.hpp"
#include "utils/ip.hpp"
#include "core/session_base.hpp"
#include "runtime/engine/asio/asio_zmp_engine.hpp"
#include "runtime/engine/asio/i_asio_transport.hpp"
#include <boost/asio/post.hpp>
#include <deque>

namespace zlink
{
class session_termination_test_access_t
{
  public:
    static options_t options_for (own_t *owner_) { return owner_->options; }
    static own_t *owner_of (own_t *child_) { return child_->_owner; }
    static void finish_plug (own_t *object_) { object_->process_seqnum (); }
    static void terminate (own_t *object_) { object_->terminate (); }
    static bool attached_pipe_connection_ids_are_live (socket_base_t *socket_, size_t expected_)
    {
        std::vector<pipe_t *> pipes;
        socket_->snapshot_attached_pipes (&pipes);
        if (pipes.size () != expected_)
            return false;
        for (size_t i = 0; i != pipes.size (); ++i) {
            const uint64_t live = pipes[i]->get_transport_connection_id ();
            if (live == 0 || pipes[i]->get_endpoint_pair ().connection_id != live)
                return false;
        }
        return true;
    }
    static socket_monitor_runtime_t &monitor_for (socket_base_t *socket_)
    {
        TEST_ASSERT_NOT_NULL (socket_);
        return socket_->monitor_runtime ();
    }
};
}

// Exercise the monitor producer and its real queue without starting the public
// monitor's independent command executor or dispatch worker.
struct contract_zmp_monitor_t
{
    contract_zmp_monitor_t (void *socket_, uint64_t events_) :
        runtime (
          zlink::session_termination_test_access_t::monitor_for (as_socket_handle (socket_).socket))
    {
        runtime.reset_worker_state (UINT64_MAX, sizeof (zlink::socket_monitor_event_record_t));
        runtime.events = events_;
        runtime.events_atomic.store (events_, std::memory_order_release);
    }
    ~contract_zmp_monitor_t ()
    {
        runtime.events_atomic.store (0, std::memory_order_release);
        runtime.events = 0;
        runtime.stop_task ();
    }
    bool next (zlink::socket_monitor_event_record_t *record_)
    {
        if (!runtime.dequeue_worker_event_nowait (record_))
            return false;
        runtime.complete_worker_event ();
        return true;
    }
    zlink::socket_monitor_runtime_t &runtime;
};

// The real engine owns this transport. Tests supply input and acknowledge
// output explicitly; transport I/O uses no descriptor, clock, or network.
// The engine constructor still requires an unconnected descriptor for its
// peer-address and nonblocking setup; that descriptor never carries data.
struct contract_zmp_transport_state_t
{
    contract_zmp_transport_state_t (bool messages_, bool encrypted_) :
        io (NULL),
        opened (false),
        messages (messages_),
        encrypted (encrypted_),
        hold_writes (false),
        read_buffer (NULL),
        read_capacity (0)
    {
    }
    boost::asio::io_context *io;
    bool opened, messages, encrypted, hold_writes;
    unsigned char *read_buffer;
    size_t read_capacity;
    zlink::i_asio_transport::completion_handler_t read_handler;
    std::deque<std::vector<unsigned char>> incoming, outgoing;
    std::deque<std::pair<zlink::i_asio_transport::completion_handler_t, size_t>> writes;

    void deliver ()
    {
        if (!opened || !read_handler || incoming.empty ())
            return;
        std::vector<unsigned char> &front = incoming.front ();
        const size_t size = std::min (front.size (), read_capacity);
        TEST_ASSERT_TRUE (!messages || size == front.size ());
        memcpy (read_buffer, &front[0], size);
        front.erase (front.begin (), front.begin () + size);
        if (front.empty ())
            incoming.pop_front ();
        zlink::i_asio_transport::completion_handler_t handler = read_handler;
        read_handler = zlink::i_asio_transport::completion_handler_t ();
        boost::asio::post (*io, [handler, size] { handler (boost::system::error_code (), size); });
    }
    void feed (const std::vector<unsigned char> &bytes_)
    {
        TEST_ASSERT_TRUE (opened);
        incoming.push_back (bytes_);
        deliver ();
    }
    void drain_writes ()
    {
        while (!writes.empty ()) {
            const std::pair<zlink::i_asio_transport::completion_handler_t, size_t> next =
              writes.front ();
            writes.pop_front ();
            boost::asio::post (*io,
                               [next] { next.first (boost::system::error_code (), next.second); });
        }
    }
};

struct contract_zmp_wire_frame_t
{
    unsigned char flags, kind;
    uint64_t sequence;
    std::vector<unsigned char> body;
};

inline std::vector<contract_zmp_wire_frame_t>
contract_zmp_take_output (contract_zmp_transport_state_t &state_)
{
    std::vector<unsigned char> bytes;
    while (!state_.outgoing.empty ()) {
        test_zmp_wire::append_wire_frame (&bytes, state_.outgoing.front ());
        state_.outgoing.pop_front ();
    }
    std::vector<contract_zmp_wire_frame_t> frames;
    for (size_t offset = 0; offset < bytes.size ();) {
        TEST_ASSERT_TRUE (bytes.size () - offset >= test_zmp_wire::zmp_header_size);
        const unsigned char *data = &bytes[offset];
        TEST_ASSERT_EQUAL_UINT8 (test_zmp_wire::zmp_magic, data[0]);
        TEST_ASSERT_EQUAL_UINT8 (test_zmp_wire::zmp_version, data[1]);
        const bool extended = test_zmp_wire::zmp_is_request_reply_kind (data[3]);
        const size_t header =
          extended ? test_zmp_wire::zmp_request_reply_header_size : test_zmp_wire::zmp_header_size;
        const size_t size = test_zmp_wire::get_uint32 (&data[4]);
        TEST_ASSERT_TRUE (header + size <= bytes.size () - offset);
        contract_zmp_wire_frame_t frame;
        frame.flags = data[2];
        frame.kind = data[3];
        frame.sequence = extended ? test_zmp_wire::get_uint64 (&data[8]) : 0;
        frame.body.assign (data + header, data + header + size);
        frames.push_back (frame);
        offset += header + size;
    }
    return frames;
}

class contract_zmp_transport_t : public zlink::i_asio_transport
{
  public:
    explicit contract_zmp_transport_t (
      const std::shared_ptr<contract_zmp_transport_state_t> &state_) :
        state (state_)
    {
    }
    bool open (boost::asio::io_context &io_, zlink::fd_t) ZLINK_OVERRIDE
    {
        state->io = &io_;
        state->opened = true;
        return true;
    }
    bool is_open () const ZLINK_OVERRIDE { return state->opened; }
    void close () ZLINK_OVERRIDE
    {
        state->opened = false;
        state->read_handler = completion_handler_t ();
        state->writes.clear ();
    }
    void async_read_some (unsigned char *buffer_,
                          size_t size_,
                          completion_handler_t handler_) ZLINK_OVERRIDE
    {
        TEST_ASSERT_FALSE (bool (state->read_handler));
        state->read_buffer = buffer_;
        state->read_capacity = size_;
        state->read_handler = handler_;
        state->deliver ();
    }
    size_t read_some (unsigned char *, size_t) ZLINK_OVERRIDE
    {
        errno = EAGAIN;
        return 0;
    }
    bool supports_speculative_write () const ZLINK_OVERRIDE { return false; }
    bool has_message_boundaries () const ZLINK_OVERRIDE { return state->messages; }
    bool is_encrypted () const ZLINK_OVERRIDE { return state->encrypted; }
    const char *name () const ZLINK_OVERRIDE { return "unit-memory"; }
    void async_write_some (const unsigned char *buffer_,
                           size_t size_,
                           completion_handler_t handler_) ZLINK_OVERRIDE
    {
        state->outgoing.push_back (std::vector<unsigned char> (buffer_, buffer_ + size_));
        state->writes.push_back (std::make_pair (handler_, size_));
        if (!state->hold_writes)
            state->drain_writes ();
    }
    size_t write_some (const unsigned char *, size_t) ZLINK_OVERRIDE
    {
        errno = EAGAIN;
        return 0;
    }

  private:
    std::shared_ptr<contract_zmp_transport_state_t> state;
};

class contract_zmp_session_t : public zlink::session_base_t
{
  public:
    contract_zmp_session_t (zlink::io_thread_t *io_,
                            zlink::socket_base_t *socket_,
                            const zlink::options_t &options_,
                            bool *alive_) :
        session_base_t (io_, false, socket_, options_, NULL), alive (alive_)
    {
        *alive = true;
    }
    ~contract_zmp_session_t () ZLINK_OVERRIDE { *alive = false; }
    void stop () { terminate (); }

  private:
    bool *alive;
};

struct contract_zmp_engine_t
{
    contract_zmp_engine_t (void *socket_, bool messages_ = false, bool encrypted_ = false) :
        core (as_socket_handle (socket_).socket),
        io (core->get_ctx (), core->get_tid ()),
        state (new contract_zmp_transport_state_t (messages_, encrypted_)),
        alive (false),
        descriptor (zlink::open_socket (AF_INET, SOCK_STREAM, IPPROTO_TCP))
    {
        TEST_ASSERT_NOT_EQUAL (zlink::retired_fd, descriptor);
        zlink::options_t options = zlink::session_termination_test_access_t::options_for (core);
        options.handshake_ivl = 0;
        options.linger.store (0);
        options.transport_pair_initiator = false;
        session = new contract_zmp_session_t (&io, core, options, &alive);
        std::unique_ptr<zlink::i_asio_transport> transport (new contract_zmp_transport_t (state));
        zlink::asio_zmp_engine_t *engine = new zlink::asio_zmp_engine_t (
          descriptor, options, zlink::make_unconnected_bind_endpoint_pair ("unit://memory"),
          std::move (transport));
        zlink::command_t command = {};
        command.destination = session;
        command.type = zlink::command_t::attach;
        command.args.attach.engine = engine;
        session->inc_seqnum ();
        session->process_command (command);
        pump ();
    }
    ~contract_zmp_engine_t ()
    {
        if (alive)
            session->stop ();
        pump ();
        TEST_ASSERT_FALSE (alive);
        close (descriptor);
    }
    void pump ()
    {
        size_t work;
        do {
            io.get_io_context ().restart ();
            work = io.get_io_context ().poll ();
            work += contract_socket_pair_t::pump_owner (core);
        } while (work != 0);
    }
    void feed (const std::vector<unsigned char> &bytes_)
    {
        state->feed (bytes_);
        pump ();
    }
    bool transfer_to (contract_zmp_engine_t &peer_)
    {
        pump ();
        const bool available = !state->outgoing.empty ();
        while (!state->outgoing.empty ()) {
            const std::vector<unsigned char> bytes = state->outgoing.front ();
            state->outgoing.pop_front ();
            peer_.feed (bytes);
        }
        return available;
    }
    void handshake (unsigned char peer_type_,
                    const char *rid_,
                    unsigned char lanes_ = 1,
                    unsigned char lane_ = 0)
    {
        std::vector<unsigned char> hello;
        hello.push_back (test_zmp_wire::zmp_control_hello);
        hello.push_back (peer_type_);
        hello.push_back (static_cast<unsigned char> (strlen (rid_)));
        hello.insert (hello.end (), rid_, rid_ + strlen (rid_));
        feed (test_zmp_wire::control_frame (hello));
        std::vector<unsigned char> ready (1, test_zmp_wire::zmp_control_ready);
        const char *type = peer_type_ == test_zmp_wire::socket_dealer   ? "DEALER"
                           : peer_type_ == test_zmp_wire::socket_router ? "ROUTER"
                                                                        : "PAIR";
        test_zmp_wire::zmp_metadata::append_property (ready, "Socket-Type", type, strlen (type));
        test_zmp_wire::zmp_metadata::append_property (ready, "Routing-Id", rid_, strlen (rid_));
        const unsigned char maximum[8] = {};
        test_zmp_wire::zmp_metadata::append_property (ready, "Zlink-Max-Message-Size", maximum, 8);
        if (peer_type_ == test_zmp_wire::socket_dealer
            || peer_type_ == test_zmp_wire::socket_router) {
            test_zmp_wire::zmp_metadata::append_property (ready, "Zlink-Lane", &lane_, 1);
            test_zmp_wire::zmp_metadata::append_property (ready, "Zlink-Lane-Count", &lanes_, 1);
        }
        feed (test_zmp_wire::control_frame (ready));
    }
    zlink::socket_base_t *core;
    zlink::io_thread_t io;
    std::shared_ptr<contract_zmp_transport_state_t> state;
    bool alive;
    zlink::fd_t descriptor;
    contract_zmp_session_t *session;
};
#endif
