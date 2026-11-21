/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include "m6a_mesh_fixture.hpp"
#include "runtime/protocol/service_wire_codec.hpp"

namespace zlink::framework::test::m6a
{
namespace protocol = runtime::protocol;

protocol::relocation_prepare_t
relocation_prepare_request (const mesh::service_node_descriptor_t &source_descriptor,
                            const mesh::service_node_descriptor_t &target_descriptor);
}
