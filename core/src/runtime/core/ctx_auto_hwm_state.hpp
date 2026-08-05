/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_CTX_AUTO_HWM_STATE_HPP_INCLUDED__
#define __ZLINK_CTX_AUTO_HWM_STATE_HPP_INCLUDED__

#include "utils/stdint.hpp"
#include "zlink.h"

namespace zlink
{
struct ctx_auto_hwm_options_t
{
    bool enabled;
    zlink_auto_hwm_profile_t profile;
    uint64_t msg_unit_bytes;
};

class ctx_auto_hwm_state_t
{
  public:
    ctx_auto_hwm_state_t ();

    void set_enabled (bool enabled_);
    void set_recalc_debounce_ms (int debounce_ms_);
    void set_profile (zlink_auto_hwm_profile_t profile_);
    void set_msg_unit_bytes (uint64_t msg_unit_bytes_);

    bool enabled () const;
    int recalc_debounce_ms () const;
    zlink_auto_hwm_profile_t profile () const;
    uint64_t msg_unit_bytes () const;
    ctx_auto_hwm_options_t options () const;

    uint64_t recalc_task_id () const;
    void set_recalc_task_id (uint64_t task_id_);
    uint64_t clear_recalc_task_id ();

    void schedule (uint64_t now_ms_, int debounce_ms_);
    void mark_applied ();
    bool recalc_due (uint64_t now_ms_) const;

  private:
    bool _enabled;
    int _recalc_debounce_ms;
    zlink_auto_hwm_profile_t _profile;
    uint64_t _msg_unit_bytes;
    bool _recalc_pending;
    uint64_t _last_change_ms;
    uint64_t _recalc_deadline_ms;
    uint64_t _pending_generation;
    uint64_t _last_applied_generation;
    uint64_t _recalc_task_id;
};
}

#endif
