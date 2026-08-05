/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/mesh/mesh_node_runtime.hpp"

#include <zlink/framework/contracts/configuration/route_mesh_runtime_options.hpp>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace zlink::framework::runtime
{

class route_mesh_runtime_options_service_t final :
    public route_mesh_runtime_options_t
{
  public:
    explicit route_mesh_runtime_options_service_t (
      std::vector<std::shared_ptr<detail::mesh_node_runtime_t>> nodes);
    ~route_mesh_runtime_options_service_t () override;

    int placement_weight () const override;
    void placement_weight (int value) override;
    mesh_channel_runtime_options_t &
    channel (std::string channel_name) override;

  private:
    class channel_options_t;

    std::vector<std::shared_ptr<detail::mesh_node_runtime_t>> _nodes;
    std::map<std::string, std::unique_ptr<channel_options_t>> _channels;
};

} // namespace zlink::framework::runtime
