#pragma once

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/label.hpp>
#include <zlink_godot_stream_connector.hpp>

namespace godot
{

class EngineLobbyNode : public Control
{
    GDCLASS (EngineLobbyNode, Control)

  public:
    void _ready () override;
    void _process (double delta) override;
    void _exit_tree () override;

    void set_endpoint (String endpoint);
    String get_endpoint () const;
    void set_player_name (String name);
    String get_player_name () const;
    void set_first_chat (String text);
    String get_first_chat () const;

  protected:
    static void _bind_methods ();

  private:
    void handle_response (const zlink::godot_stream_connector::packet_t &packet);
    void handle_packet (const zlink::godot_stream_connector::packet_t &packet);
    void fail (const String &message);

    zlink::godot_stream_connector::stream_connector_t connector_;
    Label *status_ = nullptr;
    String endpoint_ = "ws://127.0.0.1:22700";
    String player_name_ = "godot-player";
    String first_chat_ = "hello from Godot C++";
    bool failed_ = false;
};

} // namespace godot
