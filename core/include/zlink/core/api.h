/* SPDX-License-Identifier: MPL-2.0 */

#ifndef ZLINK_CORE_API_H_INCLUDED
#define ZLINK_CORE_API_H_INCLUDED

#include <zlink/common.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Public declarations for the helper substrate layer.
 *
 * The helper substrate layer exposes the `*_part` primitives. These entry
 * points are the low-level building blocks intended for bindings
 * implementations that need part-by-part control over send and recv paths.
 */

/**
 * @brief Return the errno for the current thread.
 * @return errno value (POSIX errno or ZLINK_HAUSNUMERO-based extended code).
 */
ZLINK_EXPORT int zlink_errno (void);

/**
 * @brief Return a human-readable string for the given error number.
 * @param errnum_  Error number (e.g. return value of zlink_errno()).
 * @return Static string pointer. Must not be modified or freed.
 */
ZLINK_EXPORT const char *zlink_strerror (int errnum_);

/**
 * @brief Return the runtime library version.
 * @param[out] major_  Major version.
 * @param[out] minor_  Minor version.
 * @param[out] patch_  Patch version.
 */
ZLINK_EXPORT void zlink_version (int *major_, int *minor_, int *patch_);

/******************************************************************************/
/*  0MQ infrastructure (a.k.a. context) initialisation & termination.         */
/******************************************************************************/
#define ZLINK_IO_THREADS_DFLT 4
#define ZLINK_MAX_SOCKETS_DFLT 4095
#define ZLINK_THREAD_PRIORITY_DFLT -1
#define ZLINK_THREAD_SCHED_POLICY_DFLT -1
#define ZLINK_CTX_AUTO_HWM_ENABLE_DFLT 1
#define ZLINK_CTX_AUTO_HWM_RECALC_DEBOUNCE_MS_DFLT 3000
#define ZLINK_CTX_AUTO_HWM_PROFILE_DFLT ZLINK_AUTO_HWM_PROFILE_BALANCED
#define ZLINK_CTX_AUTO_HWM_MSG_UNIT_BYTES_DFLT ((uint64_t) 0)

/**
 * @brief Create a new zlink context.
 *
 * A context manages I/O threads and serves as the foundation for
 * creating sockets. Must be released with zlink_ctx_term().
 *
 * @return Context handle, or NULL on failure (errno is set).
 */
ZLINK_EXPORT void *zlink_ctx_new (void);

/**
 * @brief Terminate the context and release all resources.
 *
 * May block until all sockets are closed.
 *
 * @param context_  Context handle.
 * @return 0 on success, -1 on failure (errno is set).
 */
ZLINK_EXPORT zlink_close_result_t zlink_ctx_term (void *context_);

/**
 * @brief Shut down the context immediately.
 *
 * Interrupts any blocking calls with ETERM.
 * zlink_ctx_term() must still be called for final cleanup.
 *
 * @param context_  Context handle.
 * @return 0 on success, -1 on failure (errno is set).
 */
ZLINK_EXPORT zlink_close_result_t zlink_ctx_shutdown (void *context_);

/**
 * @brief Set a context option.
 * @param context_  Context handle.
 * @param option_   Option name (ZLINK_IO_THREADS, ZLINK_MAX_SOCKETS, etc.).
 * @param optval_   Option value. `ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES`
 *                  is not an int option; use zlink_ctx_set_data().
 * @return 0 on success, -1 on failure (errno is set).
 */
ZLINK_EXPORT zlink_config_result_t zlink_ctx_set (void *context_,
                                                  zlink_ctx_option_t option_,
                                                  int optval_);

/**
 * @brief Set a context option from a byte buffer.
 *
 * This is used for context options whose public binding type is not an int.
 * `ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES` requires an exact `uint64_t`
 * byte value. `0` selects the socket-type planning-unit default.
 *
 * @param context_    Context handle.
 * @param option_     Option name.
 * @param optval_     Option value bytes.
 * @param optvallen_  Number of bytes in optval_.
 * @return ZLINK_CONFIG_OK on success, otherwise a zlink_config_result_t error.
 */
ZLINK_EXPORT zlink_config_result_t zlink_ctx_set_data (void *context_,
                                                       zlink_ctx_option_t option_,
                                                       const void *optval_,
                                                       size_t optvallen_);

/**
 * @brief Get a context option into a caller-provided byte buffer.
 *
 * `ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES` requires a `uint64_t` output
 * buffer. The function writes the required size to @p optvallen_.
 *
 * @param context_    Context handle.
 * @param option_     Option name.
 * @param optval_     Output buffer.
 * @param optvallen_  Input capacity and output value size.
 * @return ZLINK_CONFIG_OK on success, otherwise a zlink_config_result_t error.
 */
ZLINK_EXPORT zlink_config_result_t zlink_ctx_get_data (void *context_,
                                                       zlink_ctx_option_t option_,
                                                       void *optval_,
                                                       size_t *optvallen_);

/**
 * @brief Get a context option.
 * @param context_  Context handle.
 * @param option_   Option name.
 * @return Option value, or -1 on failure (errno is set).
 * `ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES` is not available through this
 * function; use zlink_ctx_get_data().
 */
ZLINK_EXPORT int
zlink_ctx_get (void *context_, zlink_ctx_option_t option_, zlink_config_result_t *error_out_);

/**
 * @brief Recalculate and apply auto HWM for the entire context immediately.
 *
 * If auto HWM is disabled, this is a no-op that returns success.
 *
 * @param context_  Context handle.
 * @return 0 on success, -1 on failure (errno is set).
 */
ZLINK_EXPORT zlink_config_result_t zlink_ctx_auto_hwm_recalculate (void *context_);

/** @brief Start a built-in proxy between frontend and backend sockets. */
ZLINK_EXPORT zlink_config_result_t zlink_proxy (void *frontend_, void *backend_, void *capture_);

/** @brief Start a steerable proxy with an additional control socket. */
ZLINK_EXPORT zlink_config_result_t zlink_proxy_steerable (void *frontend_,
                                                          void *backend_,
                                                          void *capture_,
                                                          void *control_);

/** @brief Check if the library supports a given capability (e.g. "ipc", "tls"). */
ZLINK_EXPORT bool zlink_has (const char *capability_);

/******************************************************************************/
/*  Atomic utility methods                                                    */
/******************************************************************************/

/** @brief Create a new atomic counter, initialized to zero. */
ZLINK_EXPORT void *zlink_atomic_counter_new (void);
ZLINK_EXPORT void zlink_atomic_counter_set (void *counter_, int value_);
ZLINK_EXPORT int zlink_atomic_counter_inc (void *counter_);
ZLINK_EXPORT int zlink_atomic_counter_dec (void *counter_);
ZLINK_EXPORT int zlink_atomic_counter_value (void *counter_);
ZLINK_EXPORT void zlink_atomic_counter_destroy (void **counter_p_);

/** @brief Start a high-resolution stopwatch. Returns an opaque handle. */
ZLINK_EXPORT void *zlink_stopwatch_start (void);

/** @brief Return elapsed microseconds without stopping the stopwatch. */
ZLINK_EXPORT unsigned long zlink_stopwatch_intermediate (void *watch_);

/** @brief Stop the stopwatch and return total elapsed microseconds. */
ZLINK_EXPORT unsigned long zlink_stopwatch_stop (void *watch_);

/** @brief Sleep for the given number of seconds. */
ZLINK_EXPORT void zlink_sleep (int seconds_);

typedef void (zlink_thread_fn) (void *);

/** @brief Start a new thread running the given function. Returns a thread handle. */
ZLINK_EXPORT void *zlink_thread_start (zlink_thread_fn *func_, void *arg_);

/** @brief Wait for a thread to finish and release its handle. */
ZLINK_EXPORT void zlink_thread_join (void *thread_);

#ifdef __cplusplus
}
#endif

#endif
