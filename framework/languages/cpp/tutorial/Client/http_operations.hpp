#pragma once

#include "../Shared/contracts.hpp"

#include <zlink/framework.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace fw = zlink::framework;

class redirect_player_http_handler_t
{
  public:
    fw::http_response_t handle (const fw::http_request_t &request) const
    {
        return fw::http_response_t{301, ""}.header (
          "Location", "/players/" + request.route_values.at ("playerId"));
    }
};

class export_room_http_handler_t
{
  public:
    explicit export_room_http_handler_t (fw::route_client_t &routes) : _routes (routes) {}

    fw::task_t<fw::http_response_t> handle (const fw::http_request_t &request)
    {
        const auto room_id = request.route_values.at ("roomId");
        const auto state = co_await _routes.request_to_spot (room_id, get_room_state_t{})
                             .timeout (std::chrono::seconds (3))
                             .async<room_state_t> ();

        std::string body = nlohmann::json{{"roomId", room_id}}.dump () + "\n";
        for (const auto &message : state.chat)
            body += nlohmann::json{{"message", message}}.dump () + "\n";

        co_return fw::http_response_t{200, std::move (body), "application/x-ndjson"};
    }

  private:
    fw::route_client_t &_routes;
};

class import_room_http_handler_t
{
  public:
    explicit import_room_http_handler_t (fw::route_client_t &routes) : _routes (routes) {}

    fw::task_t<fw::http_response_t> handle (const fw::http_request_t &request)
    {
        const auto room_id = request.route_values.at ("roomId");
        std::istringstream lines (request.body);
        std::string line;
        int imported = 0;
        while (std::getline (lines, line)) {
            if (!line.empty () && line.back () == '\r')
                line.pop_back ();
            if (line.empty ())
                continue;
            const auto message = nlohmann::json::parse (line).get<post_chat_t> ();
            co_await _routes.send_to_spot (room_id, message).async ();
            ++imported;
        }

        co_return fw::http_response_t{
          200, nlohmann::json{{"imported", imported}}.dump ()};
    }

  private:
    fw::route_client_t &_routes;
};
