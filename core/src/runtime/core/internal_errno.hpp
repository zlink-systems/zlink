/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_INTERNAL_ERRNO_HPP_INCLUDED__
#define __ZLINK_INTERNAL_ERRNO_HPP_INCLUDED__

#include "../../include/zlink_errno.h"

// EPROTO is not available on some targets such as OpenBSD.
#ifndef EPROTO
#define EPROTO 0
#endif

#ifndef ESHUTDOWN
#define ESHUTDOWN ENOTCONN
#endif

namespace zlink
{
namespace internal_errno
{
/*
 * Internal errno remains the detailed failure channel inside core.
 *
 * Public APIs normalize these values at the exported boundary:
 * - send/request/reply submit -> zlink_submit_result_t
 * - request callback completion -> zlink_request_result_t
 *
 * This file is the single internal catalog for the errno values that are
 * intentionally recognized by core and by those normalization layers.
 *
 * Rules:
 * - Internal paths keep the original errno.
 * - Only exported boundaries normalize errno to public result enums.
 * - Errors that are ambiguous or transport-specific remain errno-only.
 * - This catalog includes direct errno assignment, propagated errno checks,
 *   and Windows WSA-to-errno translation targets used inside core/src.
 */
enum class submit_error_class
{
    /* Submit succeeded and no internal errno needs interpretation. */
    none = 0,
    /* Submit was rejected by policy or temporary routing availability. */
    control_flow,
    /* Submit failed because the runtime or context is shutting down. */
    runtime_failure,
    /* Submit violated the public or internal call contract. */
    contract_failure,
    /* Submit failed while allocating or building internal state. */
    internal_failure,
    /* Submit errno is not yet classified by the public normalization layer. */
    unknown
};

enum class request_completion_class
{
    /* Callback completed with a reply payload. */
    success = 0,
    /* Callback completed because the request timed out. */
    timeout,
    /* Callback completed with an explicit target-not-found result. */
    target_not_found,
    /* Callback completed because the runtime terminated. */
    terminated,
    /* Callback completed with a decode or envelope error. */
    protocol_error,
    /* Callback errno is not yet classified by the completion layer. */
    unknown
};

enum class domain
{
    /* Plain POSIX errno or platform errno normalized to a POSIX-style value. */
    posix_or_system = 0,
    /* ZLINK_HAUSNUMERO-based public extension errno. */
    zlink_extended,
    /* Internal-only classification bucket with no public errno code. */
    internal_only
};

enum class error_class
{
    /* No error. */
    none = 0,
    /* Operation failed because permission or access policy denied it. */
    permission_denied,
    /* Operation was interrupted and may be safely retried by the caller. */
    interrupted,
    /* Operation would block under the current submit or I/O policy. */
    would_block,
    /* Peer, path, transport, or connection state is unavailable. */
    connectivity,
    /* Requested target, path, device, or registry entry does not exist. */
    target_not_found,
    /* Operation is still pending or has exceeded a timeout window. */
    timeout,
    /* Context or owning runtime has been terminated. */
    terminated,
    /* Handle or destination reference is invalid for this operation. */
    invalid_handle,
    /* Argument value or argument shape is invalid. */
    invalid_argument,
    /* Feature, option, protocol, or operation is not supported. */
    unsupported,
    /* State machine or lifecycle state does not allow the operation. */
    invalid_state,
    /* Call violated the thread-affinity or thread-safety contract. */
    thread_violation,
    /* Resource budget is exhausted: memory, buffers, descriptors, address slot. */
    resource_exhausted,
    /* Message or protocol frame is malformed or incompatible. */
    protocol,
    /* Low-level transport I/O failed outside a narrower category. */
    transport_io,
    /* Endpoint or address-family configuration is invalid or unavailable. */
    addressing,
    /* File system path or directory shape is invalid. */
    filesystem,
    /* Internal failure bucket for uncategorized core-only failures. */
    internal_failure,
    /* Errno is still outside the internal catalog. */
    unknown
};

/*
 * Internal errno catalog
 *
 * control-flow:
 * - used by loops, pollers, and non-blocking send/recv paths
 * - EINTR, EAGAIN, EWOULDBLOCK
 *
 * connectivity / transport availability:
 * - used when a peer, route, socket, or network path exists conceptually
 *   but is not currently usable
 * - ENOTCONN, EHOSTUNREACH, ENETUNREACH, ECONNREFUSED
 * - ECONNRESET, ECONNABORTED, ENETDOWN, ENETRESET
 * - EPIPE, ESHUTDOWN, EBADF
 *
 * target/path lookup:
 * - used when a logical destination, registry entry, device, or local path
 *   cannot be found
 * - ENOENT, ENODEV, ENOTDIR
 *
 * timeout / in-progress:
 * - used for request timeout, connect timeout, or "already in progress"
 *   style lifecycle checks
 * - ETIMEDOUT, EINPROGRESS, EALREADY
 *
 * lifecycle:
 * - used when the owning zlink context or runtime is shutting down
 * - ETERM
 *
 * contract:
 * - used when the caller breaks API rules or when the internal state machine
 *   rejects an otherwise well-formed call
 * - EFAULT, ENOTSOCK, EDESTADDRREQ
 * - EINVAL, EMSGSIZE, EAFNOSUPPORT, ENAMETOOLONG
 * - ENOTSUP, EOPNOTSUPP, EPROTONOSUPPORT, ENOPROTOOPT
 * - EFSM, EBUSY, EDEADLK, EISCONN, EMTHREAD
 * - EACCES
 *
 * resources:
 * - used when memory, buffers, addresses, or file/socket descriptors are
 *   exhausted during setup or I/O preparation
 * - ENOMEM, ENOBUFS, EADDRINUSE, EADDRNOTAVAIL, EMFILE, ENFILE
 *
 * protocol/build/decode:
 * - used when envelopes, headers, metadata, or payload framing are malformed
 *   or incompatible with the active protocol
 * - EPROTO, ENOCOMPATPROTO, EBADMSG
 *
 * transport I/O:
 * - fallback I/O failure when a narrower transport category is not available
 * - EIO
 *
 * address/file-system style setup failures used by some transports:
 * - mostly seen during bind/connect/setup paths for IPC, TCP, WS, and TLS
 * - EADDRINUSE, EADDRNOTAVAIL, ENOTDIR
*/

/*
 * control_flow:
 * - caller may handle and retry based on submit policy
 * runtime_failure:
 * - lifecycle/runtime failure that is visible outside the call
 * contract_failure:
 * - invalid handle, invalid argument, unsupported operation, invalid state
 * internal_failure:
 * - allocation or internal protocol/build failure
 */
inline domain classify_domain (int err_)
{
    if (err_ >= ZLINK_HAUSNUMERO)
        return domain::zlink_extended;

    return domain::posix_or_system;
}

/* Submit control-flow maps to retry/backpressure style public results. */
inline bool is_submit_control_flow (int err_)
{
    return err_ == EAGAIN || err_ == ETIMEDOUT || err_ == ENOBUFS || err_ == ENOTCONN
           || err_ == EHOSTUNREACH || err_ == ECONNREFUSED || err_ == EACCES || err_ == ENOENT
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
           || err_ == EWOULDBLOCK
#endif
      ;
}

/* Runtime failure is a local lifecycle break rather than retryable routing loss. */
inline bool is_submit_runtime_failure (int err_)
{
    return err_ == ETERM || err_ == ESHUTDOWN;
}

/* Contract failure covers invalid handles, invalid arguments, and bad state. */
inline bool is_submit_contract_failure (int err_)
{
    return err_ == EFAULT || err_ == EINVAL || err_ == EMSGSIZE || err_ == ENOTSUP
           || err_ == EOPNOTSUPP || err_ == EFSM || err_ == EBUSY || err_ == ESTALE
           || err_ == EALREADY || err_ == EDEADLK || err_ == EPERM || err_ == EMTHREAD
           || err_ == EOVERFLOW;
}

/* Internal submit failure covers allocation or internal protocol preparation. */
inline bool is_submit_internal_failure (int err_)
{
    return err_ == ENOMEM || err_ == EPROTO;
}

/* Would-block means the caller may retry according to the active policy. */
inline bool is_would_block (int err_)
{
    return err_ == EAGAIN
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
           || err_ == EWOULDBLOCK
#endif
      ;
}

/* Permission denied is intentionally kept internal and is not publicized as a
 * dedicated submit result.
 */
inline bool is_permission_denied (int err_)
{
    return err_ == EACCES;
}

/* Interrupted means the current syscall or wait path was interrupted. */
inline bool is_interrupted (int err_)
{
    return err_ == EINTR;
}

/* Connectivity captures peer/path unavailability and broken transport state. */
inline bool is_connectivity (int err_)
{
    return err_ == ENOTCONN || err_ == EHOSTUNREACH || err_ == ENETUNREACH || err_ == ECONNREFUSED
           || err_ == ECONNRESET || err_ == ECONNABORTED || err_ == ENETDOWN || err_ == ENETRESET
           || err_ == EPIPE || err_ == ESHUTDOWN || err_ == EBADF;
}

/* Target-not-found is used for missing routes, devices, or local registry keys. */
inline bool is_target_not_found (int err_)
{
    return err_ == ENOENT || err_ == ENODEV || err_ == ENOTDIR;
}

/* Timeout includes timed-out work and "already/in-progress" style progress states. */
inline bool is_timeout (int err_)
{
    return err_ == ETIMEDOUT || err_ == EINPROGRESS || err_ == EALREADY;
}

/* Invalid-handle is narrower than invalid-argument and points to the handle itself. */
inline bool is_invalid_handle (int err_)
{
    return err_ == EFAULT || err_ == ENOTSOCK || err_ == EDESTADDRREQ;
}

/* Invalid-argument means the call shape or value contract is broken. */
inline bool is_invalid_argument (int err_)
{
    return err_ == EINVAL || err_ == EMSGSIZE || err_ == EAFNOSUPPORT || err_ == ENAMETOOLONG;
}

/* Unsupported means the stack recognized the request but cannot honor it. */
inline bool is_unsupported (int err_)
{
    return err_ == ENOTSUP || err_ == EOPNOTSUPP || err_ == EPROTONOSUPPORT || err_ == ENOPROTOOPT;
}

/* Invalid-state means the object exists but is in the wrong lifecycle/state. */
inline bool is_invalid_state (int err_)
{
    return err_ == EFSM || err_ == EBUSY || err_ == EDEADLK || err_ == EISCONN;
}

/* Resource exhaustion captures memory, buffers, descriptors, and address slots. */
inline bool is_resource_exhausted (int err_)
{
    return err_ == ENOMEM || err_ == ENOBUFS || err_ == EADDRINUSE || err_ == EADDRNOTAVAIL
           || err_ == EMFILE || err_ == ENFILE;
}

/* Addressing is kept separate because transports often treat setup errors specially. */
inline bool is_addressing (int err_)
{
    return err_ == EADDRINUSE || err_ == EADDRNOTAVAIL || err_ == EAFNOSUPPORT;
}

/* Filesystem currently covers path-shape failures used by IPC-style transports. */
inline bool is_filesystem (int err_)
{
    return err_ == ENOTDIR;
}

/* Protocol covers malformed or incompatible metadata, frames, and envelopes. */
inline bool is_protocol (int err_)
{
    return err_ == EPROTO || err_ == ENOCOMPATPROTO || err_ == EBADMSG;
}

/* Transport I/O is the low-level fallback bucket for residual I/O failure. */
inline bool is_transport_io (int err_)
{
    return err_ == EIO;
}

/* Internal failure is the coarse internal-only fallback for fatal core-side work. */
inline bool is_internal_failure (int err_)
{
    return is_resource_exhausted (err_) || is_protocol (err_) || is_transport_io (err_);
}

/* Request completion is intentionally narrower than the full internal catalog. */
inline bool is_request_completion (int err_)
{
    return err_ == 0 || err_ == ETIMEDOUT || err_ == ENOENT || err_ == EPROTO || err_ == ETERM;
}

/* General internal classification used for cataloging and maintenance review. */
inline error_class classify (int err_)
{
    if (err_ == 0)
        return error_class::none;

    if (is_permission_denied (err_))
        return error_class::permission_denied;

    if (is_interrupted (err_))
        return error_class::interrupted;

    if (is_would_block (err_))
        return error_class::would_block;

    if (is_connectivity (err_))
        return error_class::connectivity;

    if (is_target_not_found (err_))
        return error_class::target_not_found;

    if (is_timeout (err_))
        return error_class::timeout;

    if (err_ == ETERM)
        return error_class::terminated;

    if (is_invalid_handle (err_))
        return error_class::invalid_handle;

    if (is_invalid_argument (err_))
        return error_class::invalid_argument;

    if (is_unsupported (err_))
        return error_class::unsupported;

    if (is_invalid_state (err_))
        return error_class::invalid_state;

    if (err_ == EMTHREAD)
        return error_class::thread_violation;

    if (is_resource_exhausted (err_))
        return error_class::resource_exhausted;

    if (is_protocol (err_))
        return error_class::protocol;

    if (is_transport_io (err_))
        return error_class::transport_io;

    if (is_addressing (err_))
        return error_class::addressing;

    if (is_filesystem (err_))
        return error_class::filesystem;

    return error_class::unknown;
}

inline submit_error_class classify_submit (int err_)
{
    if (err_ == 0)
        return submit_error_class::none;

    if (is_submit_control_flow (err_))
        return submit_error_class::control_flow;

    if (is_submit_runtime_failure (err_))
        return submit_error_class::runtime_failure;

    if (is_submit_contract_failure (err_))
        return submit_error_class::contract_failure;

    if (is_submit_internal_failure (err_))
        return submit_error_class::internal_failure;

    return submit_error_class::unknown;
}

inline request_completion_class classify_request_completion (int err_)
{
    switch (err_) {
        case 0:
            return request_completion_class::success;
        case ETIMEDOUT:
            return request_completion_class::timeout;
        case ENOENT:
            return request_completion_class::target_not_found;
        case ETERM:
            return request_completion_class::terminated;
        case EPROTO:
            return request_completion_class::protocol_error;
        default:
            return request_completion_class::unknown;
    }
}

inline bool is_known_submit_errno (int err_)
{
    return classify_submit (err_) != submit_error_class::unknown;
}

inline bool is_known_request_completion_errno (int err_)
{
    return classify_request_completion (err_) != request_completion_class::unknown;
}

inline bool is_known_internal_errno (int err_)
{
    return classify (err_) != error_class::unknown;
}
}
}

#endif
