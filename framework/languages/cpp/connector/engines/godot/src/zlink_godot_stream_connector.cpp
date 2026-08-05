/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink_godot_stream_connector.hpp>

#include <zlink/stream_connector.hpp>

#include <deque>
#include <chrono>
#include <memory>
#include <mutex>
#include <utility>

namespace zlink::godot_stream_connector
{

namespace
{

packet_t to_godot_packet (std::string name, const zlink::message_t &payload)
{
    packet_t packet;
    packet.name = std::move (name);
    const auto bytes = payload.to_string ();
    packet.payload.assign (bytes.begin (), bytes.end ());
    return packet;
}

} // namespace

class stream_connector_t::runtime_t
{
  public:
    zlink::stream_connector::connector_t connector;
    connection_state_t current_state = connection_state_t::created;
    std::function<void (const packet_t &)> packet_callback;
    std::function<void (const packet_t &)> request_callback;
    std::function<void (connection_state_t)> state_callback;
    std::function<void (std::function<void ()>)> main_thread_dispatcher;
    std::mutex pending_callbacks_mutex;
    std::deque<std::function<void ()>> pending_callbacks;

    void post_to_main_thread (std::function<void ()> callback)
    {
        if (!callback) {
            return;
        }
        if (main_thread_dispatcher) {
            main_thread_dispatcher (std::move (callback));
            return;
        }
        const std::lock_guard lock (pending_callbacks_mutex);
        pending_callbacks.push_back (std::move (callback));
    }

    void drain_pending_callbacks ()
    {
        std::deque<std::function<void ()>> callbacks;
        {
            const std::lock_guard lock (pending_callbacks_mutex);
            callbacks.swap (pending_callbacks);
        }
        for (auto &callback : callbacks) {
            callback ();
        }
    }

    void emit_state (connection_state_t state)
    {
        if (!state_callback) {
            return;
        }
        post_to_main_thread ([callback = state_callback, state] { callback (state); });
    }

    void emit_request (packet_t packet)
    {
        if (!request_callback) {
            return;
        }
        post_to_main_thread (
          [callback = request_callback, packet = std::move (packet)] { callback (packet); });
    }
};

stream_connector_t::stream_connector_t () : _runtime (std::make_shared<runtime_t> ()) {}
stream_connector_t::~stream_connector_t ()
{
    if (_runtime) {
        close ();
    }
}
stream_connector_t::stream_connector_t (stream_connector_t &&) noexcept = default;
stream_connector_t &stream_connector_t::operator= (stream_connector_t &&) noexcept = default;

void stream_connector_t::connect (std::string endpoint)
{
    zlink::stream_connector::connector_options_t options;
    options.endpoint = std::move (endpoint);
    _runtime->connector = zlink::stream_connector::connector_factory_t::create (std::move (options));
    _runtime->current_state = connection_state_t::connecting;
    _runtime->emit_state (_runtime->current_state);
    const auto connected = _runtime->connector.connect ();
    _runtime->current_state =
      connected ? connection_state_t::connected : connection_state_t::disconnected;
    _runtime->emit_state (_runtime->current_state);
}

void stream_connector_t::close ()
{
    _runtime->connector.close ();
    _runtime->current_state = connection_state_t::closed;
    _runtime->emit_state (_runtime->current_state);
}

void stream_connector_t::send_json (std::string packet_name, std::string json_payload)
{
    send_json (std::move (packet_name), std::move (json_payload), send_options_t{});
}

void stream_connector_t::send_json (std::string packet_name,
                                    std::string json_payload,
                                    send_options_t options)
{
    zlink::stream_connector::packet_t packet;
    packet.name = std::move (packet_name);
    packet.codec = zlink::stream_connector::codec_t::json;
    packet.payload = zlink::message_t::from (std::move (json_payload));
    for (auto &entry : options.metadata) {
        packet.metadata.with (std::move (entry.first), std::move (entry.second));
    }
    auto call = _runtime->connector.send (std::move (packet));
    if (options.compress) {
        call.compress ();
    }
    call.submit ();
}

void stream_connector_t::request_json (std::string packet_name,
                                       std::string json_payload,
                                       double timeout_seconds)
{
    auto reply_name = packet_name;
    zlink::stream_connector::packet_t packet;
    packet.name = std::move (packet_name);
    packet.codec = zlink::stream_connector::codec_t::json;
    packet.payload = zlink::message_t::from (std::move (json_payload));
    auto request = _runtime->connector.request (std::move (packet));
    request.timeout (
      std::chrono::milliseconds (static_cast<int> (timeout_seconds * 1000.0)));
    request.submit<zlink::message_t> ([runtime = std::weak_ptr<runtime_t> (_runtime), reply_name = std::move (reply_name)] (
                      zlink::stream_connector::result_t<zlink::message_t> result) mutable {
        if (!result) {
            return;
        }
        if (auto owner = runtime.lock ()) {
            owner->emit_request (to_godot_packet (std::move (reply_name), result.value ()));
        }
    });
}

void stream_connector_t::dispatch ()
{
    _runtime->connector.dispatch ();
    _runtime->drain_pending_callbacks ();
}

void stream_connector_t::set_main_thread_dispatcher (
  std::function<void (std::function<void ()>)> dispatcher)
{
    if (dispatcher) {
        _runtime->main_thread_dispatcher = std::move (dispatcher);
    }
}

connection_state_t stream_connector_t::state () const
{
    return _runtime->current_state;
}

void stream_connector_t::on_packet (std::function<void (const packet_t &)> callback)
{
    _runtime->packet_callback = std::move (callback);
}

void stream_connector_t::on_request_completed (std::function<void (const packet_t &)> callback)
{
    _runtime->request_callback = std::move (callback);
}

void stream_connector_t::on_connection_state_changed (
  std::function<void (connection_state_t)> callback)
{
    _runtime->state_callback = std::move (callback);
}

} // namespace zlink::godot_stream_connector
