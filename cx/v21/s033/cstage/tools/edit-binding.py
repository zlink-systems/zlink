exec(open('/root/jobs/fx/cx/v21/s033/cstage/tools/edit-core.py').read().split("new('core/src/api/socket/staged_request_prototype.hpp'")[0])
# Reuse the Core pending list as the worker's discovery index.
edit('core/src/api/socket/staged_request_prototype.hpp',lambda s:s.replace('int zlink_proto_request_fail (void *, zlink_staged_request_t);','int zlink_proto_request_fail (void *, zlink_staged_request_t);\nvoid *zlink_proto_request_next_body (void *);'))
edit('core/src/libzlink.vers',lambda s:s.replace('    zlink_proto_request_fail;','    zlink_proto_request_fail;\n    zlink_proto_request_next_body;'))
edit('core/src/api/socket/socket_request_reply_submit_api.cpp',lambda s:s+'''
extern "C" void *zlink_proto_request_next_body (void *socket_)
{
    socket_handle_t handle = as_socket_handle (socket_);
    if (!handle.socket) return NULL;
    auto state = reqrep::find_request_reply_state (handle);
    if (!state) return NULL;
    std::lock_guard<std::mutex> lock (state->mutex);
    for (auto it = state->pending_requests.begin (); it != state->pending_requests.end (); ++it)
        if (it->second.staged_part_count)
            return it->second.pull_completion->completion.user_context;
    return NULL;
}
''')
edit('bindings/cpp/include/zlink/Contracts/Messaging/operation_contracts.hpp',lambda s:s.replace('    request_submit_operation_t message (message_t &part_) &&;', '''    // Measurement-only: reserve bounds before the body factory runs.
    request_submit_operation_t deferred (
      std::vector<uint64_t> bounds_,
      std::function<std::vector<message_t> ()> body_) &&;
    request_submit_operation_t message (message_t &part_) &&;''',1))
# std::function is already included by the contract; ensure explicit include.
edit('bindings/cpp/include/zlink/Contracts/Messaging/operation_contracts.hpp',lambda s:s.replace('#include <chrono>','#include <chrono>\n#include <functional>'))
edit('bindings/cpp/src/Runtime/Messaging/operation_state.hpp',lambda s:s.replace('#include <chrono>','#include <chrono>\n#include <functional>\n#include "../../../../../core/src/api/socket/staged_request_prototype.hpp"').replace('    message_parts_t message;', '''    std::vector<uint64_t> staged_bounds;
    std::function<std::vector<message_t> ()> staged_body;
    zlink_staged_request_t staged_identity;
    message_parts_t message;''').replace('    state_.kind = operation_kind_t::none;', '''    state_.staged_bounds.clear ();
    state_.staged_body = {};
    state_.staged_identity = {};
    state_.kind = operation_kind_t::none;'''))
edit('bindings/cpp/src/Runtime/Messaging/request_reply.cpp',lambda s:s.replace('request_submit_operation_t request_operation_t::message (message_t &part_) &&','''request_submit_operation_t request_operation_t::deferred (
  std::vector<uint64_t> bounds_, std::function<std::vector<message_t> ()> body_) &&
{
    if (bounds_.empty () || bounds_.size () > 4 || !body_)
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    state ().staged_bounds = std::move (bounds_);
    state ().staged_body = std::move (body_);
    return request_submit_operation_t (release_state_ptr ());
}

request_submit_operation_t request_operation_t::message (message_t &part_) &&''').replace('if (!detail::has_send_parts (operation))','if (!detail::has_send_parts (operation) && !operation.staged_body)'))
edit('bindings/cpp/src/Runtime/Messaging/operation_submit.hpp',lambda s:s.replace('    int raw_result = -1;\n    if (state_.message.single_part.has_value ()', '''    int raw_result = -1;
    if (state_.staged_body) {
        raw_result = zlink_proto_request_reserve (
          state_.raw.socket, target, state_.staged_bounds.data (),
          state_.staged_bounds.size (), timeout, user_context_, completion_id_out_,
          &state_.staged_identity);
    } else if (state_.message.single_part.has_value ()'''))
edit('bindings/cpp/src/Runtime/Messaging/completion_owner.hpp',lambda s:s.replace('#include <vector>','#include <vector>\n#include <thread>').replace('    bool start_request ();','''    bool start_request ();
    bool has_staged_body ();
    std::unique_ptr<operation_state_t> take_staged_body ();
    void build_staged_body (std::unique_ptr<operation_state_t>);''').replace('    void insert_entry_locked (','    void body_worker_loop ();\n    std::thread _body_worker;\n    void insert_entry_locked ('))
def co(s):
 s=s.replace('#include <exception>','#include <exception>\n#include <pthread.h>')
 s=s.replace('        admitted_operation = std::move (_request_operation);','''        if (!_request_operation->staged_body)
            admitted_operation = std::move (_request_operation);''')
 s=s.replace('    return entry_->start_request ();','''    if (entry_->has_staged_body ()) {
        std::lock_guard<std::mutex> lock (_mutex);
        if (!_body_worker.joinable ())
            _body_worker = std::thread ([this] { body_worker_loop (); });
    }
    const bool admitted = entry_->start_request ();
    _send_registered.notify_one ();
    return admitted;''')
 s=s.replace('    _send_registered.notify_all ();\n    std::shared_ptr<completion_entry_t> inline_entry', '''    _send_registered.notify_all ();
    lock.unlock ();
    if (_body_worker.joinable ())
        _body_worker.join ();
    lock.lock ();
    std::shared_ptr<completion_entry_t> inline_entry''')
 # Notify after native WRITABLE resubmission admits a staged request.
 s=s.replace('return submit_request_attempt (false);','return submit_request_attempt (false);')
 # All drain exits after retry list must wake worker. Find actual retry loop later if needed.
 s=s.replace('    return processed;\n}', '    _send_registered.notify_one ();\n    return processed;\n}')
 pos=s.index('completion_owner_t::completion_owner_t (void *socket_)')
 s=s[:pos]+'''bool completion_entry_t::has_staged_body ()
{
    std::lock_guard<std::mutex> lock (_mutex);
    return _request_operation && bool (_request_operation->staged_body);
}

std::unique_ptr<operation_state_t> completion_entry_t::take_staged_body ()
{
    std::lock_guard<std::mutex> lock (_mutex);
    if (_published && !_request_waiting_writable && _request_operation
        && _request_operation->staged_body && _request_operation->staged_identity.sequence)
        return std::move (_request_operation);
    return {};
}

void completion_entry_t::build_staged_body (std::unique_ptr<operation_state_t> operation)
{
    try {
        operation->message.parts = operation->staged_body ();
        if (submit_message_parts (operation->message.parts,
              [&] (zlink_msg_t *parts, size_t count) {
                  return zlink_proto_request_publish (
                    operation->raw.socket, operation->staged_identity, parts, count);
              }) != 0)
            throw std::runtime_error ("staged publish failed");
    }
    catch (...) {
        if (zlink_proto_request_fail (operation->raw.socket, operation->staged_identity) != 0)
            terminate (zlink_errno ());
    }
    release_state (std::move (operation));
}

void completion_owner_t::body_worker_loop ()
{
    pthread_setname_np (pthread_self (), "cstage-body");
    for (;;) {
        std::shared_ptr<completion_entry_t> entry;
        std::unique_ptr<operation_state_t> operation;
        {
            std::unique_lock<std::mutex> lock (_mutex);
            _send_registered.wait (lock, [&] {
                void *const context = zlink_proto_request_next_body (_socket);
                if (context) {
                    if (_inline_entry && _inline_entry->context () == context)
                        entry = _inline_entry;
                    else {
                        const auto it = _entries.find (context);
                        if (it != _entries.end ()) entry = it->second;
                    }
                    if (entry) operation = entry->take_staged_body ();
                }
                return operation || _shutdown;
            });
            if (!operation) return;
        }
        entry->build_staged_body (std::move (operation));
    }
}

'''+s[pos:]
 return s
edit('bindings/cpp/src/Runtime/Messaging/completion_owner.cpp',co)
# The prototype worker must finish accepted body work before native close.
edit('bindings/cpp/src/Runtime/Sockets/socket.cpp',lambda s:s.replace('socket_t::~socket_t ()\n{','''socket_t::~socket_t ()
{
    if (_runtime)
        _runtime->completion->shutdown ();'''))
print('Binding prototype edited')
