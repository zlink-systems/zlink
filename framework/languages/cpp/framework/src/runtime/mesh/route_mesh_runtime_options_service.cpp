/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/mesh/route_mesh_runtime_options_service.hpp"

#include <zlink/framework/contracts/errors/error.hpp>

#include <utility>

namespace zlink::framework::runtime
{

namespace
{

framework_exception_t runtime_options_error (std::string message)
{
    return framework_exception_t (framework_error_kind_t::protocol_error,
                                  std::move (message));
}

} // namespace

class route_mesh_runtime_options_service_t::channel_options_t final :
    public mesh_channel_runtime_options_t
{
  public:
    channel_options_t (std::shared_ptr<detail::mesh_node_runtime_t> node,
                       std::string channel_name) :
        _node (std::move (node)), _channel_name (std::move (channel_name))
    {
    }

    int weight () const override
    {
        const auto channels = _node->channel_weights ();
        const auto found = channels.find (_channel_name);
        if (found == channels.end ())
            throw runtime_options_error ("RouteMesh channel is not configured: "
                                         + _channel_name);
        return found->second;
    }

    void weight (int value) override
    {
        if (value < 0 || value > 10000)
            throw runtime_options_error (
              "channel weight must be in range 0..10000");
        _node->set_channel_weight (_channel_name, value);
    }

  private:
    std::shared_ptr<detail::mesh_node_runtime_t> _node;
    std::string _channel_name;
};

route_mesh_runtime_options_service_t::route_mesh_runtime_options_service_t (
  std::vector<std::shared_ptr<detail::mesh_node_runtime_t>> nodes) :
    _nodes (std::move (nodes))
{
    for (const auto &node : _nodes) {
        for (const auto &[channel_name, _] : node->channel_weights ()) {
            if (!_channels
                   .emplace (
                     channel_name,
                     std::make_unique<channel_options_t> (
                       node, channel_name))
                   .second)
                throw runtime_options_error (
                  "RouteMesh ChannelName is registered by more than one MeshNode: "
                  + channel_name);
        }
    }
}

route_mesh_runtime_options_service_t::~route_mesh_runtime_options_service_t () =
  default;

int route_mesh_runtime_options_service_t::placement_weight () const
{
    if (_nodes.empty ())
        throw runtime_options_error ("RouteMesh is not configured");
    return _nodes.front ()->placement_weight ();
}

void route_mesh_runtime_options_service_t::placement_weight (int value)
{
    if (value < 0 || value > 10000)
        throw runtime_options_error (
          "placement weight must be in range 0..10000");
    for (const auto &node : _nodes)
        node->set_placement_weight (value);
}

mesh_channel_runtime_options_t &
route_mesh_runtime_options_service_t::channel (std::string channel_name)
{
    if (channel_name.empty ())
        throw runtime_options_error ("channel_name is required");
    const auto found = _channels.find (channel_name);
    if (found == _channels.end ())
        throw runtime_options_error (
          "RouteMesh channel is not configured: " + channel_name);
    return *found->second;
}

} // namespace zlink::framework::runtime
