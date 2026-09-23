#include "EngineLobbyScene.h"

#include "EngineLobbyProtocol.h"

#include <nlohmann/json.hpp>

namespace {
constexpr char endpoint[] = "ws://127.0.0.1:22700";
constexpr char player_name[] = "axmol-player";
constexpr char first_chat[] = "hello from Axmol";

nlohmann::json
payload_json(const zlink::axmol_stream_connector::packet_t &packet) {
  return nlohmann::json::parse(packet.payload.begin(), packet.payload.end(),
                               nullptr, false);
}

bool required_string(const nlohmann::json &json, const char *field,
                     std::string &value) {
  if (!json.is_object() || !json.contains(field) || !json[field].is_string()) {
    return false;
  }
  value = json[field].get<std::string>();
  return !value.empty();
}
} // namespace

bool EngineLobbyScene::init() {
  if (!Scene::init()) {
    return false;
  }
  const auto size = ax::Director::getInstance()->getVisibleSize();
  status_ =
      ax::Label::createWithSystemFont("Engine Lobby: connecting", "Arial", 24);
  status_->setPosition(ax::Vec2(size.width / 2, size.height / 2));
  addChild(status_);

  // --8<-- [start:connect]
  connector_.on_request_completed(
      [this](const auto &packet) { handle_reply(packet); });
  connector_.on_packet([this](const auto &packet) { handle_packet(packet); });
  connector_.connect(endpoint);
  if (connector_.state() !=
      zlink::axmol_stream_connector::connection_state_t::connected) {
    set_status("Engine Lobby: connection failed");
    return true;
  }
  send_ping();
  // --8<-- [end:connect]

  // --8<-- [start:pump]
  schedule([this](float) { connector_.dispatch(); }, "engine-lobby-pump");
  // --8<-- [end:pump]
  return true;
}

void EngineLobbyScene::send_ping() {
  connector_.request_json(
      engine_lobby::packet::ping_req,
      nlohmann::json({{engine_lobby::field::sent_at_unix_ms, "1000"}}).dump(),
      5.0);
}

// --8<-- [start:handler]
void EngineLobbyScene::handle_reply(
    const zlink::axmol_stream_connector::packet_t &packet) {
  const auto json = payload_json(packet);
  if (packet.name == engine_lobby::packet::ping_res) {
    std::string sent_at;
    if (!required_string(json, engine_lobby::field::sent_at_unix_ms, sent_at) ||
        sent_at != "1000") {
      set_status("Engine Lobby: invalid PingRes");
      return;
    }
    send_join();
  } else if (packet.name == engine_lobby::packet::join_res) {
    std::string name;
    if (!required_string(json, engine_lobby::field::actor_id, actor_id_) ||
        !required_string(json, engine_lobby::field::name, name) ||
        name != player_name) {
      set_status("Engine Lobby: invalid JoinRes");
      return;
    }
    set_status("joined as " + name + " (" + actor_id_ + ")");
    send_chat();
  }
}

void EngineLobbyScene::handle_packet(
    const zlink::axmol_stream_connector::packet_t &packet) {
  if (packet.name != engine_lobby::packet::chat_notify) {
    return;
  }
  const auto json = payload_json(packet);
  std::string actor_id;
  std::string name;
  std::string text;
  if (!required_string(json, engine_lobby::field::actor_id, actor_id) ||
      !required_string(json, engine_lobby::field::name, name) ||
      !required_string(json, engine_lobby::field::text, text) ||
      actor_id != actor_id_ || name != player_name || text != first_chat) {
    set_status("Engine Lobby: invalid ChatNotify");
    return;
  }
  set_status(name + ": " + text);
}

void EngineLobbyScene::send_join() {
  connector_.request_json(
      engine_lobby::packet::join_req,
      nlohmann::json({{engine_lobby::field::name, player_name}}).dump(), 5.0);
}

void EngineLobbyScene::send_chat() {
  connector_.send_json(
      engine_lobby::packet::chat_msg,
      nlohmann::json({{engine_lobby::field::text, first_chat}}).dump());
}
// --8<-- [end:handler]

// --8<-- [start:lifecycle]
void EngineLobbyScene::onExit() {
  unschedule("engine-lobby-pump");
  connector_.on_request_completed({});
  connector_.on_packet({});
  connector_.close();
  Scene::onExit();
}
// --8<-- [end:lifecycle]

void EngineLobbyScene::set_status(const std::string &status) {
  status_->setString(status);
  AXLOG("%s", status.c_str());
}
