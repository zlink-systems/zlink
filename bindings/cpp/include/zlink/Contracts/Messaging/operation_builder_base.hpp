/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <memory>
#include <utility>

namespace zlink
{
namespace detail
{

struct operation_state_t;

struct pooled_operation_state_policy_t
{
    static void destroy (std::unique_ptr<operation_state_t> state_ptr_) noexcept;
};

struct unique_operation_state_policy_t
{
    template <typename StateT>
    static void destroy (std::unique_ptr<StateT>) noexcept
    {
    }
};

template <typename StateT, typename DestroyPolicyT> class operation_builder_base_t
{
  protected:
    operation_builder_base_t () = default;

    explicit operation_builder_base_t (StateT &&state_) :
        _state (std::make_unique<StateT> (std::move (state_)))
    {
    }

    explicit operation_builder_base_t (std::unique_ptr<StateT> state_ptr_) noexcept :
        _state (std::move (state_ptr_))
    {
    }

    ~operation_builder_base_t () noexcept { DestroyPolicyT::destroy (std::move (_state)); }

    operation_builder_base_t (operation_builder_base_t &&) noexcept = default;

    operation_builder_base_t &operator= (operation_builder_base_t &&other_) noexcept
    {
        if (this != &other_) {
            DestroyPolicyT::destroy (std::move (_state));
            _state = std::move (other_._state);
        }
        return *this;
    }

    operation_builder_base_t (const operation_builder_base_t &) = delete;
    operation_builder_base_t &operator= (const operation_builder_base_t &) = delete;

    StateT &state () noexcept { return (*_state); }

    const StateT &state () const noexcept { return (*_state); }

    std::unique_ptr<StateT> release_state_ptr () noexcept { return std::move (_state); }

  private:
    std::unique_ptr<StateT> _state;
};

} // namespace detail
} // namespace zlink
