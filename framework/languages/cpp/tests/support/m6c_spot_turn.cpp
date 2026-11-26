/* SPDX-License-Identifier: Elastic-2.0 */

#include "m6c_spot_turn.hpp"
#include "runtime/spots/spot_runtime.hpp"

#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <utility>

namespace m6c_test
{

bool run_serial_turn (const std::shared_ptr<zlink::framework::detail::spot_context_state_t> &state,
                      std::string name,
                      std::function<void ()> work)
{
    struct turn_state_t
    {
        std::promise<void> entered;
        std::exception_ptr error;
    };
    auto turn_state = std::make_shared<turn_state_t> ();
    auto terminal = turn_state->entered.get_future ();
    const auto posted =
      state->serial_queue->try_post (std::move (name), [turn_state, work = std::move (work)] {
          try {
              work ();
          }
          catch (...) {
              turn_state->error = std::current_exception ();
          }
          turn_state->entered.set_value ();
      });
    if (!posted || terminal.wait_for (std::chrono::seconds (1)) != std::future_status::ready)
        return false;
    if (turn_state->error)
        std::rethrow_exception (turn_state->error);
    return true;
}

} // namespace m6c_test
