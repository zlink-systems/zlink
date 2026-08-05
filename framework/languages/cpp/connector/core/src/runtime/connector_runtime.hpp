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

struct pending_wait_t
{
    std::uint64_t wait_id = 0;
    std::string packet_name;
    std::function<bool (const packet_t &)> predicate;
    std::function<void (result_t<packet_t>)> callback;
    std::shared_ptr<boost::asio::steady_timer> timeout_timer;
};

struct inbound_observer_entry_t
{
    std::uint64_t id = 0;
    std::function<void (const inbound_observation_t &)> callback;
    std::atomic_bool active{true};
};

class connector_state_t : public std::enable_shared_from_this<connector_state_t>
{
  public:
    explicit connector_state_t (connector_options_t options) :
        connector_id (next_connector_id.fetch_add (1, std::memory_order_relaxed)),
        options (std::move (options)),
        io_context (shared_io_context ()),
        write_strand (boost::asio::make_strand (io_context)),
        delivery_strand (boost::asio::make_strand (shared_callback_io_context ()))
    {
    }

    inline static std::atomic_uint64_t next_connector_id{1};
    std::uint64_t connector_id = 0;
    connector_options_t options;
    connection_state_t state = connection_state_t::created;
    std::uint64_t next_request_seq = 1;
    std::uint64_t next_wait_id = 1;
    std::map<std::uint64_t, pending_request_t> pending_requests;
    std::map<std::uint64_t, pending_wait_t> pending_waits;
    std::deque<pending_send_t> pending_sends;
    std::deque<pending_write_t> pending_writes;
    std::vector<std::uint8_t> inbound_buffer;
    std::deque<packet_t> dispatch_queue;
    std::deque<std::function<void ()>> delivery_queue;
    std::vector<packet_t> sent_packets;
    std::map<std::string, std::vector<std::function<void (const packet_t &)>>> packet_handlers;
    std::vector<std::function<void (const connection_state_changed_t &)>> state_handlers;
    std::vector<std::function<void (const error_t &)>> error_handlers;
    std::vector<std::function<void ()>> disconnected_handlers;
    std::uint64_t next_inbound_observer_id = 1;
    std::vector<std::shared_ptr<inbound_observer_entry_t>> inbound_observers;
    std::atomic_size_t pending_inbound_observer_notifications{0};
    std::atomic_bool inbound_observer_drop_report_pending{false};
    bool connect_started = false;
    codec_t default_codec = codec_t::json;
    std::set<codec_t> enabled_codecs{codec_t::json};
    std::shared_ptr<const compression_codec_t> compression_codec;
    bool lz4_enabled = false;
    std::atomic_bool close_requested{false};
    bool send_in_progress = false;
    bool write_in_progress = false;
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
    // Registered lifecycle callbacks must not outlive close(). Operation
    // completions use a separate path because close() completes them with a
    // closed result.
    std::atomic_bool lifecycle_callbacks_enabled{true};
    mutable std::mutex lifecycle_mutex;
    std::condition_variable lifecycle_changed;
    bool connect_attempt_active = false;
    bool reconnect_scheduled = false;
    std::shared_ptr<boost::asio::steady_timer> reconnect_timer;
    std::mutex transport_mutex;
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
result_t<void> dispatch_pending (std::shared_ptr<connector_state_t> state);
result_t<packet_t> receive_next (std::shared_ptr<connector_state_t> state,
                                 std::chrono::milliseconds timeout);
result_t<packet_t> wait_for_packet (std::shared_ptr<connector_state_t> state,
                                    std::string packet_name,
                                    std::function<bool (const packet_t &)> predicate,
                                    std::chrono::milliseconds timeout);
void deliver_received_packet (connector_state_t &state, packet_t packet);
void schedule_delivery (std::shared_ptr<connector_state_t> state, std::function<void ()> callback);
void schedule_lifecycle_delivery (std::shared_ptr<connector_state_t> state,
                                  std::function<void ()> callback);
void publish_error (connector_state_t &state, error_t error) noexcept;
void post_runtime_operation (std::function<void ()> operation);
void post_connect_operation (std::function<void ()> operation);
std::shared_ptr<boost::asio::steady_timer>
post_runtime_operation_after (std::chrono::milliseconds delay,
                              std::function<void ()> operation);
void change_state (std::shared_ptr<connector_state_t> state,
                   connection_state_t next,
                   std::optional<error_t> error = std::nullopt);

} // namespace zlink::stream_connector::detail
