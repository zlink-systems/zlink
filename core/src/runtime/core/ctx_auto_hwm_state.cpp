/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "core/ctx_auto_hwm_state.hpp"

zlink::ctx_auto_hwm_state_t::ctx_auto_hwm_state_t () :
    _enabled (ZLINK_CTX_AUTO_HWM_ENABLE_DFLT != 0),
    _recalc_debounce_ms (ZLINK_CTX_AUTO_HWM_RECALC_DEBOUNCE_MS_DFLT),
    _profile (ZLINK_CTX_AUTO_HWM_PROFILE_DFLT),
    _msg_unit_bytes (ZLINK_CTX_AUTO_HWM_MSG_UNIT_BYTES_DFLT),
    _recalc_pending (false),
    _last_change_ms (0),
    _recalc_deadline_ms (0),
    _pending_generation (0),
    _last_applied_generation (0),
    _recalc_task_id (0)
{
}

void zlink::ctx_auto_hwm_state_t::set_enabled (bool enabled_)
{
    _enabled = enabled_;
}

void zlink::ctx_auto_hwm_state_t::set_recalc_debounce_ms (int debounce_ms_)
{
    _recalc_debounce_ms = debounce_ms_;
}

void zlink::ctx_auto_hwm_state_t::set_profile (zlink_auto_hwm_profile_t profile_)
{
    _profile = profile_;
}

void zlink::ctx_auto_hwm_state_t::set_msg_unit_bytes (uint64_t msg_unit_bytes_)
{
    _msg_unit_bytes = msg_unit_bytes_;
}

bool zlink::ctx_auto_hwm_state_t::enabled () const
{
    return _enabled;
}

int zlink::ctx_auto_hwm_state_t::recalc_debounce_ms () const
{
    return _recalc_debounce_ms;
}

zlink_auto_hwm_profile_t zlink::ctx_auto_hwm_state_t::profile () const
{
    return _profile;
}

uint64_t zlink::ctx_auto_hwm_state_t::msg_unit_bytes () const
{
    return _msg_unit_bytes;
}

zlink::ctx_auto_hwm_options_t zlink::ctx_auto_hwm_state_t::options () const
{
    ctx_auto_hwm_options_t snapshot;
    snapshot.enabled = _enabled;
    snapshot.profile = _profile;
    snapshot.msg_unit_bytes = _msg_unit_bytes;
    return snapshot;
}

uint64_t zlink::ctx_auto_hwm_state_t::recalc_task_id () const
{
    return _recalc_task_id;
}

void zlink::ctx_auto_hwm_state_t::set_recalc_task_id (uint64_t task_id_)
{
    _recalc_task_id = task_id_;
}

uint64_t zlink::ctx_auto_hwm_state_t::clear_recalc_task_id ()
{
    const uint64_t task_id = _recalc_task_id;
    _recalc_task_id = 0;
    return task_id;
}

void zlink::ctx_auto_hwm_state_t::schedule (uint64_t now_ms_, int debounce_ms_)
{
    _last_change_ms = now_ms_;
    ++_pending_generation;

    if (debounce_ms_ <= 0) {
        _recalc_pending = false;
        _recalc_deadline_ms = now_ms_;
    } else {
        _recalc_pending = true;
        _recalc_deadline_ms = now_ms_ + static_cast<uint64_t> (debounce_ms_);
    }
}

void zlink::ctx_auto_hwm_state_t::mark_applied ()
{
    _recalc_pending = false;
    _recalc_deadline_ms = 0;
    _last_applied_generation = _pending_generation;
}

bool zlink::ctx_auto_hwm_state_t::recalc_due (uint64_t now_ms_) const
{
    return _recalc_pending && now_ms_ >= _recalc_deadline_ms
           && _pending_generation != _last_applied_generation;
}
