/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/channels/channel_runtime.hpp"

#include <memory>
#include <string>

namespace zlink::framework::detail
{

class channel_bundle_factory_t
{
  public:
    std::shared_ptr<channel_runtime_bundle_t>
    create_client_bundle (const std::string &channel_name, const channel_snapshot_t &channel) const;

    std::shared_ptr<channel_runtime_bundle_t>
    create_server_bundle (const std::string &channel_name, const channel_snapshot_t &channel) const;

    std::shared_ptr<channel_runtime_bundle_t>
    create_publisher_bundle (const std::string &channel_name,
                             const channel_snapshot_t &channel) const;

    std::shared_ptr<channel_runtime_bundle_t>
    create_subscriber_bundle (const std::string &channel_name,
                              const channel_snapshot_t &channel) const;

  private:
    static std::shared_ptr<channel_runtime_bundle_t>
    create_bundle (const std::string &channel_name,
                   const channel_capability_snapshot_t &capability,
                   bool attach_bind_endpoints);
};

} // namespace zlink::framework::detail
