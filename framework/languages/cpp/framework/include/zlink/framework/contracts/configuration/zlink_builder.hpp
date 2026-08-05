/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/channels/channel.hpp>
#include <zlink/framework/contracts/configuration/mesh_node.hpp>
#include <zlink/framework/contracts/spots/spot.hpp>
#include <zlink/framework/contracts/streams/stream.hpp>

#include <cstddef>
#include <chrono>
#include <memory>
#include <string>

namespace zlink::framework
{

struct dispatch_options_t;
class zlink_builder_t;

namespace detail
{
class channel_runtime_manager_t;
class mesh_node_runtime_t;
class spot_node_runtime_t;
class zlink_builder_state_t;
void apply_dispatch_options (zlink_builder_t &builder, const dispatch_options_t &options);
void bind_stream_serializers (zlink_builder_t &builder, serializer_registry_t &serializers);
void apply_stream_compression_codec (zlink_builder_t &builder,
                                     std::shared_ptr<const stream_compression_codec_t> codec);
void drain_zlink_builder_runtime (zlink_builder_t &builder) noexcept;
class monitoring_runtime_state_t;
void bind_zlink_monitoring (
  zlink_builder_t &builder,
  std::shared_ptr<monitoring_runtime_state_t> monitoring);
} // namespace detail

class zlink_builder_t
{
  public:
    zlink_builder_t ();
    ~zlink_builder_t ();

    zlink_builder_t (zlink_builder_t &&) noexcept;
    zlink_builder_t &operator= (zlink_builder_t &&) noexcept;
    zlink_builder_t (const zlink_builder_t &) = delete;
    zlink_builder_t &operator= (const zlink_builder_t &) = delete;

    zlink_builder_t &add_node (std::string node_name);
    zlink_builder_t &max_pending (std::size_t count);
    zlink_builder_t &default_request_timeout (std::chrono::milliseconds timeout);
    route_channel_builder_t route_channel (std::string route_channel_name);
    channel_builder_t channel (std::string channel_name);
    mesh_node_builder_t add_route_mesh (std::string mesh_name);
    stream_builder_t stream (std::string stream_name);

    message_bus_t message_bus () const;
    request_client_t request_client (std::string channel_name) const;
    publisher_t publisher () const;
    route_client_t route_client (serializer_registry_t &serializers) const;

  private:
    friend void detail::apply_dispatch_options (zlink_builder_t &builder,
                                                const dispatch_options_t &options);
    friend void detail::bind_stream_serializers (zlink_builder_t &builder,
                                                 serializer_registry_t &serializers);
    friend void detail::apply_stream_compression_codec (
      zlink_builder_t &builder, std::shared_ptr<const stream_compression_codec_t> codec);
    friend void detail::drain_zlink_builder_runtime (zlink_builder_t &builder) noexcept;
    friend void detail::bind_zlink_monitoring (
      zlink_builder_t &builder,
      std::shared_ptr<detail::monitoring_runtime_state_t> monitoring);
    friend class detail::channel_runtime_manager_t;
    friend class detail::mesh_node_runtime_t;
    friend class detail::spot_node_runtime_t;
    friend class detail::stream_runtime_t;
    std::shared_ptr<detail::zlink_builder_state_t> _state;
};

} // namespace zlink::framework
