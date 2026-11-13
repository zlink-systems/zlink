from pathlib import Path
import shutil
r=Path('/root/worktree/zlink-cstage');j=Path('/root/jobs/fx/cx/v21/s033/cstage');pfx=j/'prototype-source'
for f in ['core/src/api/socket/socket_request_reply_pending_api.cpp','core/src/api/socket/socket_request_reply_dispatch.cpp']:
 p=j/'original'/f;p.parent.mkdir(parents=True,exist_ok=True)
 if not p.exists():shutil.copy2(r/f,p)
 p=pfx/f;p.parent.mkdir(parents=True,exist_ok=True)
 if not p.exists():shutil.copy2(r/f,p)
p=pfx/'core/src/api/socket/socket_request_reply_internal.hpp';s=p.read_text().replace('void release_pending_request_completion (','void cancel_socket_staged_body (const std::shared_ptr<socket_request_reply_state_t> &, pending_request_t *);\nvoid release_pending_request_completion (');p.write_text(s)
p=pfx/'core/src/api/socket/socket_request_reply_internal.cpp';s=p.read_text();pos=s.index('void release_pending_request_completion (')
s=s[:pos]+'''void cancel_socket_staged_body (
  const std::shared_ptr<socket_request_reply_state_t> &state_, pending_request_t *pending_)
{
    if (!pending_->staged_part_count || !pending_->correlation.pipe ())
        return;
    auto &owner = state_->socket->lifecycle_coordinator ();
    socket_public_api_lock_scope_t turn (
      owner, !owner.public_api_sync_owned_by_current_thread ());
    (void) pending_->correlation.pipe ()->publish_staged_record (
      pending_->staged_slots, pending_->staged_part_count, NULL);
    pending_->staged_part_count = 0;
}

'''+s[pos:]
s=s.replace('        pending.correlation.release ();','        cancel_socket_staged_body (ctx->state, &pending);\n        pending.correlation.release ();')
s=s.replace('    pending_->correlation.release ();','    cancel_socket_staged_body (state_, pending_);\n    pending_->correlation.release ();')
s=s.replace('            it->correlation.release ();','            cancel_socket_staged_body (state, &*it);\n            it->correlation.release ();')
s=s.replace('        failed.correlation.release ();','        cancel_socket_staged_body (state_, &failed);\n        failed.correlation.release ();');p.write_text(s)
p=pfx/'core/src/api/socket/socket_request_reply_pending_api.cpp';s=p.read_text().replace('        pending.correlation.release ();','        reqrep::cancel_socket_staged_body (state_, &pending);\n        pending.correlation.release ();');p.write_text(s)
p=pfx/'core/src/api/socket/socket_request_reply_dispatch.cpp';s=p.read_text().replace('    pending.correlation.release ();','    cancel_socket_staged_body (state_, &pending);\n    pending.correlation.release ();');p.write_text(s)
p=pfx/'core/src/api/socket/socket_request_reply_submit_api.cpp';s=p.read_text().replace('''    if (!zlink::staged_prototype::capture)
        (void) reqrep::arm_socket_pending_request_timeout (state_, token_);''','    (void) reqrep::arm_socket_pending_request_timeout (state_, token_);')
s=s.replace('''    if (parts_)
        return reqrep::arm_socket_pending_request_timeout (state, token);
    failed.correlation.release ();''','''    if (parts_)
        return 0;
    failed.correlation.release ();''');p.write_text(s)
p=pfx/'bindings/cpp/src/Runtime/Messaging/completion_owner.cpp';s=p.read_text().replace('''              }) != 0)
            throw std::runtime_error ("staged publish failed");''','''              }) != 0 && zlink_errno () != ECANCELED)
            throw std::runtime_error ("staged publish failed");''').replace('''        if (zlink_proto_request_fail (operation->raw.socket, operation->staged_identity) != 0)
            terminate (zlink_errno ());''','''        if (zlink_proto_request_fail (operation->raw.socket, operation->staged_identity) != 0
            && zlink_errno () != ECANCELED)
            terminate (zlink_errno ());''');p.write_text(s)
print('Staged cancellation uses the existing lifecycle sync and aggregate timeout')
