/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <string>
#include <utility>

namespace zlink::framework::runtime
{

struct actor_execution_context_t
{
    std::string actor_key;
    std::string spot_id;
};

inline thread_local actor_execution_context_t current_actor_execution;

class actor_execution_scope_t
{
  public:
    actor_execution_scope_t (std::string actor_key, std::string spot_id) :
        _previous (std::move (current_actor_execution))
    {
        current_actor_execution = {
          std::move (actor_key), std::move (spot_id)};
    }

    ~actor_execution_scope_t ()
    {
        current_actor_execution = std::move (_previous);
    }

    actor_execution_scope_t (const actor_execution_scope_t &) = delete;
    actor_execution_scope_t &operator= (const actor_execution_scope_t &) = delete;

  private:
    actor_execution_context_t _previous;
};

} // namespace zlink::framework::runtime
