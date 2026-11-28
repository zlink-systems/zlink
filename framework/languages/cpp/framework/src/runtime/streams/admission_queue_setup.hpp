/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "runtime/streams/admission_ticket.hpp"
#include <zlink/framework/contracts/streams/stream.hpp>

#include <memory>

namespace zlink::framework::detail
{
struct mesh_node_builder_state_t;
}

namespace zlink::framework::runtime
{
class live_location_reader_t;

namespace admission_queue
{

struct settings_t
{
    std::string mesh_name;
    std::shared_ptr<const ticket_signer_t> tickets;
    std::optional<std::int32_t> global_limit;
};

std::optional<settings_t>
configure (const std::vector<stream_snapshot_t> &streams,
           const std::vector<std::shared_ptr<detail::mesh_node_builder_state_t>> &meshes,
           live_location_reader_t *locations);

} // namespace admission_queue
} // namespace zlink::framework::runtime
