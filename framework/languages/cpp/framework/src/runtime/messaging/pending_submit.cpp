/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/messaging/pending_submit.hpp"

#include <stdexcept>

namespace zlink::framework::runtime::messaging
{

pending_submit_t pending_submit_t::create_command (try_submit_fn try_submit,
                                                   std::optional<clock_t::time_point> deadline,
                                                   wake_fn wake)
{
    return pending_submit_t (std::move (try_submit), deadline, std::move (wake), true);
}

pending_submit_t pending_submit_t::create_request (try_submit_fn try_submit,
                                                   std::optional<clock_t::time_point> deadline,
                                                   wake_fn wake)
{
    return pending_submit_t (std::move (try_submit), deadline, std::move (wake), false);
}

pending_submit_t::pending_submit_t (try_submit_fn try_submit,
                                    std::optional<clock_t::time_point> deadline,
                                    wake_fn wake,
                                    bool complete_on_accepted) :
    _operation (detail::make_pending_operation ()),
    _try_submit (std::move (try_submit)),
    _deadline (deadline),
    _wake (std::move (wake)),
    _complete_on_accepted (complete_on_accepted)
{
    if (!_try_submit) {
        throw std::invalid_argument ("pending submit requires a submit function");
    }
}

bool pending_submit_t::expired (clock_t::time_point now) const noexcept
{
    return _deadline.has_value () && now >= *_deadline;
}

bool pending_submit_t::try_submit ()
{
    if (completed ()) {
        return false;
    }
    if (expired ()) {
        fail (std::make_exception_ptr (
          std::runtime_error ("ZLink submit timed out before acceptance.")));
        return false;
    }

    bool accepted = false;
    try {
        accepted = _try_submit ();
    }
    catch (...) {
        fail (std::current_exception ());
        throw;
    }
    if (accepted && _complete_on_accepted) {
        complete ();
    }
    return accepted;
}

void pending_submit_t::complete ()
{
    if (detail::complete_pending_operation (_operation) && _wake) {
        _wake ();
    }
}

void pending_submit_t::cancel ()
{
    if (_operation.cancel () && _wake) {
        _wake ();
    }
}

void pending_submit_t::fail (std::exception_ptr error)
{
    if (detail::fail_pending_operation (_operation, std::move (error)) && _wake) {
        _wake ();
    }
}

} // namespace zlink::framework::runtime::messaging
