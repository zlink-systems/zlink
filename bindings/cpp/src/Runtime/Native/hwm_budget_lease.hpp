/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_NATIVE_HWM_BUDGET_LEASE_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_NATIVE_HWM_BUDGET_LEASE_HPP_INCLUDED

#include <zlink/socket/api.h>

#include <memory>
#include <utility>
#include <vector>

namespace zlink
{
namespace detail
{

// One received binding envelope may contain several physical frames. Keep the
// corresponding Core leases behind one shared lifetime so copying received_t
// or topic_message_t cannot publish credit while another envelope copy is live.
class hwm_budget_lease_set_t
{
  public:
    hwm_budget_lease_set_t () = default;
    hwm_budget_lease_set_t (const hwm_budget_lease_set_t &) = delete;
    hwm_budget_lease_set_t &operator= (const hwm_budget_lease_set_t &) = delete;

    ~hwm_budget_lease_set_t () { release_all (); }

    void adopt (zlink_hwm_budget_lease_t *lease_)
    {
        if (!lease_)
            return;
        try {
            _leases.push_back (lease_);
        }
        catch (...) {
            zlink_hwm_budget_lease_release (&lease_);
            throw;
        }
    }

  private:
    void release_all () noexcept
    {
        for (auto &lease : _leases)
            zlink_hwm_budget_lease_release (&lease);
        _leases.clear ();
    }

    std::vector<zlink_hwm_budget_lease_t *> _leases;
};

inline void adopt_hwm_budget_lease (
  std::shared_ptr<hwm_budget_lease_set_t> &owner_,
  zlink_hwm_budget_lease_t *lease_)
{
    if (!lease_)
        return;
    if (!owner_) {
        try {
            owner_ = std::make_shared<hwm_budget_lease_set_t> ();
        }
        catch (...) {
            zlink_hwm_budget_lease_release (&lease_);
            throw;
        }
    }
    owner_->adopt (lease_);
}

inline void release_hwm_budget_lease (zlink_hwm_budget_lease_t *lease_) noexcept
{
    if (lease_)
        zlink_hwm_budget_lease_release (&lease_);
}

} // namespace detail
} // namespace zlink

#endif
