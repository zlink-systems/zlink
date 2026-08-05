/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
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
    void request_json (std::string packet_name, std::string json_payload, double timeout_seconds);
    void dispatch ();
    void set_main_thread_dispatcher (std::function<void (std::function<void ()>)> dispatcher);

    connection_state_t state () const;
    void on_packet (std::function<void (const packet_t &)> callback);
    void on_request_completed (std::function<void (const packet_t &)> callback);
    void on_connection_state_changed (std::function<void (connection_state_t)> callback);

  private:
    class runtime_t;
    std::shared_ptr<runtime_t> _runtime;
};

} // namespace zlink::godot_stream_connector
