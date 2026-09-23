/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/stream_connector/contracts/zlink_stream_enums.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace zlink::godot_stream_connector
{

enum class connection_state_t : std::uint8_t
{
    created,
    connecting,
    connected,
    reconnecting,
    disconnected,
    closed
};

using error_code_t = zlink::stream_connector::error_code_t;

struct packet_t
{
    std::string name;
    std::vector<std::uint8_t> payload;
    std::map<std::string, std::string> metadata;
    bool compressed = false;
};

struct send_options_t
{
    std::map<std::string, std::string> metadata;
    bool compress = false;
};

struct request_sending_context_t
{
    std::string request_packet_name;
    std::optional<std::string> actor_id;
    void set_metadata (std::string key, std::string value);
    const std::map<std::string, std::string> &metadata_values () const { return _metadata; }

  private:
    std::map<std::string, std::string> _metadata;
};

struct error_t
{
    error_code_t code;
    std::string message;
};

struct reply_received_context_t
{
    std::string request_packet_name;
    std::optional<std::string> actor_id;
    bool succeeded = false;
    std::optional<packet_t> reply;
    std::optional<error_t> error;
    std::chrono::milliseconds elapsed{0};
};

struct request_result_t
{
    std::optional<packet_t> reply;
    std::optional<error_code_t> error_code;
    std::string error_message;
};

class subscription_t
{
  public:
    subscription_t () = default;
    ~subscription_t () { unsubscribe (); }
    subscription_t (subscription_t &&) noexcept = default;
    subscription_t &operator= (subscription_t &&other) noexcept;
    subscription_t (const subscription_t &) = delete;
    subscription_t &operator= (const subscription_t &) = delete;
    void unsubscribe ();
    bool active () const;

  private:
    friend class stream_connector_t;
    subscription_t (std::function<void ()> unsubscribe, std::function<bool ()> is_active);
    std::function<void ()> _unsubscribe;
    std::function<bool ()> _is_active;
};

class stream_connector_t
{
  public:
    stream_connector_t ();
    ~stream_connector_t ();

    stream_connector_t (stream_connector_t &&) noexcept;
    stream_connector_t &operator= (stream_connector_t &&) noexcept;
    stream_connector_t (const stream_connector_t &) = delete;
    stream_connector_t &operator= (const stream_connector_t &) = delete;

    void connect (std::string endpoint);
    void close ();
    void send_json (std::string packet_name, std::string json_payload);
    void send_json (std::string packet_name, std::string json_payload, send_options_t options);
    void request_json (std::string packet_name,
                       std::string json_payload,
                       double timeout_seconds,
                       std::function<void (const request_result_t &)> callback);
    subscription_t on (std::string packet_name, std::function<void (const packet_t &)> callback);
    void dispatch ();
    void set_main_thread_dispatcher (std::function<void (std::function<void ()>)> dispatcher);

    connection_state_t state () const;
    void on_connection_state_changed (std::function<void (connection_state_t)> callback);
    subscription_t on_request_sending (std::function<void (request_sending_context_t &)> callback);
    subscription_t
    on_reply_received (std::function<void (const reply_received_context_t &)> callback);

  private:
    class runtime_t;
    std::shared_ptr<runtime_t> _runtime;
};

} // namespace zlink::godot_stream_connector
