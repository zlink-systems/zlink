/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/stream_connector/contracts/connector.hpp>

#include "runtime/transport/transport_connection.hpp"

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <typeindex>
#include <vector>

namespace zlink::stream_connector::detail
{

class actor_access_t
{
  public:
    static std::shared_ptr<actor_t>
    create (std::shared_ptr<void> connector_state, std::string actor_id, std::uint16_t actor_slot);
    static void close (const std::shared_ptr<actor_t> &actor);
    static std::uint16_t slot (const std::shared_ptr<actor_t> &actor);
};

boost::asio::io_context &shared_io_context ();
boost::asio::io_context &shared_callback_io_context ();
bool configure_shared_runtime_worker_count (std::size_t worker_count);

struct pending_send_t
{
    packet_t packet;
    std::function<void (result_t<void>)> callback;
};

struct pending_write_t
{
    std::vector<std::uint8_t> frame;
    std::function<void (result_t<void>)> callback;
    std::uint64_t write_id = 0;
};

struct pending_request_t
{
    std::uint64_t request_seq = 0;
    packet_t packet;
    std::function<void (result_t<request_reply_t>)> callback;
    std::shared_ptr<boost::asio::steady_timer> timeout_timer;
    // Internal sync-over-async bridges set this: their callback only resolves
    // a promise, so it must run directly instead of riding the user delivery
    // queue (which the blocked caller can never dispatch in queued mode).
    bool deliver_direct = false;
};

/* Lock order for connector_state_t (see the mutex members below).
 *
 *     transport_mutex  ->  lifecycle_mutex
 *                      ->  delivery_mutex
 *                      ->  received_counts_mutex
 *
 * transport_mutex is the only outer lock. lifecycle_mutex, delivery_mutex and
 * received_counts_mutex are leaves: none of them is ever held while another of
 * the three is taken, and none of them is ever held while transport_mutex is
 * taken. Acquiring them in any other order would close a cycle.
 *
 * No user code runs under any of these locks. A wait predicate, a packet
 * handler, a state handler or an error handler can call back into the
 * connector surface, and every one of those entry points takes
 * transport_mutex; std::mutex is not recursive, so such a call deadlocks. The
 * runtime therefore copies the handler list (or the candidate predicates)
 * under the lock, releases it, and only then runs the user callable.
 * Compression and frame encoding stay outside transport_mutex for the same
 * reason in reverse: they are slow, and the read pump needs that lock. */

/* Handler registrations carry an id so a subscription_t can remove exactly its
 * own registration (stream-connector §7). */
struct packet_handler_entry_t
{
    std::uint64_t id = 0;
    std::function<void (const packet_t &)> handler;
};

struct actor_lifecycle_delivery_t
{
    bool bound = false;
    std::function<void ()> callback;
};

template <typename THandler> struct handler_entry_t
{
    std::uint64_t id = 0;
    THandler handler;
};

struct pending_wait_t
{
    std::uint64_t wait_id = 0;
    std::string packet_name;
    std::function<bool (const packet_t &)> predicate;
    std::function<void (result_t<packet_t>)> callback;
    std::shared_ptr<boost::asio::steady_timer> timeout_timer;
};

class connector_state_t : public std::enable_shared_from_this<connector_state_t>
{
  public:
    explicit connector_state_t (connector_options_t options) :
        connector_id (next_connector_id.fetch_add (1, std::memory_order_relaxed)),
        diagnostics_level_cell (options.diagnostics_level),
        options (std::move (options)),
        io_context (shared_io_context ()),
        write_strand (boost::asio::make_strand (io_context)),
        delivery_strand (boost::asio::make_strand (shared_callback_io_context ()))
    {
    }

    inline static std::atomic_uint64_t next_connector_id{1};
    std::uint64_t connector_id = 0;
    // Live diagnostics level cell (message-flow-tracing §4.1, stream-connector
    // §13): seeded from options.diagnostics_level at construction, then read
    // and written independently of options so connector_t::diagnostics_level()
    // / set_diagnostics_level() can change it without recreating the
    // connector. Each processing point loads this once and uses that single
    // value for the whole operation; it never re-reads mid-operation.
    std::atomic<diagnostics_level_t> diagnostics_level_cell;
    connector_options_t options;
    connection_state_t state = connection_state_t::created;
    std::uint64_t next_request_seq = 1;
    std::uint64_t next_wait_id = 1;
    std::map<std::uint64_t, pending_request_t> pending_requests;
    std::map<std::uint64_t, pending_wait_t> pending_waits;
    /* Bumped on every insertion into and removal from pending_waits. A matcher
     * that evaluates user predicates outside transport_mutex re-reads this
     * before it concludes "no wait matches", so a wait registered during the
     * unlocked evaluation is not missed. */
    std::uint64_t pending_waits_version = 0;
    std::deque<pending_send_t> pending_sends;
    std::deque<pending_write_t> pending_writes;
    std::optional<pending_write_t> active_write;
    std::vector<std::uint8_t> inbound_buffer;
    std::deque<packet_t> dispatch_queue;
    /* Bumped whenever dispatch_queue is dropped wholesale (a new connection,
     * or close). A scan that evaluates user predicates outside transport_mutex
     * re-reads this before putting the packets it did not take back, so a
     * connection boundary crossed during the scan still drops them
     * (stream-connector §10). */
    std::uint64_t dispatch_queue_generation = 0;
    std::deque<std::function<void ()>> delivery_queue;
    std::deque<actor_lifecycle_delivery_t> actor_lifecycle_delivery_queue;
    std::vector<packet_t> sent_packets;
    std::map<std::string, std::vector<packet_handler_entry_t>> packet_handlers;
    std::vector<handler_entry_t<std::function<void (const connection_state_changed_t &)>>>
      state_handlers;
    std::vector<handler_entry_t<std::function<void (const error_t &)>>> error_handlers;
    std::vector<handler_entry_t<std::function<void (std::optional<close_reason_t>)>>>
      disconnected_handlers;
    std::map<std::uint16_t, std::shared_ptr<actor_t>> actors_by_slot;
    std::map<std::string, std::shared_ptr<actor_t>, std::less<>> actors_by_id;
    std::vector<handler_entry_t<std::function<void (const std::shared_ptr<actor_t> &)>>>
      actor_bound_handlers;
    std::vector<handler_entry_t<std::function<void (const std::shared_ptr<actor_t> &)>>>
      actor_unbound_handlers;
    std::atomic_uint64_t next_subscription_id{1};
    /* Per-name receive counts for the current connection (stream-connector
     * §10). Guarded by its own mutex because the frame decode paths that
     * update it do not all hold transport_mutex. */
    /* Lock order: leaf. Taken under transport_mutex, never the other way, and
     * never while lifecycle_mutex or delivery_mutex is held. */
    mutable std::mutex received_counts_mutex;
    std::map<std::string, std::size_t, std::less<>> received_counts;
    bool connect_started = false;
    codec_t default_codec = codec_t::json;
    std::set<codec_t> enabled_codecs{codec_t::json};
    std::shared_ptr<const compression_codec_t> compression_codec;
    bool lz4_enabled = false;
    std::atomic_bool close_requested{false};
    bool send_in_progress = false;
    bool write_in_progress = false;
    std::uint64_t next_write_id = 1;
    bool request_pump_scheduled = false;
    bool read_in_progress = false;
    std::optional<error_t> inbound_error;
    // Last error that drove a disconnect/close transition. Synchronous waiters
    // that observe the transport already down report this instead of a generic
    // "not connected" when the pump consumed the inbound error first.
    std::optional<error_t> last_disconnect_error;
    /* Last observed session close reason (graceful-drain-handoff §7.1):
     * stored from a received session-closing control, or synthesized as
     * client_close/transport_error when no control arrived. */
    std::optional<close_reason_t> last_close_reason;
    /* Reason announced by a session-closing control before the socket goes
     * away. Consumed by the next disconnect transition; last_close_reason then
     * keeps that value and is never cleared (stream-connector §6.2). */
    std::optional<close_reason_t> pending_close_reason;
    /* Transport resolved from the endpoint scheme and the transport option
     * (stream-connector §3.1). Filled by option validation at connect. */
    transport_t effective_transport = transport_t::tcp;
    /* Server liveness ping received (graceful-drain-handoff §7.2): the pump
     * answers with a heartbeat pong on its next maintenance pass. */
    bool heartbeat_pong_due = false;
    std::chrono::steady_clock::time_point last_heartbeat_sent{};
    std::chrono::steady_clock::time_point last_inbound_received{};
    std::uint64_t heartbeat_generation = 0;
    std::shared_ptr<boost::asio::steady_timer> heartbeat_timer;
    boost::asio::io_context &io_context;
    boost::asio::strand<boost::asio::io_context::executor_type> write_strand;
    boost::asio::strand<boost::asio::io_context::executor_type> delivery_strand;
    std::shared_ptr<stream_connection_t> connection;
    std::shared_ptr<transport_connect_control_t> connect_control;
    /* Guards state, the handler registries and the close reason. Lock order:
     * leaf. Taken under transport_mutex, never the other way. Handler lists
     * are copied out under it and invoked after it is released. */
    mutable std::mutex lifecycle_mutex;
    std::condition_variable lifecycle_changed;
    bool connect_attempt_active = false;
    bool reconnect_scheduled = false;
    std::shared_ptr<boost::asio::steady_timer> reconnect_timer;
    /* The outermost lock. Guards the transport, the pending queues, the
     * dispatch queue and the pending request/wait maps. Every other mutex in
     * this type may be taken while it is held; it must never be taken while
     * one of them is held. No user callable runs under it. */
    std::mutex transport_mutex;
    /* Guards delivery_queue. Lock order: leaf. Taken under transport_mutex,
     * never the other way. */
    std::mutex delivery_mutex;
    std::condition_variable state_changed;
};

class connector_runtime_t
{
  public:
    explicit connector_runtime_t (std::shared_ptr<connector_state_t> state);

    static connector_runtime_t from (const connector_t &connector);

    void receive_packet (packet_t packet);
    const std::vector<packet_t> &sent_packets () const noexcept;
    std::size_t pending_request_count () const noexcept;

  private:
    std::shared_ptr<connector_state_t> _state;
};

result_t<void> submit_send (std::shared_ptr<connector_state_t> state, packet_t packet);
void submit_send_async (std::shared_ptr<connector_state_t> state,
                        packet_t packet,
                        std::function<void (result_t<void>)> callback);
void start_read_loop (std::shared_ptr<connector_state_t> state);
void start_heartbeat_monitor (std::shared_ptr<connector_state_t> state);
void stop_heartbeat_monitor (std::shared_ptr<connector_state_t> state);
void schedule_reconnect (std::shared_ptr<connector_state_t> state);
void resume_pending_writes_after_connect (std::shared_ptr<connector_state_t> state);
std::function<void (result_t<void>)>
take_active_write_callback (std::shared_ptr<connector_state_t> state);
result_t<void> dispatch_pending (std::shared_ptr<connector_state_t> state);
result_t<packet_t> receive_next (std::shared_ptr<connector_state_t> state,
                                 std::chrono::milliseconds timeout);
result_t<packet_t> wait_for_packet (std::shared_ptr<connector_state_t> state,
                                    std::string packet_name,
                                    std::function<bool (const packet_t &)> predicate,
                                    std::chrono::milliseconds timeout);
/* Routes a packet injected through connector_runtime_t::receive_packet. Takes
 * transport_mutex itself and runs the immediate-mode handlers after releasing
 * it, so the caller must hold no connector lock. */
void deliver_received_packet (connector_state_t &state, packet_t packet);
/* Appends to dispatch_queue. The caller must already hold transport_mutex. */
void enqueue_received_message (connector_state_t &state, packet_t packet);
void schedule_delivery (std::shared_ptr<connector_state_t> state, std::function<void ()> callback);
void schedule_lifecycle_delivery (std::shared_ptr<connector_state_t> state,
                                  std::function<void ()> callback);
void publish_error (connector_state_t &state, error_t error) noexcept;
void close_bound_actors (const std::shared_ptr<connector_state_t> &state);
/* Counts one received application packet by name (stream-connector §10). */
void note_received_packet (connector_state_t &state, const packet_t &packet);
/* Randomized reconnect wait: a value between 50% and 100% of the base delay
 * (stream-connector §6). */
std::chrono::milliseconds jittered_delay (std::chrono::milliseconds base);
/* Resolves the endpoint scheme and the transport option into one transport
 * (stream-connector §3.1), and validates every option item (§6.3). */
std::optional<transport_t> transport_from_scheme (const std::string &endpoint);
result_t<transport_t> resolve_transport (const connector_options_t &options);
result_t<transport_t> validate_options (const connector_options_t &options);
/* Current inbound flow of the executing handler, if any (stream-connector
 * §5.5): an outbound call started while a received message is being handled
 * continues that message's flow. */
struct current_flow_t
{
    std::string flow_id;
    std::optional<flow_origin_t> flow_origin;
};
const current_flow_t &current_flow () noexcept;
class flow_scope_t
{
  public:
    explicit flow_scope_t (const packet_t &packet);
    ~flow_scope_t ();

    flow_scope_t (const flow_scope_t &) = delete;
    flow_scope_t &operator= (const flow_scope_t &) = delete;

  private:
    current_flow_t _previous;
};
void post_runtime_operation (std::function<void ()> operation);
void post_connect_operation (std::function<void ()> operation);
std::shared_ptr<boost::asio::steady_timer>
post_runtime_operation_after (std::chrono::milliseconds delay, std::function<void ()> operation);
void change_state (std::shared_ptr<connector_state_t> state,
                   connection_state_t next,
                   std::optional<error_t> error = std::nullopt);
/* Removes every registered wait and hands back its callback, timers cancelled
 * and pending_waits_version bumped. The caller must hold transport_mutex and
 * owns the delivery. */
std::vector<std::function<void (result_t<packet_t>)>>
take_pending_waits_locked (connector_state_t &state);
/* Ends the connection with `error`: publishes it, moves the state to
 * disconnected and releases the waits that observed the connection as
 * disconnected, delivered like any other completion (stream-connector
 * §10.1.1). The caller owns what follows - closing the transport, failing the
 * writes and requests, the reconnect. Must be called with no connector lock
 * held. */
void connection_ended (const std::shared_ptr<connector_state_t> &state, const error_t &error);

} // namespace zlink::stream_connector::detail
