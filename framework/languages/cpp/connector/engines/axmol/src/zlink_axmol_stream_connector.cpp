/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink_axmol_stream_connector.hpp>

#include <zlink/stream_connector.hpp>

#include <deque>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

namespace zlink::axmol_stream_connector
{

namespace
{

packet_t to_axmol_packet (std::string name, const zlink::message_t &payload)
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
    std::function<void (connection_state_t)> state_callback;
    std::function<void (std::function<void ()>)> axmol_thread_dispatcher;
    struct subscription_entry_t
    {
        std::string name;
        std::function<void (const packet_t &)> callback;
        zlink::stream_connector::subscription_t handle;
    };
    std::map<std::uint64_t, subscription_entry_t> subscriptions;
    std::uint64_t next_subscription_id = 1;
    std::mutex pending_callbacks_mutex;
    std::deque<std::function<void ()>> pending_callbacks;

    void post_to_axmol_thread (std::function<void ()> callback)
    {
        if (!callback) {
            return;
        }
        if (axmol_thread_dispatcher) {
            axmol_thread_dispatcher (std::move (callback));
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
        post_to_axmol_thread ([callback = state_callback, state] { callback (state); });
    }

    void register_subscription (std::uint64_t id,
                                subscription_entry_t &entry,
                                std::weak_ptr<runtime_t> weak_owner)
    {
        entry.handle = connector.on<zlink::stream_connector::packet_t> (
          entry.name,
          [weak_owner, id] (
            const zlink::stream_connector::message_t<zlink::stream_connector::packet_t> &message) {
              if (auto owner = weak_owner.lock ()) {
                  if (owner->subscriptions.contains (id)) {
                      auto packet = to_axmol_packet (message.packet_name, message.payload.payload);
                      packet.compressed = message.payload.compressed;
                      packet.metadata = message.metadata.values;
                      owner->post_to_axmol_thread ([weak_owner, id, packet = std::move (packet)] {
                          if (auto runtime = weak_owner.lock ()) {
                              if (const auto it = runtime->subscriptions.find (id);
                                  it != runtime->subscriptions.end ()) {
                                  auto callback = it->second.callback;
                                  if (callback) {
                                      callback (packet);
                                  }
                              }
                          }
                      });
                  }
              }
          });
    }
};

subscription_t::subscription_t (std::function<void ()> unsubscribe,
                                std::function<bool ()> is_active) :
    _unsubscribe (std::move (unsubscribe)), _is_active (std::move (is_active))
{
}

subscription_t &subscription_t::operator= (subscription_t &&other) noexcept
{
    if (this != &other) {
        unsubscribe ();
        _unsubscribe = std::move (other._unsubscribe);
        _is_active = std::move (other._is_active);
    }
    return *this;
}

void subscription_t::unsubscribe ()
{
    if (active ()) {
        _unsubscribe ();
    }
}

bool subscription_t::active () const
{
    return _is_active && _is_active ();
}

stream_connector_t::stream_connector_t () : _runtime (std::make_shared<runtime_t> ())
{
}
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
    for (auto &[id, entry] : _runtime->subscriptions) {
        entry.handle.unsubscribe ();
    }
    zlink::stream_connector::connector_options_t options;
    options.endpoint = std::move (endpoint);
    _runtime->connector =
      zlink::stream_connector::connector_factory_t::create (std::move (options));
    for (auto &[id, entry] : _runtime->subscriptions) {
        _runtime->register_subscription (id, entry, _runtime);
    }
    _runtime->current_state = connection_state_t::connecting;
    _runtime->emit_state (_runtime->current_state);
    const auto connected = _runtime->connector.connect ();
    _runtime->current_state =
      connected ? connection_state_t::connected : connection_state_t::disconnected;
    _runtime->emit_state (_runtime->current_state);
}

void stream_connector_t::close ()
{
    _runtime->subscriptions.clear ();
    _runtime->connector.close ();
    _runtime->current_state = connection_state_t::closed;
    _runtime->emit_state (_runtime->current_state);
}

subscription_t stream_connector_t::on (std::string packet_name,
                                       std::function<void (const packet_t &)> callback)
{
    const auto id = _runtime->next_subscription_id++;
    auto [it, inserted] = _runtime->subscriptions.emplace (
      id, runtime_t::subscription_entry_t{std::move (packet_name), std::move (callback), {}});
    if (_runtime->current_state == connection_state_t::connected) {
        _runtime->register_subscription (id, it->second, _runtime);
    }
    auto runtime = std::weak_ptr<runtime_t> (_runtime);
    return subscription_t (
      [runtime, id] {
          if (auto owner = runtime.lock ()) {
              owner->subscriptions.erase (id);
          }
      },
      [runtime, id] {
          if (auto owner = runtime.lock ()) {
              return owner->subscriptions.contains (id);
          }
          return false;
      });
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
                                       double timeout_seconds,
                                       std::function<void (const request_result_t &)> callback)
{
    zlink::stream_connector::packet_t packet;
    packet.name = std::move (packet_name);
    packet.codec = zlink::stream_connector::codec_t::json;
    packet.payload = zlink::message_t::from (std::move (json_payload));
    auto request = _runtime->connector.request (std::move (packet));
    request.timeout (std::chrono::milliseconds (static_cast<int> (timeout_seconds * 1000.0)));
    request.submit<zlink::message_t> (
      [runtime = std::weak_ptr<runtime_t> (_runtime), callback = std::move (callback)] (
        zlink::stream_connector::result_t<zlink::message_t> result) mutable {
          if (auto owner = runtime.lock ()) {
              request_result_t completion;
              if (result) {
                  completion.reply = to_axmol_packet ({}, result.value ());
              } else {
                  completion.error_code = result.error ()->code;
                  completion.error_message = result.error ()->message;
              }
              if (callback) {
                  owner->post_to_axmol_thread (
                    [callback = std::move (callback), completion = std::move (completion)] {
                        callback (completion);
                    });
              }
          }
      });
}

void stream_connector_t::dispatch ()
{
    _runtime->connector.dispatch ();
    _runtime->drain_pending_callbacks ();
}

void stream_connector_t::set_axmol_thread_dispatcher (
  std::function<void (std::function<void ()>)> dispatcher)
{
    if (dispatcher) {
        _runtime->axmol_thread_dispatcher = std::move (dispatcher);
    }
}

connection_state_t stream_connector_t::state () const
{
    return _runtime->current_state;
}

void stream_connector_t::on_connection_state_changed (
  std::function<void (connection_state_t)> callback)
{
    _runtime->state_callback = std::move (callback);
}

} // namespace zlink::axmol_stream_connector
