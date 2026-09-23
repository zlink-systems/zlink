#pragma once

#include <zlink/framework.hpp>

#include <nlohmann/json.hpp>

#include <string>

namespace fw = zlink::framework;

// Weight is the one value this node can change while running. 0 keeps the
// socket open and finishes in-flight work, but other nodes stop choosing this
// one for new calls. 100 is the normal value.
//
// An HTTP handler asks for what it needs in its constructor, and the runtime
// surface for the change is route_mesh_runtime_options_t. It is the mesh as it
// runs, not the builder that configured it, so there is no way to reach a value
// the builder never opened.
// --8<-- [start:weight-runtime]
class channel_weight_handler_t
{
  public:
    explicit channel_weight_handler_t (fw::route_mesh_runtime_options_t &mesh) : _mesh (mesh) {}

    fw::http_response_t handle (const fw::http_request_t &request)
    {
        const auto channel = request.route_values.at ("channel");

        const auto given = request.query_values.find ("value");
        if (given == request.query_values.end ())
            return fw::http_response_t{400, R"({"error":"value is required"})"};

        const auto value = std::stoi (given->second);

        auto &options = _mesh.channel (channel);
        options.weight (value);

        return fw::http_response_t{
          200, nlohmann::json{{"channel", channel}, {"weight", options.weight ()}}.dump ()};
    }

  private:
    fw::route_mesh_runtime_options_t &_mesh;
};
// --8<-- [end:weight-runtime]
