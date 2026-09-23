#pragma once

#include "axmol.h"
#include <zlink_axmol_stream_connector.hpp>

#include <string>

class EngineLobbyScene final : public ax::Scene {
public:
  bool init() override;
  void onExit() override;
  CREATE_FUNC(EngineLobbyScene);

private:
  void send_ping();
  void send_join();
  void send_chat();
  void handle_reply(const zlink::axmol_stream_connector::packet_t &packet);
  void handle_packet(const zlink::axmol_stream_connector::packet_t &packet);
  void set_status(const std::string &status);

  zlink::axmol_stream_connector::stream_connector_t connector_;
  ax::Label *status_ = nullptr;
  std::string actor_id_;
};
