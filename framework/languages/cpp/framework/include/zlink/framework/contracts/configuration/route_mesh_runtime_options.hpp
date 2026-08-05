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

class route_mesh_runtime_options_t
{
  public:
    virtual ~route_mesh_runtime_options_t () = default;
    virtual int placement_weight () const = 0;
    virtual void placement_weight (int value) = 0;
    virtual mesh_channel_runtime_options_t &
    channel (std::string channel_name) = 0;
};

} // namespace zlink::framework
