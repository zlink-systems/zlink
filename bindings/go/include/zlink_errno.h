/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_ERRNO_H_INCLUDED__
#define __ZLINK_ERRNO_H_INCLUDED__

#if !defined _WIN32_WCE
#include <errno.h>
#endif

/******************************************************************************/
/*  zlink extended errno values.                                              */
/******************************************************************************/
#define ZLINK_HAUSNUMERO 156384712

#ifndef ENOTSUP
#define ENOTSUP (ZLINK_HAUSNUMERO + 1)
#endif
#ifndef EPROTONOSUPPORT
#define EPROTONOSUPPORT (ZLINK_HAUSNUMERO + 2)
#endif
#ifndef ENOBUFS
#define ENOBUFS (ZLINK_HAUSNUMERO + 3)
#endif
#ifndef ENETDOWN
#define ENETDOWN (ZLINK_HAUSNUMERO + 4)
#endif
#ifndef EADDRINUSE
#define EADDRINUSE (ZLINK_HAUSNUMERO + 5)
#endif
#ifndef EADDRNOTAVAIL
#define EADDRNOTAVAIL (ZLINK_HAUSNUMERO + 6)
#endif
#ifndef ECONNREFUSED
#define ECONNREFUSED (ZLINK_HAUSNUMERO + 7)
#endif
#ifndef EINPROGRESS
#define EINPROGRESS (ZLINK_HAUSNUMERO + 8)
#endif
#ifndef ENOTSOCK
#define ENOTSOCK (ZLINK_HAUSNUMERO + 9)
#endif
#ifndef EMSGSIZE
#define EMSGSIZE (ZLINK_HAUSNUMERO + 10)
#endif
#ifndef EAFNOSUPPORT
#define EAFNOSUPPORT (ZLINK_HAUSNUMERO + 11)
#endif
#ifndef ENETUNREACH
#define ENETUNREACH (ZLINK_HAUSNUMERO + 12)
#endif
#ifndef ECONNABORTED
#define ECONNABORTED (ZLINK_HAUSNUMERO + 13)
#endif
#ifndef ECONNRESET
#define ECONNRESET (ZLINK_HAUSNUMERO + 14)
#endif
#ifndef ENOTCONN
#define ENOTCONN (ZLINK_HAUSNUMERO + 15)
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT (ZLINK_HAUSNUMERO + 16)
#endif
#ifndef EHOSTUNREACH
#define EHOSTUNREACH (ZLINK_HAUSNUMERO + 17)
#endif
#ifndef ENETRESET
#define ENETRESET (ZLINK_HAUSNUMERO + 18)
#endif
#ifndef ESTALE
#define ESTALE (ZLINK_HAUSNUMERO + 19)
#endif
#ifndef EALREADY
#define EALREADY (ZLINK_HAUSNUMERO + 20)
#endif
#ifndef EDEADLK
#define EDEADLK (ZLINK_HAUSNUMERO + 21)
#endif
#ifndef ESHUTDOWN
#define ESHUTDOWN (ZLINK_HAUSNUMERO + 22)
#endif

#define EFSM (ZLINK_HAUSNUMERO + 51)
#define ENOCOMPATPROTO (ZLINK_HAUSNUMERO + 52)
#define ETERM (ZLINK_HAUSNUMERO + 53)
#define EMTHREAD (ZLINK_HAUSNUMERO + 54)

/******************************************************************************/
/*  zlink error code enums.                                                   */
/*                                                                            */
/*  Submit (1-12) and request completion (101-104) codes occupy               */
/*  non-overlapping ranges so a single int error code is unambiguous.          */
/******************************************************************************/

typedef enum zlink_submit_result_t
{
    /* Submit succeeded. */
    ZLINK_SUBMIT_OK = 0,

    /* Normal control-flow result. */
    ZLINK_SUBMIT_BACKPRESSURED = 1,
    ZLINK_SUBMIT_NOT_CONNECTED = 2,
    ZLINK_SUBMIT_NOT_FOUND = 3,
    ZLINK_SUBMIT_NOT_ADMITTED = 13,

    /* Runtime / lifecycle failure. */
    ZLINK_SUBMIT_TERMINATED = 4,

    /* Caller contract violation. */
    ZLINK_SUBMIT_INVALID_HANDLE = 5,
    ZLINK_SUBMIT_INVALID_ARGUMENT = 6,
    ZLINK_SUBMIT_NOT_SUPPORTED = 7,
    ZLINK_SUBMIT_INVALID_STATE = 8,
    ZLINK_SUBMIT_THREAD_VIOLATION = 9,

    /* Internal failure. */
    ZLINK_SUBMIT_OUT_OF_MEMORY = 10,
    ZLINK_SUBMIT_SEQ_EXHAUSTED = 11,
    ZLINK_SUBMIT_INTERNAL_ERROR = 12,

} zlink_submit_result_t;

/*  Request completion result (101+).                                        */
typedef enum zlink_request_result_t
{
    ZLINK_REQUEST_OK = 0,
    ZLINK_REQUEST_TIMED_OUT = 101,
    ZLINK_REQUEST_NOT_FOUND = 102,
    ZLINK_REQUEST_TERMINATED = 103,
    ZLINK_REQUEST_PROTOCOL_ERROR = 104,
    ZLINK_REQUEST_INTERNAL_ERROR = 105,
    ZLINK_REQUEST_REJECTED = 106,
    ZLINK_REQUEST_CONFLICT = 107,
    ZLINK_REQUEST_BUSY = 108,
    ZLINK_REQUEST_NOT_CONNECTED = 109,
    ZLINK_REQUEST_INVALID_ARGUMENT = 110,
    ZLINK_REQUEST_INVALID_STATE = 111,
    ZLINK_REQUEST_NOT_SUPPORTED = 112,
    ZLINK_REQUEST_BACKPRESSURED = 113
} zlink_request_result_t;

/*  Recv / subscribe / subscription_event result (201+).                     */
typedef enum zlink_recv_result_t
{
    ZLINK_RECV_OK = 0,
    ZLINK_RECV_NO_DATA = 201,        /* EAGAIN    — non-blocking, no data available */
    ZLINK_RECV_BUSY = 202,           /* EBUSY     — handler already attached */
    ZLINK_RECV_TERMINATED = 203,     /* ETERM     — context terminated */
    ZLINK_RECV_INVALID_HANDLE = 204, /* EFAULT    — NULL or invalid handle */
    ZLINK_RECV_NOT_SUPPORTED = 205,  /* ENOTSUP   — unsupported socket type for recv */
    ZLINK_RECV_INTERNAL_ERROR = 206,  /* internal errno that has no finer recv bucket */
    ZLINK_RECV_BUFFER_TOO_SMALL = 207, /* ENOBUFS — first record or topic does not fit */
    ZLINK_RECV_INVALID_STATE = 208     /* EINVAL/ESTALE/ESHUTDOWN — claim or revoke state */
} zlink_recv_result_t;

/*  Handler registration result (301+).                                      */
/*  Applies to recv_handler, send_ready_handler, and monitor_handler.        */
typedef enum zlink_handler_result_t
{
    ZLINK_HANDLER_OK = 0,
    ZLINK_HANDLER_INVALID_ARGUMENT = 301, /* EINVAL    — NULL handler */
    ZLINK_HANDLER_BUSY = 302,             /* EBUSY     — handler already attached */
    ZLINK_HANDLER_NOT_SUPPORTED = 303,    /* ENOTSUP   — unsupported subject */
    ZLINK_HANDLER_DEADLOCK = 304,         /* EDEADLK   — reentrant call (send_ready only) */
    ZLINK_HANDLER_INVALID_HANDLE = 305,   /* EFAULT    — NULL or invalid handle */
    ZLINK_HANDLER_INTERNAL_ERROR = 306    /* internal errno that has no finer handler bucket */
} zlink_handler_result_t;

/*  Close / destroy result (401+).                                           */
typedef enum zlink_close_result_t
{
    ZLINK_CLOSE_OK = 0,
    ZLINK_CLOSE_BUSY = 401,           /* EBUSY     — in-flight callback or API */
    ZLINK_CLOSE_SHUTDOWN = 402,       /* ESHUTDOWN — already closed */
    ZLINK_CLOSE_INVALID_HANDLE = 403, /* EFAULT    — NULL or invalid handle */
    ZLINK_CLOSE_INTERNAL_ERROR = 404  /* internal errno that has no finer close bucket */
} zlink_close_result_t;

/*  Bind result (501+).                                                      */
typedef enum zlink_bind_result_t
{
    ZLINK_BIND_OK = 0,
    ZLINK_BIND_INVALID_ARGUMENT = 501, /* EINVAL    — invalid endpoint */
    ZLINK_BIND_ADDR_IN_USE = 502,      /* EADDRINUSE — address already bound */
    ZLINK_BIND_NOT_SUPPORTED = 503,    /* ENOTSUP   — unsupported transport */
    ZLINK_BIND_INVALID_HANDLE = 504,   /* EFAULT    — NULL or invalid handle */
    ZLINK_BIND_INTERNAL_ERROR = 505    /* internal errno that has no finer bind bucket */
} zlink_bind_result_t;

/*  Connect / disconnect / unbind result (601+).                             */
typedef enum zlink_connect_result_t
{
    ZLINK_CONNECT_OK = 0,
    ZLINK_CONNECT_INVALID_ARGUMENT = 601, /* EINVAL    — invalid endpoint */
    ZLINK_CONNECT_NOT_SUPPORTED = 602,    /* ENOTSUP   — unsupported transport */
    ZLINK_CONNECT_INVALID_HANDLE = 603,   /* EFAULT    — NULL or invalid handle */
    ZLINK_CONNECT_INTERNAL_ERROR = 604,   /* internal errno that has no finer connect bucket */
    ZLINK_CONNECT_NOT_FOUND = 605,        /* ENOENT    — endpoint or peer routing id not found */
    ZLINK_CONNECT_CONFLICT = 606, /* EADDRINUSE — peer routing id matches more than one pipe */
    ZLINK_CONNECT_BUSY = 607,     /* EBUSY     — lifecycle owner rejects manual change */
    ZLINK_CONNECT_AUTH_FAILED = 608 /* EACCES  — trust profile or peer authentication failed */
} zlink_connect_result_t;

/*  Configuration result (701+).                                             */
/*  Applies to set/get_option, set/get_routing_id, set_tls,                  */
/*  set/unset_subscription, and subscription_at.                             */
typedef enum zlink_config_result_t
{
    ZLINK_CONFIG_OK = 0,
    ZLINK_CONFIG_INVALID_HANDLE = 701,   /* EFAULT    — NULL or invalid handle */
    ZLINK_CONFIG_INVALID_ARGUMENT = 702, /* EINVAL    — invalid parameter */
    ZLINK_CONFIG_NOT_SUPPORTED = 703,    /* ENOTSUP   — unsupported option */
    ZLINK_CONFIG_INTERNAL_ERROR = 704,   /* internal errno that has no finer config bucket */
    ZLINK_CONFIG_INVALID_STATE = 705,    /* EBUSY/ESHUTDOWN — lifecycle state rejects config */
    ZLINK_CONFIG_NOT_FOUND = 706,        /* ENOENT    — local lookup target not found */
    ZLINK_CONFIG_CONFLICT = 707,         /* EEXIST    — duplicate identity, name or binding */
    ZLINK_CONFIG_BUFFER_TOO_SMALL = 708, /* ENOBUFS   — caller output capacity too small */
    ZLINK_CONFIG_BUSY = 709              /* EBUSY     — concurrent use of a mutable object */
} zlink_config_result_t;

#endif
