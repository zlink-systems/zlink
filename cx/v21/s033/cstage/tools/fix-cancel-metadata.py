from pathlib import Path
import shutil
r=Path('/root/worktree/zlink-cstage');j=Path('/root/jobs/fx/cx/v21/s033/cstage');q=j/'prototype-source'
def edit(f,fn):
 p=r/f;orig=j/'original'/f
 if not orig.exists():orig.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,orig)
 s=p.read_text();p.write_text(fn(s));d=q/f;d.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,d)
def base(s):
 s=s.replace('    virtual ~ypipe_base_t () ZLINK_DEFAULT;', '    typedef void (*cancelled_fn) (const T &, bool, void *);\n    virtual ~ypipe_base_t () ZLINK_DEFAULT;')
 s=s.replace('virtual bool check_read () = 0;', 'virtual bool check_read (cancelled_fn = NULL, void * = NULL) = 0;')
 s=s.replace('bool *prefetched_batch_exhausted_ = NULL) = 0;', 'bool *prefetched_batch_exhausted_ = NULL,\n                       cancelled_fn = NULL, void * = NULL) = 0;')
 s=s.replace('void *userdata_) = 0;', 'void *userdata_, cancelled_fn = NULL, void * = NULL) = 0;')
 return s
edit('core/src/runtime/core/ypipe_base.hpp',base)
def queue(s):
 s=s.replace('  public:\n    struct entry_t', '  public:\n    typedef typename ypipe_base_t<T>::cancelled_fn cancelled_fn;\n    struct entry_t',1)
 s=s.replace('    bool check_read ()\n', '    bool check_read (cancelled_fn cancelled_ = NULL, void *cancel_context_ = NULL)\n')
 s=s.replace('''            if (_queue.front ().state != 2)
                return true;
            _queue.pop ();''','''            if (_queue.front ().state != 2 || !cancelled_)
                return true;
            T value;
            value = _queue.front ().value;
            _queue.pop ();
            cancelled_ (value, &_queue.front () == _r, cancel_context_);''',1)
 s=s.replace('bool *prefetched_batch_exhausted_ = NULL)', 'bool *prefetched_batch_exhausted_ = NULL,\n               cancelled_fn cancelled_ = NULL, void *cancel_context_ = NULL)')
 s=s.replace('if (!check_read ())', 'if (!check_read (cancelled_, cancel_context_))')
 s=s.replace('''bool probe_if_published (void (*fn_) (const T &, void *),
                             void *userdata_)''','''bool probe_if_published (void (*fn_) (const T &, void *),
                             void *userdata_, cancelled_fn cancelled_ = NULL,
                             void *cancel_context_ = NULL)''')
 s=s.replace('''        while (_queue.front ().state == 2) {
            _queue.pop ();''','''        while (_queue.front ().state == 2 && cancelled_) {
            T value;
            value = _queue.front ().value;
            _queue.pop ();
            cancelled_ (value, &_queue.front () == _r, cancel_context_);''')
 return s
edit('core/src/runtime/core/ypipe.hpp',queue)
def conflate(s):
 s=s.replace('  public:\n    ypipe_conflate_t', '  public:\n    typedef typename ypipe_base_t<T>::cancelled_fn cancelled_fn;\n    ypipe_conflate_t',1)
 s=s.replace('bool check_read ()','bool check_read (cancelled_fn = NULL, void * = NULL)')
 s=s.replace('bool *prefetched_batch_exhausted_ = NULL)', 'bool *prefetched_batch_exhausted_ = NULL, cancelled_fn = NULL, void * = NULL)')
 s=s.replace('void *userdata_)\n    {\n        //  dbuffer inspects', 'void *userdata_, cancelled_fn = NULL, void * = NULL)\n    {\n        //  dbuffer inspects')
 return s
edit('core/src/runtime/core/ypipe_conflate.hpp',conflate)
edit('core/src/runtime/core/pipe.hpp',lambda s:s.replace('    void account_inbound_frame (','    static void account_cancelled_frame (const msg_t &, bool, void *);\n    void account_inbound_frame ('))
def receive(s):
 # Registry release has one owner, also used by cancelled metadata consumption.
 blocks=['''                if (_registry_accounting) {
                    const uint64_t frame_bytes = frame_accounted_bytes (msg_);
                    const uint64_t counted_messages =
                      counted_pending_message_ref (*msg_);
                    get_ctx ()->_physical_queue_registry.release_committed_frame (
                      _in_physical_queue, frame_bytes, counted_messages);
                }
''','''            if (_registry_accounting)
                get_ctx ()->_physical_queue_registry.release_committed_frame (
                  _in_physical_queue, frame_accounted_bytes (msg_),
                  counted_pending_message_ref (*msg_));
''','''    if (_registry_accounting) {
        const uint64_t frame_bytes = frame_accounted_bytes (msg_);
        const uint64_t counted_messages = counted_pending_message_ref (*msg_);
        get_ctx ()->_physical_queue_registry.release_committed_frame (
          _in_physical_queue, frame_bytes, counted_messages);
    }
''']
 for b in blocks:
  assert b in s,b;s=s.replace(b,'')
 needle='''void zlink::pipe_t::account_inbound_frame (
  const msg_t *msg_, bool prefetched_batch_exhausted_)
{'''
 assert needle in s
 s=s.replace(needle,'''void zlink::pipe_t::account_cancelled_frame (const msg_t &msg_, bool exhausted_, void *context_)
{
    static_cast<pipe_t *> (context_)->account_inbound_frame (&msg_, exhausted_);
}

'''+needle+'''
    if (_registry_accounting)
        get_ctx ()->_physical_queue_registry.release_committed_frame (
          _in_physical_queue, frame_accounted_bytes (msg_),
          counted_pending_message_ref (*msg_));''')
 s=s.replace('_in_pipe->check_read ()', '_in_pipe->check_read (&account_cancelled_frame, this)')
 s=s.replace('_in_pipe->read_if (&delimiter, &consume_if_delimiter, NULL)', '_in_pipe->read_if (&delimiter, &consume_if_delimiter, NULL, NULL, &account_cancelled_frame, this)')
 s=s.replace('_in_pipe->probe_if_published (&probe_normalized_head, &probe)', '_in_pipe->probe_if_published (&probe_normalized_head, &probe, &account_cancelled_frame, this)')
 s=s.replace('&prefetched_batch_exhausted);', '&prefetched_batch_exhausted, &account_cancelled_frame, this);')
 s=s.replace('_in_pipe->read (msg_, &prefetched_batch_exhausted)', '_in_pipe->read (msg_, &prefetched_batch_exhausted, &account_cancelled_frame, this)')
 return s
edit('core/src/runtime/core/pipe_receive.cpp',receive)
edit('core/src/runtime/core/pipe_transport.cpp',lambda s:s.replace('_in_pipe->read_if (&delimiter, &consume_if_delimiter, NULL)', '_in_pipe->read_if (&delimiter, &consume_if_delimiter, NULL, NULL, &account_cancelled_frame, this)'))
def write(s):
 # A failed record returns its payload charge immediately. Its resident fixed
 # frame charge stays in the existing HWM ledger until the existing reader skips it.
 s=s.replace('''    const uint64_t difference = reserved - actual;
    if (difference) {''','''    const uint64_t retained = parts_ ? actual : count_ * sizeof (msg_t);
    const uint64_t difference = reserved - retained;
    if (difference) {''')
 s=s.replace('_out_physical_queue, difference, parts_ ? 0 : 1);','_out_physical_queue, difference, 0);')
 s=s.replace('_msgs_written.load () - (parts_ ? 0 : 1),','_msgs_written.load (),')
 return s
edit('core/src/runtime/core/pipe_write.cpp',write)
print('Cancelled metadata remains charged to the existing ledger until consumed')
