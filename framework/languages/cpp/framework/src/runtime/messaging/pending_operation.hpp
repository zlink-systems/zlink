/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <exception>
#include <memory>

namespace zlink::framework
{

class pending_operation_t;

namespace detail
{
struct pending_operation_state_t;
pending_operation_t make_pending_operation ();
bool complete_pending_operation (pending_operation_t &operation) noexcept;
bool fail_pending_operation (pending_operation_t &operation, std::exception_ptr error) noexcept;
bool same_pending_operation (const pending_operation_t &left,
                             const pending_operation_t &right) noexcept;
} // namespace detail

class pending_operation_t
{
  public:
    pending_operation_t () noexcept;

    static pending_operation_t make_completed ();

    bool valid () const noexcept;
    bool completed () const noexcept;
    bool cancelled () const noexcept;
    bool cancel () noexcept;

  private:
    explicit pending_operation_t (
      std::shared_ptr<detail::pending_operation_state_t> state) noexcept;

    std::shared_ptr<detail::pending_operation_state_t> _state;

    friend pending_operation_t detail::make_pending_operation ();
    friend bool detail::complete_pending_operation (pending_operation_t &operation) noexcept;
    friend bool detail::fail_pending_operation (pending_operation_t &operation,
                                                std::exception_ptr error) noexcept;
    friend bool detail::same_pending_operation (const pending_operation_t &left,
                                                const pending_operation_t &right) noexcept;
};

} // namespace zlink::framework
