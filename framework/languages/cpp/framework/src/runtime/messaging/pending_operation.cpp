/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/messaging/pending_operation_state.hpp"

#include <utility>

namespace zlink::framework
{

pending_operation_t::pending_operation_t () noexcept = default;

pending_operation_t::pending_operation_t (
  std::shared_ptr<detail::pending_operation_state_t> state) noexcept :
    _state (std::move (state))
{
}

pending_operation_t pending_operation_t::make_completed ()
{
    auto operation = detail::make_pending_operation ();
    if (operation._state) {
        operation._state->try_complete ();
    }
    return operation;
}

bool pending_operation_t::valid () const noexcept
{
    return _state != nullptr;
}

bool pending_operation_t::completed () const noexcept
{
    return _state != nullptr && _state->completed.load ();
}

bool pending_operation_t::cancelled () const noexcept
{
    return _state != nullptr && _state->cancelled.load ();
}

bool pending_operation_t::cancel () noexcept
{
    return _state != nullptr && _state->try_cancel ();
}

namespace detail
{

pending_operation_t make_pending_operation ()
{
    return pending_operation_t (std::make_shared<pending_operation_state_t> ());
}

bool complete_pending_operation (pending_operation_t &operation) noexcept
{
    return operation._state != nullptr && operation._state->try_complete ();
}

bool fail_pending_operation (pending_operation_t &operation, std::exception_ptr error) noexcept
{
    return operation._state != nullptr && operation._state->try_fail (std::move (error));
}

bool same_pending_operation (const pending_operation_t &left,
                             const pending_operation_t &right) noexcept
{
    return left._state == right._state;
}

} // namespace detail

} // namespace zlink::framework
