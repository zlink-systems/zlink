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
    return framework_exception_t (framework_error_kind_t::protocol_error, std::move (message));
}

} // namespace

class route_mesh_runtime_options_service_t::mesh_options_t final
    : public mesh_placement_runtime_options_t
{
  public:
    explicit mesh_options_t (std::shared_ptr<detail::mesh_node_runtime_t> node) :
        _node (std::move (node))
    {
    }

    int placement_weight () const override { return _node->placement_weight (); }

    void placement_weight (int value) override
    {
        if (value < 0 || value > 10000)
            throw runtime_options_error ("placement weight must be in range 0..10000");
        _node->set_placement_weight (value);
    }

  private:
    std::shared_ptr<detail::mesh_node_runtime_t> _node;
};

class route_mesh_runtime_options_service_t::channel_options_t final
    : public mesh_channel_runtime_options_t
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
            throw runtime_options_error ("RouteMesh channel is not configured: " + _channel_name);
        return found->second;
    }

    void weight (int value) override
    {
        if (value < 0 || value > 10000)
            throw runtime_options_error ("channel weight must be in range 0..10000");
        _node->set_channel_weight (_channel_name, value);
    }

  private:
    std::shared_ptr<detail::mesh_node_runtime_t> _node;
    std::string _channel_name;
};

route_mesh_runtime_options_service_t::route_mesh_runtime_options_service_t (
  std::vector<std::shared_ptr<detail::mesh_node_runtime_t>> nodes)
{
    for (const auto &node : nodes) {
        _meshes.emplace (node->mesh_name (), std::make_unique<mesh_options_t> (node));
        for (const auto &[channel_name, _] : node->channel_weights ()) {
            if (!_channels
                   .emplace (channel_name, std::make_unique<channel_options_t> (node, channel_name))
                   .second)
                throw runtime_options_error (
                  "RouteMesh ChannelName is registered by more than one MeshNode: " + channel_name);
        }
    }
}

route_mesh_runtime_options_service_t::~route_mesh_runtime_options_service_t () = default;

mesh_placement_runtime_options_t &route_mesh_runtime_options_service_t::mesh (std::string mesh_name)
{
    const auto found = _meshes.find (mesh_name);
    if (found == _meshes.end ())
        throw framework_exception_t (framework_error_kind_t::not_configured,
                                     "RouteMesh is not configured: " + mesh_name);
    return *found->second;
}

mesh_channel_runtime_options_t &
route_mesh_runtime_options_service_t::channel (std::string channel_name)
{
    if (channel_name.empty ())
        throw runtime_options_error ("channel_name is required");
    const auto found = _channels.find (channel_name);
    if (found == _channels.end ())
        throw runtime_options_error ("RouteMesh channel is not configured: " + channel_name);
    return *found->second;
}

} // namespace zlink::framework::runtime
