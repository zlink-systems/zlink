/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace zlink::framework
{
class actor_ref_t;
}

namespace zlink::framework::runtime
{
struct serial_application_record_t;
}

namespace zlink::framework::runtime::messaging
{
struct envelope_header_t;
}

namespace zlink::framework::runtime::protocol
{
struct actor_route_fence_t;
}

namespace zlink::framework::test::actor_gateway
{
zlink::framework::actor_ref_t test_actor_ref (std::string node,
                                              std::string actor_type,
                                              std::string actor_id,
                                              std::uint64_t generation);

std::shared_ptr<zlink::framework::runtime::serial_application_record_t> accepted_actor_fifo_record (
  const zlink::framework::runtime::protocol::actor_route_fence_t &source_fence,
  std::uint64_t sequence,
  const zlink::framework::runtime::messaging::envelope_header_t &envelope,
  std::string payload);
} // namespace zlink::framework::test::actor_gateway
