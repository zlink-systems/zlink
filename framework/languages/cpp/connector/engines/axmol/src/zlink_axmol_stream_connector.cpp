/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink_axmol_stream_connector.hpp>

#include <zlink/stream_connector.hpp>

#include <deque>
#include <algorithm>
#include <chrono>
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

packet_t to_axmol_packet (const zlink::stream_connector::packet_t &source)
{
    auto packet = to_axmol_packet (source.name, source.payload);
    packet.metadata = source.metadata.values;
    packet.compressed = source.compressed;
    return packet;
}

} // namespace

void request_sending_context_t::set_metadata (std::string key, std::string value)
{
    _metadata[std::move (key)] = std::move (value);
}

subscription_t::subscription_t (std::function<void ()> release) : _release (std::move (release))
{
}

subscription_t::subscription_t (subscription_t &&other) noexcept :
    _release (std::move (other._release))
{
    other._release = {};
}

subscription_t &subscription_t::operator= (subscription_t &&other) noexcept
{
    if (this != &other) {
        unsubscribe ();
        _release = std::move (other._release);
        other._release = {};
    }
    return *this;
}

void subscription_t::unsubscribe ()
{
    if (_release) {
        auto release = std::move (_release);
        release ();
    }
}

class stream_connector_t::runtime_t
{
  public:
    zlink::stream_connector::connector_t connector;
    connection_state_t current_state = connection_state_t::created;
    std::function<void (const packet_t &)> packet_callback;
    std::function<void (const packet_t &)> request_callback;
    std::function<void (connection_state_t)> state_callback;
    std::function<void (std::function<void ()>)> axmol_thread_dispatcher;
    struct subscription_entry_t
    {
        std::string name;
        zlink::stream_connector::subscription_t handle;
    };
    std::vector<subscription_entry_t> subscriptions;
    template <typename Callback> struct hook_entry_t
    {
        Callback callback;
        zlink::stream_connector::subscription_t handle;
    };
    using sending_entry_t = hook_entry_t<std::function<void (request_sending_context_t &)>>;
    using reply_entry_t = hook_entry_t<std::function<void (const reply_received_context_t &)>>;
    std::vector<std::shared_ptr<sending_entry_t>> sending_hooks;
    std::vector<std::shared_ptr<reply_entry_t>> reply_hooks;
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

    void emit_request (packet_t packet)
    {
        if (!request_callback) {
            return;
        }
        post_to_axmol_thread (
          [callback = request_callback, packet = std::move (packet)] { callback (packet); });
    }

    void register_subscription (subscription_entry_t &entry, std::weak_ptr<runtime_t> weak_owner)
    {
        entry.handle = connector.on<zlink::stream_connector::packet_t> (
          entry.name,
          [weak_owner] (
            const zlink::stream_connector::message_t<zlink::stream_connector::packet_t> &message) {
              if (auto owner = weak_owner.lock ()) {
                  if (owner->packet_callback) {
                      auto packet = to_axmol_packet (message.packet_name, message.payload.payload);
                      packet.compressed = message.payload.compressed;
                      packet.metadata = message.metadata.values;
                      owner->post_to_axmol_thread (
                        [callback = owner->packet_callback, packet = std::move (packet)] {
                            callback (packet);
                        });
                  }
              }
          });
    }

    void register_sending_hook (const std::shared_ptr<sending_entry_t> &entry)
    {
        entry->handle = connector.on_request_sending (
          [weak_entry = std::weak_ptr<sending_entry_t> (entry)] (
            zlink::stream_connector::request_sending_context_t &source) {
              if (auto registered = weak_entry.lock ()) {
                  request_sending_context_t context{source.request_packet_name, source.actor_id};
                  registered->callback (context);
                  for (const auto &[key, value] : context.metadata_values ()) {
                      source.set_metadata (key, value);
                  }
              }
          });
    }

    void register_reply_hook (const std::shared_ptr<reply_entry_t> &entry,
                              std::weak_ptr<runtime_t> weak_owner)
    {
        entry->handle = connector.on_reply_received (
          [weak_owner, weak_entry = std::weak_ptr<reply_entry_t> (entry)] (
            const zlink::stream_connector::reply_received_context_t &source) {
              if (auto owner = weak_owner.lock ()) {
                  reply_received_context_t context;
                  context.request_packet_name = source.request_packet_name;
                  context.actor_id = source.actor_id;
                  context.succeeded = source.succeeded;
                  context.elapsed = source.elapsed;
                  if (source.reply) {
                      context.reply = to_axmol_packet (*source.reply);
                  }
                  if (source.error) {
                      context.error = error_t{source.error->code, source.error->message};
                  }
                  owner->post_to_axmol_thread ([weak_entry, context = std::move (context)] {
                      if (auto registered = weak_entry.lock ()) {
                          registered->callback (context);
                      }
                  });
              }
          });
    }
};

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
    for (auto &entry : _runtime->subscriptions) {
        entry.handle.unsubscribe ();
    }
    _runtime->connector.close ();
    while (_runtime->connector.pending_dispatch_count () != 0) {
        _runtime->connector.dispatch ();
    }
    for (const auto &entry : _runtime->sending_hooks) {
        entry->handle.unsubscribe ();
    }
    for (const auto &entry : _runtime->reply_hooks) {
        entry->handle.unsubscribe ();
    }
    zlink::stream_connector::connector_options_t options;
    options.endpoint = std::move (endpoint);
    _runtime->connector =
      zlink::stream_connector::connector_factory_t::create (std::move (options));
    for (auto &entry : _runtime->subscriptions) {
        _runtime->register_subscription (entry, _runtime);
    }
    for (const auto &entry : _runtime->sending_hooks) {
        _runtime->register_sending_hook (entry);
    }
    for (const auto &entry : _runtime->reply_hooks) {
        _runtime->register_reply_hook (entry, _runtime);
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

void stream_connector_t::subscribe (std::string packet_name)
{
    _runtime->subscriptions.push_back ({std::move (packet_name), {}});
    if (_runtime->current_state == connection_state_t::connected) {
        _runtime->register_subscription (_runtime->subscriptions.back (), _runtime);
    }
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
    request.timeout (std::chrono::milliseconds (static_cast<int> (timeout_seconds * 1000.0)));
    request.submit<zlink::message_t> (
      [runtime = std::weak_ptr<runtime_t> (_runtime), reply_name = std::move (reply_name)] (
        zlink::stream_connector::result_t<zlink::message_t> result) mutable {
          if (!result) {
              return;
          }
          if (auto owner = runtime.lock ()) {
              owner->emit_request (to_axmol_packet (std::move (reply_name), result.value ()));
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

subscription_t
stream_connector_t::on_request_sending (std::function<void (request_sending_context_t &)> callback)
{
    auto entry = std::make_shared<runtime_t::sending_entry_t> ();
    entry->callback = std::move (callback);
    _runtime->sending_hooks.push_back (entry);
    if (_runtime->current_state == connection_state_t::connected) {
        _runtime->register_sending_hook (entry);
    }
    return subscription_t ([owner = std::weak_ptr<runtime_t> (_runtime),
                            weak_entry = std::weak_ptr<runtime_t::sending_entry_t> (entry)] {
        if (auto runtime = owner.lock ()) {
            if (auto registered = weak_entry.lock ()) {
                registered->handle.unsubscribe ();
                auto &hooks = runtime->sending_hooks;
                hooks.erase (std::remove (hooks.begin (), hooks.end (), registered), hooks.end ());
            }
        }
    });
}

subscription_t stream_connector_t::on_reply_received (
  std::function<void (const reply_received_context_t &)> callback)
{
    auto entry = std::make_shared<runtime_t::reply_entry_t> ();
    entry->callback = std::move (callback);
    _runtime->reply_hooks.push_back (entry);
    if (_runtime->current_state == connection_state_t::connected) {
        _runtime->register_reply_hook (entry, _runtime);
    }
    return subscription_t ([owner = std::weak_ptr<runtime_t> (_runtime),
                            weak_entry = std::weak_ptr<runtime_t::reply_entry_t> (entry)] {
        if (auto runtime = owner.lock ()) {
            if (auto registered = weak_entry.lock ()) {
                registered->handle.unsubscribe ();
                auto &hooks = runtime->reply_hooks;
                hooks.erase (std::remove (hooks.begin (), hooks.end (), registered), hooks.end ());
            }
        }
    });
}

} // namespace zlink::axmol_stream_connector
