/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/actors/actor.hpp>
#include <zlink/framework/contracts/locations/stores.hpp>

#include <functional>

namespace zlink::framework::detail
{

class actor_manager_access_t
{
  public:
    using create_fn_t = std::function<task_t<actor_create_result_t> (
      bool,
      actor_id_t,
      std::string,
      std::optional<std::string>,
      std::optional<message_t>,
      std::chrono::milliseconds,
      creation_operation_id_t)>;
    using find_fn_t =
      std::function<task_t<std::optional<actor_ref_t>> (actor_id_t)>;
    using find_spot_fn_t =
      std::function<task_t<std::optional<spot_ref_t>> (actor_id_t)>;
    using destroy_fn_t = std::function<task_t<bool> (actor_ref_t)>;

    static actor_manager_t create (create_fn_t create_actor,
                                   find_fn_t find_actor,
                                   find_spot_fn_t find_spot,
                                   destroy_fn_t destroy_actor);
};

} // namespace zlink::framework::detail
