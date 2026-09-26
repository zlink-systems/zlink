/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <string>

namespace zlink::framework
{

class mesh_channel_runtime_options_t
{
  public:
    virtual ~mesh_channel_runtime_options_t () = default;
    virtual int weight () const = 0;
    virtual void weight (int value) = 0;
};

// Runtime placement weight of one MeshNode. Values are 0..10000; zero excludes the MeshNode from
// new placement without changing existing objects or completed reservations.
class mesh_placement_runtime_options_t
{
  public:
    virtual ~mesh_placement_runtime_options_t () = default;
    virtual int placement_weight () const = 0;
    virtual void placement_weight (int value) = 0;
};

class route_mesh_runtime_options_t
{
  public:
    virtual ~route_mesh_runtime_options_t () = default;
    // An unregistered mesh_name is a configuration error.
    virtual mesh_placement_runtime_options_t &mesh (std::string mesh_name) = 0;
    virtual mesh_channel_runtime_options_t &channel (std::string channel_name) = 0;
};

} // namespace zlink::framework
