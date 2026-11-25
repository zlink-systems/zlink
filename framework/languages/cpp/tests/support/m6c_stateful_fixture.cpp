/* SPDX-License-Identifier: Elastic-2.0 */

#include "m6c_stateful_fixture.hpp"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace m6c_test
{

object_ref_t create_actor (stateful_object_runtime_t &runtime, std::string key, std::string node)
{
    runtime.replace_placement_candidates ({{.mesh_name = "mesh",
                                            .node_id = std::move (node),
                                            .stable_types = {"actor"},
                                            .weight = 100,
                                            .active_capacity = 100,
                                            .active_count = 0,
                                            .pending_capacity = 100,
                                            .pending_count = 0}});
    auto created = runtime.begin_create ({.kind = object_kind_t::actor,
                                          .key = std::move (key),
                                          .stable_type = "actor",
                                          .mesh_name = std::optional<std::string>{"mesh"},
                                          .creation_request = {},
                                          .exclusive = true,
                                          .instance_intent = false});
    if (created.status != create_status_t::reserved
        || runtime.commit_create (created.attempt) != stateful_error_t::none)
        throw std::runtime_error ("actor creation failed");
    return created.object;
}

object_ref_t create_spot (stateful_object_runtime_t &runtime, object_kind_t kind, std::string key)
{
    runtime.replace_placement_candidates ({{.mesh_name = "mesh",
                                            .node_id = "node-a",
                                            .stable_types = {"spot", "actor"},
                                            .weight = 100,
                                            .active_capacity = 100,
                                            .active_count = 0,
                                            .pending_capacity = 100,
                                            .pending_count = 0}});
    auto created = runtime.begin_create ({.kind = kind,
                                          .key = std::move (key),
                                          .stable_type = "spot",
                                          .mesh_name = std::optional<std::string>{"mesh"},
                                          .creation_request = {},
                                          .exclusive = true,
                                          .instance_intent = kind == object_kind_t::instance_spot});
    if (created.status != create_status_t::reserved
        || runtime.commit_create (created.attempt) != stateful_error_t::none)
        throw std::runtime_error ("spot creation failed");
    return created.object;
}

inventory_digest_t digest_with (std::uint8_t value)
{
    inventory_digest_t digest{};
    digest.fill (value);
    return digest;
}

} // namespace m6c_test
