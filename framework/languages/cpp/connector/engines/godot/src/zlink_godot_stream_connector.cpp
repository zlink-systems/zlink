/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink_godot_stream_connector.hpp>

#include <zlink/stream_connector.hpp>

#if __has_include(<godot_cpp/variant/utility_functions.hpp>)
#include <godot_cpp/variant/utility_functions.hpp>
#define ZLINK_GODOT_HAS_LOGGER 1
#endif

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace zlink::godot_stream_connector
{

namespace
{

packet_t to_godot_packet (std::string name, const std::vector<std::uint8_t> &payload)
{
    packet_t packet;
    packet.name = std::move (name);
    packet.payload = payload;
    return packet;
}

packet_t to_godot_packet (const zlink::stream_connector::packet_t &source)
{
    auto packet = to_godot_packet (source.name, source.payload);
    packet.metadata = source.metadata.values;
    packet.compressed = source.compressed;
    return packet;
}

void log_callback_error (const char *message)
{
#if defined(ZLINK_GODOT_HAS_LOGGER)
    godot::UtilityFunctions::push_error (message);
#else
    std::fprintf (stderr, "ZLink stream connector callback failed: %s\n", message);
#endif
}

template <typename Callback> void invoke_callback (Callback &&callback)
{
#if ZLINK_HAS_EXCEPTIONS
    try {
        callback ();
    }
    catch (const std::exception &error) {
        log_callback_error (error.what ());
    }
    catch (...) {
        log_callback_error ("unknown exception");
    }
#else
    callback ();
#endif
}

} // namespace

void request_sending_context_t::set_metadata (std::string key, std::string value)
{
    _metadata[std::move (key)] = std::move (value);
}

class stream_connector_t::runtime_t
{
  public:
    struct registration_t
    {
        std::string name;
        std::function<void (const packet_t &)> callback;
        zlink::stream_connector::subscription_t handle;
    };

    zlink::stream_connector::connector_t connector;
    std::map<std::uint64_t, registration_t> subscriptions;
    std::uint64_t next_subscription_id = 1;
    template <typename Callback> struct hook_entry_t
    {
        Callback callback;
        zlink::stream_connector::subscription_t handle;
    };
    using sending_entry_t = hook_entry_t<std::function<void (request_sending_context_t &)>>;
    using reply_entry_t = hook_entry_t<std::function<void (const reply_received_context_t &)>>;
    std::map<std::uint64_t, std::shared_ptr<sending_entry_t>> sending_hooks;
    std::map<std::uint64_t, std::shared_ptr<reply_entry_t>> reply_hooks;
    std::uint64_t next_hook_id = 1;
    connection_state_t current_state = connection_state_t::created;
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
            main_thread_dispatcher (
              [callback = std::move (callback)] { invoke_callback (callback); });
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
            invoke_callback (callback);
        }
    }

    void emit_state (connection_state_t state)
    {
        if (!state_callback) {
            return;
        }
        post_to_main_thread ([callback = state_callback, state] { callback (state); });
    }

    zlink::stream_connector::subscription_t bind_subscription (std::uint64_t id,
                                                               const std::string &packet_name,
                                                               std::weak_ptr<runtime_t> runtime)
    {
        return connector.on<zlink::stream_connector::packet_t> (
          packet_name,
          [runtime = std::move (runtime), id] (
            const zlink::stream_connector::message_t<zlink::stream_connector::packet_t> &message) {
              if (auto owner = runtime.lock ()) {
                  auto packet = to_godot_packet (message.packet_name, message.payload.payload);
                  packet.compressed = message.payload.compressed;
                  packet.metadata = message.metadata.values;
                  owner->post_to_main_thread ([runtime, id, packet = std::move (packet)] {
                      if (auto current = runtime.lock ()) {
                          const auto found = current->subscriptions.find (id);
                          if (found != current->subscriptions.end ()) {
                              auto callback = found->second.callback;
                              callback (packet);
                          }
                      }
                  });
              }
          });
    }

    void register_sending_hook (const std::shared_ptr<sending_entry_t> &entry)
    {
        entry->handle = connector.on_request_sending (
          [weak_entry = std::weak_ptr<sending_entry_t> (entry)] (
            zlink::stream_connector::request_sending_context_t &source) {
              if (auto registered = weak_entry.lock ()) {
                  request_sending_context_t context;
                  context.request_packet_name = source.request_packet_name;
                  context.actor_id = source.actor_id;
                  invoke_callback ([&] { registered->callback (context); });
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
                      context.reply = to_godot_packet (*source.reply);
                  }
                  if (source.error) {
                      context.error = error_t{source.error->code, source.error->message};
                  }
                  owner->post_to_main_thread ([weak_entry, context = std::move (context)] {
                      if (auto registered = weak_entry.lock ()) {
                          registered->callback (context);
                      }
                  });
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
    _runtime->connector.close ();
    while (_runtime->connector.pending_dispatch_count () != 0) {
        _runtime->connector.dispatch ();
    }
    for (auto &[id, registration] : _runtime->subscriptions) {
        registration.handle.unsubscribe ();
    }
    for (auto &[id, entry] : _runtime->sending_hooks) {
        entry->handle.unsubscribe ();
    }
    for (auto &[id, entry] : _runtime->reply_hooks) {
        entry->handle.unsubscribe ();
    }
    zlink::stream_connector::connector_options_t options;
    options.endpoint = std::move (endpoint);
    _runtime->connector =
      zlink::stream_connector::connector_factory_t::create (std::move (options));
    for (auto &[id, registration] : _runtime->subscriptions) {
        registration.handle = _runtime->bind_subscription (id, registration.name, _runtime);
    }
    for (auto &[id, entry] : _runtime->sending_hooks) {
        _runtime->register_sending_hook (entry);
    }
    for (auto &[id, entry] : _runtime->reply_hooks) {
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
    packet.payload.assign (json_payload.begin (), json_payload.end ());
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
    if (!callback) {
        return;
    }
    zlink::stream_connector::packet_t packet;
    packet.name = std::move (packet_name);
    packet.codec = zlink::stream_connector::codec_t::json;
    packet.payload.assign (json_payload.begin (), json_payload.end ());
    auto request = _runtime->connector.request (std::move (packet));
    request.timeout (std::chrono::milliseconds (static_cast<int> (timeout_seconds * 1000.0)));
    request.submit<std::vector<std::uint8_t>> (
      [runtime = std::weak_ptr<runtime_t> (_runtime), callback = std::move (callback)] (
        zlink::stream_connector::result_t<std::vector<std::uint8_t>> result) mutable {
          if (auto owner = runtime.lock ()) {
              request_result_t delivered;
              if (result) {
                  delivered.reply = to_godot_packet ({}, result.value ());
              } else {
                  delivered.error_code = result.error ()->code;
                  delivered.error_message = result.error ()->message;
              }
              owner->post_to_main_thread (
                [callback = std::move (callback), delivered = std::move (delivered)] () mutable {
                    callback (delivered);
                });
          }
      });
}

subscription_t stream_connector_t::on (std::string packet_name,
                                       std::function<void (const packet_t &)> callback)
{
    const auto id = _runtime->next_subscription_id++;
    runtime_t::registration_t registration{std::move (packet_name), std::move (callback), {}};
    if (_runtime->current_state != connection_state_t::created
        && _runtime->current_state != connection_state_t::closed) {
        registration.handle = _runtime->bind_subscription (id, registration.name, _runtime);
    }
    _runtime->subscriptions.emplace (id, std::move (registration));
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

void stream_connector_t::on_connection_state_changed (
  std::function<void (connection_state_t)> callback)
{
    _runtime->state_callback = std::move (callback);
}

subscription_t
stream_connector_t::on_request_sending (std::function<void (request_sending_context_t &)> callback)
{
    const auto id = _runtime->next_hook_id++;
    auto entry = std::make_shared<runtime_t::sending_entry_t> ();
    entry->callback = std::move (callback);
    _runtime->sending_hooks.emplace (id, entry);
    _runtime->register_sending_hook (entry);
    auto runtime = std::weak_ptr<runtime_t> (_runtime);
    return subscription_t (
      [runtime, id] {
          if (auto owner = runtime.lock ()) {
              owner->sending_hooks.erase (id);
          }
      },
      [runtime, id] {
          if (auto owner = runtime.lock ()) {
              return owner->sending_hooks.contains (id);
          }
          return false;
      });
}

subscription_t stream_connector_t::on_reply_received (
  std::function<void (const reply_received_context_t &)> callback)
{
    const auto id = _runtime->next_hook_id++;
    auto entry = std::make_shared<runtime_t::reply_entry_t> ();
    entry->callback = std::move (callback);
    _runtime->reply_hooks.emplace (id, entry);
    _runtime->register_reply_hook (entry, _runtime);
    auto runtime = std::weak_ptr<runtime_t> (_runtime);
    return subscription_t (
      [runtime, id] {
          if (auto owner = runtime.lock ()) {
              owner->reply_hooks.erase (id);
          }
      },
      [runtime, id] {
          if (auto owner = runtime.lock ()) {
              return owner->reply_hooks.contains (id);
          }
          return false;
      });
}

} // namespace zlink::godot_stream_connector
