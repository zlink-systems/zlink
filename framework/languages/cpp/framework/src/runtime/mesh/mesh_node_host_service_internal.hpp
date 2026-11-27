/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "runtime/locations/location_repository_values.hpp"
#include "runtime/stateful/public_host_runtime.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zlink::framework
{
class location_repository_t;
struct mesh_node_descriptor_t;
} // namespace zlink::framework

namespace zlink::framework::detail
{
struct mesh_node_builder_state_t;
class mesh_node_runtime_t;
} // namespace zlink::framework::detail

namespace zlink::framework::runtime::protocol::generated::detail
{
enum class actor_create_result_t : std::uint8_t;
enum class framework_error_code_t : std::uint32_t;
enum class request_terminal_result_t : std::uint32_t;
} // namespace zlink::framework::runtime::protocol::generated::detail

namespace zlink::framework::runtime
{
class live_location_reader_t;

namespace mesh_node_host_service_detail
{
// One process-wide Mesh operation id sequence shared by the host TUs.
inline std::atomic_uint64_t next_mesh_operation{1};

mesh_node_descriptor_t
choose_placement_target (const std::vector<mesh_node_descriptor_t> &candidates,
                         std::string_view identity);

task_t<std::vector<mesh_node_descriptor_t>>
placement_candidates (live_location_reader_t &discovery,
                      std::string mesh_name,
                      placement_object_kind_t kind,
                      std::string stable_type,
                      std::shared_ptr<detail::mesh_node_runtime_t> source,
                      std::chrono::steady_clock::time_point deadline,
                      std::function<bool (const mesh_node_descriptor_t &)> excluded);

location_write_intent_t mesh_new_claim_intent (
  const std::vector<std::shared_ptr<detail::mesh_node_builder_state_t>> &registrations,
  const mesh_node_descriptor_t &descriptor);

std::vector<std::byte> encode_actor_creation_intent (const std::string &content_type,
                                                     std::vector<std::uint8_t> body);

std::optional<std::vector<std::byte>>
read_actor_creation_request (const std::shared_ptr<location_repository_t> &store,
                             const protocol::actor_create_header_t &request);

std::vector<std::byte> actor_terminal_envelope (
  protocol::generated::detail::request_terminal_result_t terminal_result,
  protocol::generated::detail::framework_error_code_t failure_code,
  std::optional<protocol::generated::detail::actor_create_result_t> creation_result,
  const std::optional<actor_ref_t> &actor,
  const std::optional<protocol::application_payload_t> &reply);

task_t<object_complete_creation_result_t>
complete_actor_creation (std::shared_ptr<host::public_host_runtime_t> node,
                         std::shared_ptr<location_repository_t> store,
                         object_complete_creation_request_t request,
                         std::string stable_type);

std::vector<std::byte> target_actor_authority_payload (actor_authority_state_t state,
                                                       const std::string &stable_type,
                                                       const actor_id_t &actor_id,
                                                       const mesh_node_descriptor_t &target);

void execute_actor_create_operation_target (
  std::shared_ptr<detail::mesh_node_runtime_t> node,
  std::shared_ptr<location_repository_t> store,
  std::shared_ptr<detail::mesh_node_builder_state_t> registration,
  const protocol::actor_create_header_t &request,
  host::actor_create_operation_target_completion_t completion);

class terminal_callback_guard_t final
{
  public:
    explicit terminal_callback_guard_t (host::mailbox_callback_t release) :
        _release (std::move (release))
    {
    }

    ~terminal_callback_guard_t () noexcept
    {
        if (_release) {
            try {
                _release ();
            }
            catch (...) {
            }
        }
    }

    terminal_callback_guard_t (const terminal_callback_guard_t &) = delete;
    terminal_callback_guard_t &operator= (const terminal_callback_guard_t &) = delete;

    void dismiss () noexcept { _release = {}; }

  private:
    host::mailbox_callback_t _release;
};

void trace_mesh_application (std::string_view stage,
                             const host::receive_record_t &record,
                             std::size_t parts,
                             std::string_view detail = {});

void reject_application_request (const host::receive_record_t &record,
                                 std::vector<zlink::message_t> parts,
                                 framework_error_kind_t error_kind,
                                 std::string message);
} // namespace mesh_node_host_service_detail
} // namespace zlink::framework::runtime
