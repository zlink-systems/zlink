#include "engine_lobby_node.hpp"

#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <string>

namespace godot
{
namespace
{
constexpr char ping_req[] = "PingReq";
constexpr char join_req[] = "JoinReq";
constexpr char chat_msg[] = "ChatMsg";
constexpr char chat_notify[] = "ChatNotify";

std::string encode_field (const char *field, const String &value)
{
    Dictionary object;
    object[String (field)] = value;
    const CharString utf8 = JSON::stringify (object).utf8 ();
    return std::string (utf8.get_data (), utf8.length ());
}

bool decode_fields (const zlink::godot_stream_connector::packet_t &packet, Dictionary &object)
{
    const String json = String::utf8 (reinterpret_cast<const char *> (packet.payload.data ()),
                                      static_cast<int> (packet.payload.size ()));
    const Variant parsed = JSON::parse_string (json);
    if (parsed.get_type () != Variant::DICTIONARY) {
        return false;
    }
    object = parsed;
    return true;
}

bool required_string (const Dictionary &object, const char *field, String &value)
{
    const String key (field);
    if (!object.has (key) || object[key].get_type () != Variant::STRING) {
        return false;
    }
    value = object[key];
    return !value.is_empty ();
}
} // namespace

void EngineLobbyNode::_bind_methods ()
{
    ClassDB::bind_method (D_METHOD ("set_endpoint", "endpoint"), &EngineLobbyNode::set_endpoint);
    ClassDB::bind_method (D_METHOD ("get_endpoint"), &EngineLobbyNode::get_endpoint);
    ClassDB::bind_method (D_METHOD ("set_player_name", "name"), &EngineLobbyNode::set_player_name);
    ClassDB::bind_method (D_METHOD ("get_player_name"), &EngineLobbyNode::get_player_name);
    ClassDB::bind_method (D_METHOD ("set_first_chat", "text"), &EngineLobbyNode::set_first_chat);
    ClassDB::bind_method (D_METHOD ("get_first_chat"), &EngineLobbyNode::get_first_chat);
    ADD_PROPERTY (PropertyInfo (Variant::STRING, "endpoint"), "set_endpoint", "get_endpoint");
    ADD_PROPERTY (PropertyInfo (Variant::STRING, "player_name"), "set_player_name",
                  "get_player_name");
    ADD_PROPERTY (PropertyInfo (Variant::STRING, "first_chat"), "set_first_chat", "get_first_chat");
}

void EngineLobbyNode::set_endpoint (String endpoint)
{
    endpoint_ = endpoint;
}
String EngineLobbyNode::get_endpoint () const
{
    return endpoint_;
}
void EngineLobbyNode::set_player_name (String name)
{
    player_name_ = name;
}
String EngineLobbyNode::get_player_name () const
{
    return player_name_;
}
void EngineLobbyNode::set_first_chat (String text)
{
    first_chat_ = text;
}
String EngineLobbyNode::get_first_chat () const
{
    return first_chat_;
}

// --8<-- [start:connect]
void EngineLobbyNode::_ready ()
{
    status_ = get_node<Label> ("Status");
    chat_subscription_ =
      connector_.on (chat_notify, [this] (const auto &packet) { handle_packet (packet); });
    const CharString endpoint = endpoint_.utf8 ();
    connector_.connect (std::string (endpoint.get_data (), endpoint.length ()));
    if (connector_.state () != zlink::godot_stream_connector::connection_state_t::connected) {
        fail ("connection failed");
        return;
    }
    connector_.request_json (ping_req, encode_field ("sentAtUnixMs", "1000"), 5.0,
                             [this] (const auto &result) { handle_ping_response (result); });
    set_process (true);
}
// --8<-- [end:connect]

// --8<-- [start:pump]
void EngineLobbyNode::_process (double)
{
    if (!failed_) {
        connector_.dispatch ();
    }
}
// --8<-- [end:pump]

// --8<-- [start:ping-handler]
void EngineLobbyNode::handle_ping_response (
  const zlink::godot_stream_connector::request_result_t &result)
{
    if (!result.reply) {
        fail (String (result.error_message.c_str ()));
        return;
    }
    Dictionary object;
    if (!decode_fields (*result.reply, object)) {
        fail ("invalid reply JSON");
        return;
    }
    String sent_at;
    if (!required_string (object, "sentAtUnixMs", sent_at) || sent_at != "1000") {
        fail ("invalid PingRes");
        return;
    }
    connector_.request_json (join_req, encode_field ("name", player_name_), 5.0,
                             [this] (const auto &reply) { handle_join_response (reply); });
}
// --8<-- [end:ping-handler]

// --8<-- [start:join-handler]
void EngineLobbyNode::handle_join_response (
  const zlink::godot_stream_connector::request_result_t &result)
{
    if (!result.reply) {
        fail (String (result.error_message.c_str ()));
        return;
    }
    Dictionary object;
    String actor_id;
    String name;
    if (!decode_fields (*result.reply, object) || !required_string (object, "actorId", actor_id)
        || !required_string (object, "name", name) || name != player_name_) {
        fail ("invalid JoinRes");
        return;
    }
    status_->set_text (String ("joined as ") + name + " (" + actor_id + ")");
    connector_.send_json (chat_msg, encode_field ("text", first_chat_));
}
// --8<-- [end:join-handler]

// --8<-- [start:receive]
void EngineLobbyNode::handle_packet (const zlink::godot_stream_connector::packet_t &packet)
{
    Dictionary object;
    String actor_id;
    String name;
    String text;
    if (!decode_fields (packet, object) || !required_string (object, "actorId", actor_id)
        || !required_string (object, "name", name) || !required_string (object, "text", text)) {
        fail ("invalid ChatNotify");
        return;
    }
    status_->set_text (name + ": " + text);
}
// --8<-- [end:receive]

void EngineLobbyNode::fail (const String &message)
{
    failed_ = true;
    status_->set_text (String ("Engine Lobby failed: ") + message);
    set_process (false);
}

// --8<-- [start:lifecycle]
void EngineLobbyNode::_exit_tree ()
{
    chat_subscription_.unsubscribe ();
    connector_.close ();
}
// --8<-- [end:lifecycle]

} // namespace godot
