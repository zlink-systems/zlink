/* SPDX-License-Identifier: MPL-2.0 */

#ifndef ZLINK_MESSAGE_API_H_INCLUDED
#define ZLINK_MESSAGE_API_H_INCLUDED

#include <zlink/common.h>

#ifdef __cplusplus
extern "C" {
#endif

/******************************************************************************/
/*  0MQ message definition.                                                   */
/******************************************************************************/
/* ========== Message type and helpers ========== */
typedef struct zlink_msg_t
{
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
    __declspec (align (8)) unsigned char _[64];
#elif defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_ARM_ARMV7VE) || defined(_M_ARM))
    __declspec (align (4)) unsigned char _[64];
#elif defined(__GNUC__) || defined(__INTEL_COMPILER)                                               \
  || (defined(__SUNPRO_C) && __SUNPRO_C >= 0x590)                                                  \
  || (defined(__SUNPRO_CC) && __SUNPRO_CC >= 0x590)
    unsigned char _[64] __attribute__ ((aligned (sizeof (void *))));
#else
    unsigned char _[64];
#endif
} zlink_msg_t;

typedef struct zlink_routing_id_t
{
    uint8_t size;
    uint8_t data[255];
} zlink_routing_id_t;

typedef void (zlink_free_fn) (void *data_, void *hint_);
#define ZLINK_MSG_METADATA_KEY_USER_MIN 0x0100
#define ZLINK_MSG_METADATA_VALUE_MAX 65535

/** @brief Initialize an empty message. Must be closed with zlink_msg_close(). */
ZLINK_EXPORT zlink_config_result_t zlink_msg_init (zlink_msg_t *msg_);

/** @brief Initialize a message of the given size. */
ZLINK_EXPORT zlink_config_result_t zlink_msg_init_size (zlink_msg_t *msg_, size_t size_);

/**
 * @brief Initialize a message from an external data buffer (zero-copy).
 * @param msg_   Message object.
 * @param data_  External data buffer.
 * @param size_  Data size in bytes.
 * @param ffn_   Callback invoked when the last owning message releases the
 *               buffer. May be NULL.
 * @param hint_  User data passed to @p ffn_.
 *
 * If @p ffn_ is NULL, the message keeps a borrowed reference to @p data_ and
 * never frees it. Such messages report as shared because the storage is not
 * uniquely owned by the message object.
 */
ZLINK_EXPORT zlink_config_result_t zlink_msg_init_data (
  zlink_msg_t *msg_, void *data_, size_t size_, zlink_free_fn *ffn_, void *hint_);

/**
 * @brief Release this message object's ownership of its content.
 *
 * The message becomes invalid after this call. For reference-counted message
 * storage, the underlying buffer is released only when the last owning message
 * is closed or consumed by send.
 */
ZLINK_EXPORT zlink_config_result_t zlink_msg_close (zlink_msg_t *msg_);

/**
 * @brief Move message content from src_ to dest_ without copying payload.
 *
 * Ownership is transferred to @p dest_. The source becomes an empty message.
 * Existing shared/reference-counted state moves with the message; move does
 * not increment any reference count.
 */
ZLINK_EXPORT zlink_config_result_t zlink_msg_move (zlink_msg_t *dest_, zlink_msg_t *src_);

/**
 * @brief Copy a message from src_ to dest_.
 *
 * Small inline messages are copied by value. Large or externally stored
 * messages share the same underlying storage and are tracked internally by
 * reference count rather than duplicating the payload buffer.
 */
ZLINK_EXPORT zlink_config_result_t zlink_msg_copy (zlink_msg_t *dest_, zlink_msg_t *src_);

/**
 * @brief Adopt ownership from src_ into dest_ without an extra init+move step.
 *
 * This is intended for bindings that already have storage for @p dest_ and
 * need to take ownership of a freshly received native message efficiently.
 * Unlike @ref zlink_msg_move, @p dest_ must not currently own an initialized
 * message.
 *
 * On success, @p dest_ owns the original content and @p src_ becomes an empty
 * initialized message.
 */
ZLINK_EXPORT zlink_config_result_t zlink_msg_adopt (zlink_msg_t *dest_, zlink_msg_t *src_);

/** @brief Return a pointer to the message data buffer. */
ZLINK_EXPORT void *zlink_msg_data (zlink_msg_t *msg_);

/** @brief Return the message data size in bytes. */
ZLINK_EXPORT size_t zlink_msg_size (const zlink_msg_t *msg_);

/**
 * @brief Return the message storage reference count.
 *
 * Reference-counted large/zero-copy messages return their current internal
 * reference count. Message kinds that are not managed by internal reference
 * counting (for example inline or borrowed-constant storage) return 1.
 */
ZLINK_EXPORT int zlink_msg_refcnt (const zlink_msg_t *msg_, zlink_config_result_t *error_out_);

/** @brief Close all parts in a multipart message array. */
ZLINK_EXPORT void zlink_multipart_close (zlink_msg_t *parts, size_t part_count);

#ifdef __cplusplus
}
#endif

#endif
