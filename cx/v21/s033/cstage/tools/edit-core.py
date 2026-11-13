from pathlib import Path
import shutil
r=Path('/root/worktree/zlink-cstage');j=Path('/root/jobs/fx/cx/v21/s033/cstage')
def edit(f,fn):
 p=r/f; d=j/'original'/f;d.parent.mkdir(parents=True,exist_ok=True)
 if not d.exists():shutil.copy2(p,d)
 s=p.read_text();p.write_text(fn(s))
def new(f,s):
 p=r/f;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(s)
new('core/src/api/socket/staged_request_prototype.hpp',r'''/* Measurement-only staged request declarations. */
#ifndef ZLINK_STAGED_REQUEST_PROTOTYPE_HPP
#define ZLINK_STAGED_REQUEST_PROTOTYPE_HPP
#include <zlink.h>
#include <stddef.h>
#include <stdint.h>
struct zlink_staged_request_t { uint64_t sequence = 0; uint64_t cookie = 0; };
extern "C" {
zlink_submit_result_t zlink_proto_request_reserve (
  void *, const zlink_routing_id_t *, const uint64_t *, size_t,
  uint32_t, void *, zlink_completion_id_t *, zlink_staged_request_t *);
int zlink_proto_request_publish (
  void *, zlink_staged_request_t, zlink_msg_t *, size_t);
int zlink_proto_request_fail (void *, zlink_staged_request_t);
}
namespace zlink {
class pipe_t;
namespace staged_prototype {
struct capture_t {
 pipe_t *pipe = NULL;
 void *slots[4] = {};
 size_t count = 0;
 zlink_staged_request_t identity;
};
extern thread_local capture_t *capture;
}
}
#endif
''')
edit('core/src/runtime/core/ypipe_base.hpp',lambda s:s.replace('    virtual bool unwrite (T *value_) = 0;', '''    // Prototype handles identify slots in this queue, not another work queue.
    virtual void *stage_write (const T &, bool) { return NULL; }
    virtual T *stage_value (void *) { return NULL; }
    virtual void stage_finish (void *, bool) {}
    virtual bool unwrite (T *value_) = 0;'''))
def ypipe(s):
 s=s.replace('    //  Initialises the pipe.','''    struct entry_t {
        T value;
        entry_t *next;
        unsigned char state; // 0: ready, 1: body pending, 2: cancelled.
        bool record_end;
    };
    //  Initialises the pipe.''')
 a=s.index('        //  Place the value to the queue, add new terminator element.')
 b=s.index('\n    }',a)
 s=s[:a]+'''        (void) write_entry (value_, incomplete_, 0);'''+s[b:]
 pos=s.index('\n#ifdef ZLINK_HAVE_OPENVMS\n#pragma message restore')
 s=s[:pos]+'''
    void *stage_write (const T &value_, bool incomplete_)
    {
        return write_entry (value_, incomplete_, 1);
    }
    T *stage_value (void *slot_)
    {
        return &static_cast<entry_t *> (slot_)->value;
    }
    void stage_finish (void *slot_, bool cancel_)
    {
        static_cast<entry_t *> (slot_)->state = cancel_ ? 2 : 0;
    }
    entry_t *write_entry (const T &value_, bool incomplete_, unsigned char state_)
    {
        entry_t *const slot = &_queue.back ();
        slot->value = value_;
        slot->record_end = !incomplete_;
        slot->state = state_;
        _queue.push ();
        slot->next = &_queue.back ();
        if (!incomplete_)
            _f = &_queue.back ();
        return slot;
    }
'''+s[pos:]
 s=s.replace('*value_ = _queue.back ();','*value_ = _queue.back ().value;')
 a=s.index('        //  If there are no un-flushed items, do nothing.')
 b=s.index('\n    //  Check whether item is available',a)
 block=s[a:b]
 block=block.replace('        if (_w == _f)','''        entry_t *target = _w;
        for (entry_t *p = _w; p != _f; p = p->next) {
            if (p->state == 1)
                break;
            if (p->record_end)
                target = p->next;
        }
        if (_w == target)''').replace('_c.cas (_w, _f)','_c.cas (_w, target)').replace('_c.set (_f)','_c.set (target)').replace('_w = _f','_w = target')
 s=s[:a]+block+s[b:]
 # Drop cancelled cells only after their containing record is published.
 s=s.replace('    bool check_read ()\n    {','''    bool check_read ()
    {
        while (check_read_raw ()) {
            if (_queue.front ().state != 2)
                return true;
            _queue.pop ();
        }
        return false;
    }
    bool check_read_raw ()
    {''')
 s=s.replace('*value_ = _queue.front ();','*value_ = _queue.front ().value;')
 s=s.replace('T *const published =','entry_t *const published =')
 s=s.replace('        (*fn_) (_queue.front (), userdata_);','''        while (_queue.front ().state == 2) {
            _queue.pop ();
            if (!check_read ())
                return false;
        }
        (*fn_) (_queue.front ().value, userdata_);''')
 s=s.replace('(*fn_) (_queue.front (), userdata_)','(*fn_) (_queue.front ().value, userdata_)')
 s=s.replace('yqueue_t<T, N> _queue;','yqueue_t<entry_t, N> _queue;').replace('T *_w;','entry_t *_w;').replace('T *_r;','entry_t *_r;').replace('T *_f;','entry_t *_f;').replace('atomic_ptr_t<T> _c;','atomic_ptr_t<entry_t> _c;')
 return s
edit('core/src/runtime/core/ypipe.hpp',ypipe)
edit('core/src/api/socket/socket_request_reply_internal.hpp',lambda s:s.replace('    void release ();\n    zlink::pipe_t *pipe () const;', '    void release ();\n    void shrink (uint64_t actual_bytes_);\n    zlink::pipe_t *pipe () const;').replace('    zlink::socket_completion::reservation_t *pull_completion;','''    zlink::socket_completion::reservation_t *pull_completion;
    void *staged_slots[4] = {};
    size_t staged_part_count = 0;'''))
edit('core/src/api/socket/socket_request_reply_internal.cpp',lambda s:s.replace('void request_correlation_lease_t::release ()','''void request_correlation_lease_t::shrink (uint64_t actual_bytes_)
{
    zlink_assert (_pipe && actual_bytes_ > 0 && actual_bytes_ <= _accounted_bytes);
    if (actual_bytes_ != _accounted_bytes)
        _pipe->release_request_correlation (_accounted_bytes, actual_bytes_);
    _accounted_bytes = actual_bytes_;
}

void request_correlation_lease_t::release ()'''))
edit('core/src/runtime/core/pipe.hpp',lambda s:s.replace('void release_request_correlation (uint64_t accounted_bytes_);','''void release_request_correlation (uint64_t accounted_bytes_, uint64_t retained_bytes_ = 0);
    uint64_t publish_staged_record (void **slots_, size_t count_, msg_t *parts_);'''))
edit('core/src/runtime/core/pipe_receive.cpp',lambda s:s.replace('void zlink::pipe_t::release_request_correlation (uint64_t accounted_bytes_)','void zlink::pipe_t::release_request_correlation (uint64_t accounted_bytes_, uint64_t retained_bytes_)').replace('_request_correlation_bytes -= accounted_bytes_;','_request_correlation_bytes -= accounted_bytes_ - retained_bytes_;').replace('_request_correlation_work -= work_charge;','''_request_correlation_work -= work_charge - (retained_bytes_
          ? transport_pair_policy::request_correlation_work_charge (retained_bytes_) : 0);''').replace('--_request_correlation_count;','if (!retained_bytes_)\n            --_request_correlation_count;'))
def pw(s):
 s=s.replace('#include "core/ypipe.hpp"','#include "core/ypipe.hpp"\n#include "api/socket/staged_request_prototype.hpp"')
 s=s.replace('        _out_pipe->write (msg_, more_);','''        staged_prototype::capture_t *const staged = staged_prototype::capture;
        if (staged && staged->pipe == this) {
            zlink_assert (staged->count < 4);
            staged->slots[staged->count++] = _out_pipe->stage_write (msg_, more_);
            zlink_assert (staged->slots[staged->count - 1]);
        } else
            _out_pipe->write (msg_, more_);''')
 s+='''
// Called only in the socket's existing complete-send lifecycle turn.
uint64_t zlink::pipe_t::publish_staged_record (
  void **slots_, size_t count_, msg_t *parts_)
{
    if (!_out_pipe || _state != active) {
        errno = EHOSTUNREACH;
        return 0;
    }
    uint64_t reserved = 0, actual = 0;
    for (size_t i = 0; i != count_; ++i) {
        msg_t *old = _out_pipe->stage_value (slots_[i]);
        zlink_assert (old);
        reserved += frame_accounted_bytes (old);
        if (parts_) {
            if (parts_[i].size () > old->size ()) {
                errno = EMSGSIZE;
                return 0;
            }
            actual += frame_accounted_bytes (&parts_[i]);
        }
    }
    for (size_t i = 0; i != count_; ++i) {
        msg_t *old = _out_pipe->stage_value (slots_[i]);
        const unsigned char more = old->flags () & msg_t::more;
        if (parts_) {
            parts_[i].reset_flags (msg_t::more);
            parts_[i].set_flags (more);
            old->move (parts_[i]);
        } else
            old->close ();
        _out_pipe->stage_finish (slots_[i], !parts_);
    }
    const uint64_t difference = reserved - actual;
    if (difference) {
        if (_registry_accounting)
            get_ctx ()->_physical_queue_registry.release_committed_frame (
              _out_physical_queue, difference, parts_ ? 0 : 1);
        publish_outbound_ledger_unlocked (
          _msgs_written.load () - (parts_ ? 0 : 1),
          _bytes_written.load () - difference);
        publish_outbound_accounting_unlocked (false);
        send_activate_write_deferred (this, _out_generation,
          _peers_msgs_read.load (), _peers_bytes_read.load ());
    }
    _out_complete_record_pending = true;
    flush_unlocked ();
    return parts_ ? actual : reserved;
}
'''
 return s
edit('core/src/runtime/core/pipe_write.cpp',pw)
def api(s):
 s=s.replace('#include "core/msg.hpp"','#include "api/socket/staged_request_prototype.hpp"\n#include "core/msg.hpp"')
 s=s.replace('namespace reqrep =', '''namespace zlink { namespace staged_prototype {
thread_local capture_t *capture = NULL;
} }

namespace reqrep =''')
 s=s.replace('        observer->reserved_pipe = pipe_;\n        return true;', '''        observer->reserved_pipe = pipe_;
        if (zlink::staged_prototype::capture) {
            zlink::staged_prototype::capture->pipe = pipe_;
            zlink::staged_prototype::capture->count = 0;
        }
        return true;''')
 s=s.replace('            observer->reservation_committed = true;', '''            observer->reservation_committed = true;
            if (zlink::staged_prototype::capture) {
                zlink::staged_prototype::capture_t &stage = *zlink::staged_prototype::capture;
                observer->pending->staged_part_count = stage.count;
                for (size_t i = 0; i != stage.count; ++i)
                    observer->pending->staged_slots[i] = stage.slots[i];
                stage.identity.sequence = observer->identity.request_seq;
                stage.identity.cookie = observer->identity.cookie;
            }''')
 s=s.replace('    observer->pending = NULL;\n    observer->reserved_pipe = NULL;', '''    if (zlink::staged_prototype::capture)
        zlink::staged_prototype::capture->pipe = NULL;
    observer->pending = NULL;
    observer->reserved_pipe = NULL;''')
 s=s.replace('    (void) reqrep::arm_socket_pending_request_timeout (state_, token_);','''    if (!zlink::staged_prototype::capture)
        (void) reqrep::arm_socket_pending_request_timeout (state_, token_);''')
 s+='''
extern "C" zlink_submit_result_t zlink_proto_request_reserve (
  void *socket_, const zlink_routing_id_t *target_, const uint64_t *bounds_,
  size_t count_, uint32_t timeout_, void *context_, zlink_completion_id_t *id_,
  zlink_staged_request_t *stage_)
{
    if (!bounds_ || !stage_ || count_ == 0 || count_ > 4) {
        errno = EINVAL;
        return ZLINK_SUBMIT_INVALID_ARGUMENT;
    }
    zlink_msg_t parts[4];
    static char placeholder;
    for (size_t i = 0; i != count_; ++i)
        reinterpret_cast<zlink::msg_t *> (&parts[i])->init_data (
          &placeholder, bounds_[i], NULL, NULL);
    zlink::staged_prototype::capture_t capture;
    zlink_assert (!zlink::staged_prototype::capture);
    zlink::staged_prototype::capture = &capture;
    const zlink_submit_result_t result = zlink_request (
      socket_, target_, parts, count_, ZLINK_SEND_FLAGS_DONTWAIT,
      timeout_, context_, id_);
    zlink::staged_prototype::capture = NULL;
    *stage_ = capture.identity;
    for (size_t i = 0; i != count_; ++i)
        (void) zlink_msg_close (&parts[i]);
    return result;
}

namespace {
int finish_staged_request (void *socket_, zlink_staged_request_t stage_,
                           zlink_msg_t *parts_, size_t count_)
{
    socket_handle_t handle = as_socket_handle (socket_);
    if (!handle.socket)
        return -1;
    std::optional<zlink::socket_public_send_scope_t> scope;
    if (!handle.socket->begin_complete_send_scope (&scope))
        return -1;
    auto state = reqrep::find_request_reply_state (handle);
    if (!state) { errno = ENOENT; return -1; }
    reqrep::pending_request_token_t token;
    token.identity.request_seq = stage_.sequence;
    token.identity.cookie = stage_.cookie;
    reqrep::pending_request_t failed;
    {
        std::lock_guard<std::mutex> lock (state->mutex);
        auto it = state->pending_requests.find (stage_.sequence);
        if (it == state->pending_requests.end ()
            || !(it->second.identity == token.identity)) {
            errno = ECANCELED; return -1;
        }
        auto &pending = it->second;
        if (!pending.staged_part_count || (parts_ && count_ != pending.staged_part_count)) {
            errno = EINVAL; return -1;
        }
        if (parts_ && attach_request_reply_metadata (
              &parts_[0], zlink::request_reply::request_type, stage_.sequence) != 0)
            return -1;
        const uint64_t actual = pending.correlation.pipe ()->publish_staged_record (
          pending.staged_slots, pending.staged_part_count,
          reinterpret_cast<zlink::msg_t *> (parts_));
        if (!actual) return -1;
        pending.staged_part_count = 0;
        if (parts_) {
            pending.correlation.shrink (actual);
            token.resolved_timeout_ms = pending.resolved_timeout_ms;
        } else {
            (void) reqrep::remove_socket_pending_request_locked (
              state.get (), token.identity, &failed);
        }
    }
    scope.reset ();
    if (parts_)
        return reqrep::arm_socket_pending_request_timeout (state, token);
    failed.correlation.release ();
    return reqrep::publish_pending_request_completion (
      state, &failed, ZLINK_REQUEST_INTERNAL_ERROR, NULL, 0);
}
}
extern "C" int zlink_proto_request_publish (
  void *socket_, zlink_staged_request_t stage_, zlink_msg_t *parts_, size_t count_)
{
    return finish_staged_request (socket_, stage_, parts_, count_);
}
extern "C" int zlink_proto_request_fail (void *socket_, zlink_staged_request_t stage_)
{
    return finish_staged_request (socket_, stage_, NULL, 0);
}
'''
 return s
edit('core/src/api/socket/socket_request_reply_submit_api.cpp',api)
edit('core/src/libzlink.vers',lambda s:s.replace('  global:','  global:\n    zlink_proto_request_reserve;\n    zlink_proto_request_publish;\n    zlink_proto_request_fail;'))
print('Core prototype edited')
