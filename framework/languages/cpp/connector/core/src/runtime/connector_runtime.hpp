/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/stream_connector/contracts/connector.hpp>

#include "runtime/transport/transport_connection.hpp"

#include <boost/asio.hpp>

#include <algorithm>
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

class shared_runtime_t;
bool configure_shared_runtime_worker_count (std::size_t worker_count);

struct pending_write_t
{
    std::vector<std::uint8_t> frame;
    std::function<void (result_t<void>)> callback;
    std::uint64_t write_id = 0;
    bool ready = true;
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
    std::shared_ptr<std::vector<std::uint64_t>> reply_hook_ids;
};

/* Lock order for connector_state_t (see the mutex members below).
 *
 *     transport_mutex  ->  lifecycle_mutex
 *                      ->  delivery_mutex
 *
 * transport_mutex is the only outer lock. lifecycle_mutex and delivery_mutex
 * are leaves: neither is ever held while the other is taken, and neither is
 * ever held while transport_mutex is taken. Acquiring them in any other order would close a cycle.
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
struct dispatch_envelope_t
{
    packet_t packet;
    std::optional<std::uint16_t> actor_slot;
    std::function<void ()> actor_event;
    /* Position in the connector's one arrival order (connector_state_t::
     * next_arrival), taken when the frame is decoded. */
    std::uint64_t arrival = 0;
};

/* A queued Manual callback (stream-connector §7). `callbacks` is the number of
 * handler calls `run` makes with the handlers registered at the moment it is
 * asked; the pending dispatch count sums it. */
struct delivery_t
{
    std::uint64_t arrival = 0;
    std::function<void ()> run;
    std::function<std::size_t ()> callbacks;
};

struct packet_handler_entry_t
{
    std::uint64_t id = 0;
    std::function<void (const dispatch_envelope_t &)> handler;
    /* Set for an Actor handle handler: it takes only packets for that slot. */
    std::optional<std::uint16_t> actor_slot;

    bool takes (const dispatch_envelope_t &envelope) const noexcept
    {
        return !actor_slot || actor_slot == envelope.actor_slot;
    }
};

template <typename THandler> struct handler_entry_t
{
    std::uint64_t id = 0;
    THandler handler;
};

using actor_handlers_t =
  std::vector<handler_entry_t<std::function<void (const std::shared_ptr<actor_t> &)>>>;

struct pending_wait_t
{
    std::uint64_t wait_id = 0;
    std::string packet_name;
    std::function<bool (const packet_t &)> predicate;
    std::function<void (result_t<packet_t>)> callback;
    std::shared_ptr<boost::asio::steady_timer> timeout_timer;
    // The synchronous wait_for sets this, like pending_request_t: its callback
    // only resolves the promise its caller blocks on, so it runs where the
    // result is decided instead of on the delivery strand, which that caller
    // may be holding.
    bool deliver_direct = false;
};

class connector_state_t : public std::enable_shared_from_this<connector_state_t>
{
  public:
    explicit connector_state_t (connector_options_t options);

    inline static std::atomic_uint64_t next_connector_id{1};
    std::uint64_t connector_id = 0;
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
    /* stream-connector §5.2: the accepted writes of the connection that have not
     * started, in acceptance order. One write runs at a time (active_write);
     * the next starts when it completes. */
    std::deque<pending_write_t> write_queue;
    std::optional<pending_write_t> active_write;
    std::vector<std::uint8_t> inbound_buffer;
    std::deque<dispatch_envelope_t> dispatch_queue;
    /* Bumped whenever dispatch_queue is dropped wholesale (a new connection is
     * established). A scan that evaluates user predicates outside transport_mutex
     * re-reads this before putting the packets it did not take back, so a
     * connection boundary crossed during the scan still drops them
     * (stream-connector §10). */
    std::uint64_t dispatch_queue_generation = 0;
    std::deque<delivery_t> delivery_queue;
    /* stream-connector §7: a Manual pump runs queued packets and callbacks in
     * one arrival order. A packet takes its number when it is decoded, a
     * callback when it is queued; the pump runs both queues by it. */
    std::atomic_uint64_t next_arrival{1};
    std::vector<packet_t> sent_packets;
    std::map<std::string, std::vector<packet_handler_entry_t>> packet_handlers;
    std::vector<handler_entry_t<std::function<void (const connection_state_changed_t &)>>>
      state_handlers;
    std::vector<handler_entry_t<std::function<void (const error_t &)>>> error_handlers;
    std::vector<handler_entry_t<std::function<void (request_sending_context_t &)>>>
      request_sending_handlers;
    std::vector<handler_entry_t<std::function<void (const reply_received_context_t &)>>>
      reply_received_handlers;
    std::vector<handler_entry_t<std::function<void (std::optional<close_reason_t>)>>>
      disconnected_handlers;
    std::vector<std::pair<std::uint16_t, std::shared_ptr<actor_t>>> actors_by_slot;
    std::map<std::string, std::shared_ptr<actor_t>, std::less<>> actors_by_id;
    actor_handlers_t actor_bound_handlers;
    actor_handlers_t actor_unbound_handlers;
    std::atomic_uint64_t next_subscription_id{1};
    /* Per-name receive counts for the current connection (stream-connector
     * §10). Guarded by transport_mutex, like dispatch_queue: a packet is
     * counted in the step that queues or hands it off. */
    std::map<std::string, std::size_t, std::less<>> received_counts;
    codec_t default_codec = codec_t::json;
    std::set<codec_t> enabled_codecs{codec_t::json};
    std::shared_ptr<const compression_codec_t> compression_codec;
    bool lz4_enabled = false;
    /* Set once by the close that starts the close work (stream-connector §7);
     * every later close finds it set. */
    std::atomic_bool close_requested{false};
    /* The close work has finished. Guarded by lifecycle_mutex; a close called
     * outside a callback waits on lifecycle_changed for it. */
    bool close_completed = false;
    std::uint64_t next_write_id = 1;
    bool read_in_progress = false;
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
    std::shared_ptr<shared_runtime_t> runtime;
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

/* A handler event captures the ids of the handlers registered when it is
 * scheduled and resolves them again when it runs: a handler removed in
 * between does not run, and one added in between does not see the earlier
 * event (stream-connector §7). The caller holds lifecycle_mutex. */
template <typename TEntry>
std::vector<std::uint64_t> registered_handler_ids_locked (const std::vector<TEntry> &registry)
{
    std::vector<std::uint64_t> ids;
    ids.reserve (registry.size ());
    for (const auto &entry : registry) {
        ids.push_back (entry.id);
    }
    return ids;
}

/* Takes lifecycle_mutex; the handlers are invoked after it is released. */
template <typename TEntry>
std::vector<TEntry> registered_handlers (connector_state_t &state,
                                         std::vector<TEntry> connector_state_t::*registry,
                                         const std::vector<std::uint64_t> &ids)
{
    std::vector<TEntry> handlers;
    std::lock_guard<std::mutex> lock (state.lifecycle_mutex);
    for (const auto &entry : state.*registry) {
        if (std::find (ids.begin (), ids.end (), entry.id) != ids.end ()) {
            handlers.push_back (entry);
        }
    }
    return handlers;
}

/* The number of handlers registered_handlers would return now: the pending
 * dispatch count of one handler event (stream-connector §7). */
template <typename TEntry>
std::function<std::size_t ()> registered_handler_count (
  const std::shared_ptr<connector_state_t> &state,
  std::vector<TEntry> connector_state_t::*registry,
  std::vector<std::uint64_t> ids)
{
    return [weak = std::weak_ptr<connector_state_t> (state), registry, ids = std::move (ids)] {
        const auto locked = weak.lock ();
        return locked ? registered_handlers (*locked, registry, ids).size () : std::size_t{0};
    };
}

void publish_error (connector_state_t &state, error_t error) noexcept;

/* stream-connector §7, §9: a registered handler or callback that fails reaches
 * the error handlers as UserCallbackFailed; the next handler still runs. */
template <typename TCallback>
void invoke_user_callback (connector_state_t &state, const char *failure, TCallback &&callback)
{
    try {
        std::forward<TCallback> (callback) ();
    }
    catch (const std::exception &error) {
        publish_error (state, {error_code_t::user_callback_failed, error.what ()});
    }
    catch (...) {
        publish_error (state, {error_code_t::user_callback_failed, failure});
    }
}

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

void submit_send (std::shared_ptr<connector_state_t> state,
                  packet_t packet,
                  std::optional<actor_binding_ref_t> actor_binding = std::nullopt);
void submit_send_async (std::shared_ptr<connector_state_t> state,
                        packet_t packet,
                        std::function<void (result_t<void>)> callback,
                        std::optional<actor_binding_ref_t> actor_binding = std::nullopt);
void start_read_loop (std::shared_ptr<connector_state_t> state);
void start_heartbeat_monitor (std::shared_ptr<connector_state_t> state);
void stop_heartbeat_monitor (std::shared_ptr<connector_state_t> state);
void schedule_reconnect (std::shared_ptr<connector_state_t> state);
result_t<void> dispatch_pending (std::shared_ptr<connector_state_t> state);
result_t<packet_t> wait_for_packet (std::shared_ptr<connector_state_t> state,
                                    std::string packet_name,
                                    std::function<bool (const packet_t &)> predicate,
                                    std::chrono::milliseconds timeout);
/* Routes a packet injected through connector_runtime_t::receive_packet the way
 * the read pump routes a received one. The caller holds no connector lock. */
void deliver_received_packet (connector_state_t &state, packet_t packet);
/* Appends to dispatch_queue. The caller must already hold transport_mutex. */
void enqueue_received_message (connector_state_t &state, dispatch_envelope_t envelope);
/* Runs `callback` on the delivery strand inside a callback_scope_t: one
 * callback of the connector at a time, never on the calling thread. */
void run_on_delivery_strand (std::shared_ptr<connector_state_t> state,
                             std::function<void ()> callback);
/* Immediate: run_on_delivery_strand. Manual: queued for the next dispatch pump
 * (stream-connector §7). `callbacks` counts the handler calls `callback` makes
 * (delivery_t); without it the delivery is one callback. */
void schedule_delivery (std::shared_ptr<connector_state_t> state,
                        std::function<void ()> callback,
                        std::function<std::size_t ()> callbacks = {});
/* Marks the current thread as running user callbacks of `state` (stream-connector
 * §7 handlers and callbacks) for the scope's lifetime. A close called inside
 * one of them starts the close work and returns instead of waiting for it. */
class callback_scope_t
{
  public:
    explicit callback_scope_t (const connector_state_t &state) noexcept;
    ~callback_scope_t ();
    callback_scope_t (const callback_scope_t &) = delete;
    callback_scope_t &operator= (const callback_scope_t &) = delete;
    static bool running_callback_of (const connector_state_t &state) noexcept;

  private:
    const connector_state_t *_previous;
};
void close_bound_actors (const std::shared_ptr<connector_state_t> &state);
/* Queues the bound or unbound callbacks (`registry`) of one Actor event for the
 * handlers `handler_ids` registered when it happened (stream-connector §5.6). */
void schedule_actor_delivery (const std::shared_ptr<connector_state_t> &state,
                              actor_handlers_t connector_state_t::*registry,
                              std::vector<std::uint64_t> handler_ids,
                              std::shared_ptr<actor_t> actor);
/* Counts one received application packet by name (stream-connector §10). The
 * caller holds transport_mutex and hands the packet to its consumer in the
 * same critical section, so the count and the packet become observable
 * together. */
void count_received_locked (connector_state_t &state, const packet_t &packet);
/* The number of registered packet handlers that take `envelope` now: the one
 * test every dispatch step applies and the count pending dispatch reports
 * (stream-connector §7, §10). lifecycle_mutex nests under a held
 * transport_mutex. */
std::size_t packet_handler_count (connector_state_t &state, const dispatch_envelope_t &envelope);
/* Immediate: hands the queued packets a newly registered handler takes to the
 * delivery strand. No effect in Manual, where the next pump takes them. The
 * caller holds no connector lock. */
void deliver_queued_to_handlers (const std::shared_ptr<connector_state_t> &state);
/* Randomized reconnect wait: a value between 50% and 100% of the base delay
 * (stream-connector §6). */
std::chrono::milliseconds jittered_delay (std::chrono::milliseconds base);
/* Resolves the endpoint scheme and the transport option into one transport
 * (stream-connector §3.1), and validates every option item (§6.3). */
std::optional<transport_t> transport_from_scheme (const std::string &endpoint);
result_t<transport_t> resolve_transport (const connector_options_t &options);
result_t<transport_t> validate_options (const connector_options_t &options);
void post_runtime_operation (const std::shared_ptr<connector_state_t> &state,
                             std::function<void ()> operation);
void post_connect_operation (const std::shared_ptr<connector_state_t> &state,
                             std::function<void ()> operation);
std::shared_ptr<boost::asio::steady_timer>
post_runtime_operation_after (const std::shared_ptr<connector_state_t> &state,
                              std::chrono::milliseconds delay,
                              std::function<void ()> operation);
void change_state (std::shared_ptr<connector_state_t> state,
                   connection_state_t next,
                   std::optional<error_t> error = std::nullopt);
/* Removes every registered wait and hands it back, timers cancelled
 * and pending_waits_version bumped. The caller must hold transport_mutex and
 * owns the delivery. */
std::vector<pending_wait_t> take_pending_waits_locked (connector_state_t &state);
/* The operations accepted on a connection that have not completed: the write
 * callbacks (active and queued), the pending request sequences and the waits.
 * Taking them empties the write queues, so a frame not yet written is never
 * written. */
struct connection_operations_t
{
    std::vector<std::function<void (result_t<void>)>> writes;
    std::vector<std::uint64_t> requests;
    std::vector<pending_wait_t> waits;
};
/* Caller holds transport_mutex. */
connection_operations_t take_connection_operations_locked (connector_state_t &state);
/* Fails every taken operation as Disconnected with `message`, delivered like
 * any other completion. Called with no connector lock held. */
void fail_connection_operations (const std::shared_ptr<connector_state_t> &state,
                                 connection_operations_t operations,
                                 const std::string &message);
/* Ends `observed_connection` with `error` when it is still the current
 * connection, and returns false without any effect otherwise: a failure
 * observed on a replaced connection does not end its replacement. On the
 * current connection it publishes the error, moves the state to disconnected,
 * fails every write and request accepted on the connection and releases the
 * waits that observed it as disconnected, delivered like any other completion
 * (stream-connector §10.1.1). On true the caller closes the transport and
 * schedules the reconnect. Must be called with no connector lock held. */
bool connection_ended (const std::shared_ptr<connector_state_t> &state,
                       const error_t &error,
                       const std::shared_ptr<stream_connection_t> &observed_connection);

} // namespace zlink::stream_connector::detail
