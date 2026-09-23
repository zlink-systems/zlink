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

packet_t to_godot_packet (const zlink::stream_connector::packet_t &source)
{
    auto packet = to_godot_packet (source.name, source.payload);
    packet.metadata = source.metadata.values;
    packet.compressed = source.compressed;
    return packet;
}

} // namespace

request_sending_context_t::request_sending_context_t (
  std::string name,
  std::optional<std::string> actor,
  std::function<void (std::string, std::string)> setter) :
    request_packet_name (std::move (name)),
    actor_id (std::move (actor)),
    _setter (std::move (setter))
{
}

void request_sending_context_t::set_metadata (std::string key, std::string value)
{
    _setter (std::move (key), std::move (value));
}

subscription_t::subscription_t (std::function<void ()> release) : _release (std::move (release))
{
}

subscription_t::~subscription_t ()
{
    unsubscribe ();
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
        _release = {};
        release ();
    }
}

bool subscription_t::active () const noexcept
{
    return static_cast<bool> (_release);
}

class stream_connector_t::runtime_t
{
  public:
    struct sending_hook_t
    {
        std::function<void (request_sending_context_t &)> callback;
        zlink::stream_connector::subscription_t subscription;
    };

    struct reply_hook_t
    {
        std::function<void (const reply_received_context_t &)> callback;
        zlink::stream_connector::subscription_t subscription;
    };

    zlink::stream_connector::connector_t connector;
    std::vector<std::pair<std::string, zlink::stream_connector::subscription_t>> subscriptions;
    std::vector<std::weak_ptr<sending_hook_t>> sending_hooks;
    std::vector<std::weak_ptr<reply_hook_t>> reply_hooks;
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

    void emit_packet (packet_t packet)
    {
        if (!packet_callback) {
            return;
        }
        post_to_main_thread (
          [callback = packet_callback, packet = std::move (packet)] { callback (packet); });
    }

    zlink::stream_connector::subscription_t bind_subscription (std::string packet_name,
                                                               std::weak_ptr<runtime_t> runtime)
    {
        return connector.on<zlink::stream_connector::packet_t> (
          std::move (packet_name),
          [runtime = std::move (runtime)] (
            const zlink::stream_connector::message_t<zlink::stream_connector::packet_t> &message) {
              if (auto owner = runtime.lock ()) {
                  auto packet = to_godot_packet (message.packet_name, message.payload.payload);
                  packet.compressed = message.payload.compressed;
                  packet.metadata = message.metadata.values;
                  owner->emit_packet (std::move (packet));
              }
          });
    }

    void bind_sending_hook (const std::shared_ptr<sending_hook_t> &hook)
    {
        hook->subscription = connector.on_request_sending (
          [hook] (zlink::stream_connector::request_sending_context_t &core) {
              request_sending_context_t context (core.request_packet_name, core.actor_id,
                                                 [&core] (std::string key, std::string value) {
                                                     core.set_metadata (std::move (key),
                                                                        std::move (value));
                                                 });
              hook->callback (context);
          });
    }

    void bind_reply_hook (const std::shared_ptr<reply_hook_t> &hook,
                          std::weak_ptr<runtime_t> runtime)
    {
        hook->subscription = connector.on_reply_received (
          [runtime = std::move (runtime),
           hook] (const zlink::stream_connector::reply_received_context_t &core) {
              reply_received_context_t context;
              context.request_packet_name = core.request_packet_name;
              context.actor_id = core.actor_id;
              context.succeeded = core.succeeded;
              if (core.reply) {
                  context.reply = to_godot_packet (*core.reply);
              }
              if (core.error) {
                  context.error = error_t{core.error->code, core.error->message};
              }
              context.elapsed = core.elapsed;
              if (auto owner = runtime.lock ()) {
                  owner->post_to_main_thread ([hook, context = std::move (context)] {
                      if (hook->subscription.active ()) {
                          hook->callback (context);
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
    for (auto &[name, handle] : _runtime->subscriptions) {
        handle.unsubscribe ();
    }
    _runtime->connector.close ();
    while (_runtime->connector.pending_dispatch_count () != 0) {
        _runtime->connector.dispatch ();
    }
    zlink::stream_connector::connector_options_t options;
    options.endpoint = std::move (endpoint);
    _runtime->connector =
      zlink::stream_connector::connector_factory_t::create (std::move (options));
    for (auto it = _runtime->sending_hooks.begin (); it != _runtime->sending_hooks.end ();) {
        if (auto hook = it->lock ()) {
            _runtime->bind_sending_hook (hook);
            ++it;
        } else {
            it = _runtime->sending_hooks.erase (it);
        }
    }
    for (auto it = _runtime->reply_hooks.begin (); it != _runtime->reply_hooks.end ();) {
        if (auto hook = it->lock ()) {
            _runtime->bind_reply_hook (hook, _runtime);
            ++it;
        } else {
            it = _runtime->reply_hooks.erase (it);
        }
    }
    for (auto &[name, handle] : _runtime->subscriptions) {
        handle = _runtime->bind_subscription (name, _runtime);
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
              owner->emit_request (to_godot_packet (std::move (reply_name), result.value ()));
          }
      });
}

void stream_connector_t::subscribe (std::string packet_name)
{
    zlink::stream_connector::subscription_t handle;
    if (_runtime->current_state != connection_state_t::created
        && _runtime->current_state != connection_state_t::closed) {
        handle = _runtime->bind_subscription (packet_name, _runtime);
    }
    _runtime->subscriptions.emplace_back (std::move (packet_name), std::move (handle));
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

subscription_t
stream_connector_t::on_request_sending (std::function<void (request_sending_context_t &)> callback)
{
    auto hook = std::make_shared<runtime_t::sending_hook_t> ();
    hook->callback = std::move (callback);
    _runtime->sending_hooks.push_back (hook);
    _runtime->bind_sending_hook (hook);
    return subscription_t ([hook] { hook->subscription.unsubscribe (); });
}

subscription_t stream_connector_t::on_reply_received (
  std::function<void (const reply_received_context_t &)> callback)
{
    auto hook = std::make_shared<runtime_t::reply_hook_t> ();
    hook->callback = std::move (callback);
    _runtime->reply_hooks.push_back (hook);
    _runtime->bind_reply_hook (hook, _runtime);
    return subscription_t ([hook] { hook->subscription.unsubscribe (); });
}

} // namespace zlink::godot_stream_connector
