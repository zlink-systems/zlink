#include <stdint.h>
#include <errno.h>

#include "zlink.h"

#ifdef __cplusplus
#define ZLINK_JAVA_EXPORT extern "C"
#else
#define ZLINK_JAVA_EXPORT
#endif
ZLINK_JAVA_EXPORT uintptr_t zlink_java_msg_data_addr (zlink_msg_t *msg)
{
    return (uintptr_t) zlink_msg_data (msg);
}

ZLINK_JAVA_EXPORT uintptr_t
zlink_java_msg_init_size_data_addr (zlink_msg_t *msg, size_t size)
{
    if (!msg || size == 0) {
        errno = EINVAL;
        return 0;
    }
    if (zlink_msg_init_size (msg, size) != 0)
        return 0;
    return (uintptr_t) zlink_msg_data (msg);
}

ZLINK_JAVA_EXPORT int
zlink_java_snapshot_message_pair (zlink_msg_t *source, zlink_msg_t *snapshot)
{
    if (!source || !snapshot) {
        errno = EINVAL;
        return -1;
    }
    zlink_msg_init (&snapshot[0]);
    zlink_msg_init (&snapshot[1]);
    if (zlink_msg_copy (&snapshot[0], &source[0]) != 0
        || zlink_msg_copy (&snapshot[1], &source[1]) != 0) {
        const int saved_errno = errno;
        zlink_msg_close (&snapshot[0]);
        zlink_msg_close (&snapshot[1]);
        errno = saved_errno;
        return -1;
    }
    return 0;
}

static uint64_t reply_outcome (uint32_t consumed, int result)
{
    return (static_cast<uint64_t> (consumed) << 32)
           | static_cast<uint32_t> (result);
}

ZLINK_JAVA_EXPORT uint64_t
zlink_java_router_reply_two (
  void *router,
  const zlink_routing_id_t *routing_id,
  uint64_t request_sequence,
  zlink_msg_t *first,
  zlink_msg_t *second)
{
    if (!router || !routing_id || !first || !second) {
        errno = EINVAL;
        return reply_outcome (0, -1);
    }

    int result;
    do {
        result = zlink_router_reply_part (router, routing_id,
          request_sequence, first, ZLINK_PART_MORE);
    } while (result != 0 && errno == EINTR);
    if (result != 0)
        return reply_outcome (0, result);

    do {
        result = zlink_router_reply_part (router, routing_id,
          request_sequence, second, ZLINK_PART_FINAL);
    } while (result != 0 && errno == EINTR);
    return reply_outcome (result == 0 ? 2 : 1, result);
}

ZLINK_JAVA_EXPORT uint64_t
zlink_java_dealer_request_two (
  void *dealer,
  zlink_msg_t *first_source,
  zlink_msg_t *second_source,
  zlink_send_flags_t flags,
  uint32_t timeout_ms,
  zlink_reply_handler_fn handler,
  uintptr_t userdata)
{
    if (!dealer || !first_source || !second_source) {
        errno = EINVAL;
        return reply_outcome (0, ZLINK_SUBMIT_INVALID_ARGUMENT);
    }

    const zlink_submit_result_t first_result = zlink_dealer_request_part (
      dealer, first_source, flags, ZLINK_PART_MORE, 0, NULL, NULL);
    if (first_result != ZLINK_SUBMIT_OK)
        return reply_outcome (0, first_result);
    const zlink_submit_result_t result = zlink_dealer_request_part (
      dealer, second_source, flags, ZLINK_PART_FINAL, timeout_ms, handler,
      reinterpret_cast<void *> (userdata));
    return reply_outcome (result == ZLINK_SUBMIT_OK ? 2 : 1, result);
}

ZLINK_JAVA_EXPORT uint64_t
zlink_java_dealer_request_transport_pair_two (
  void *dealer,
  const zlink_routed_submit_target_t *target,
  zlink_msg_t *first_source,
  zlink_msg_t *second_source,
  zlink_send_flags_t flags,
  uint32_t timeout_ms,
  zlink_reply_handler_fn handler,
  uintptr_t userdata)
{
    if (!dealer || !target || !first_source || !second_source) {
        errno = EINVAL;
        return reply_outcome (0, ZLINK_SUBMIT_INVALID_ARGUMENT);
    }

    const zlink_submit_result_t first_result =
      zlink_dealer_request_transport_pair_part (
        dealer, target, first_source, flags, ZLINK_PART_MORE, 0, NULL, NULL);
    if (first_result != ZLINK_SUBMIT_OK)
        return reply_outcome (0, first_result);
    const zlink_submit_result_t result =
      zlink_dealer_request_transport_pair_part (
      dealer, target, second_source, flags, ZLINK_PART_FINAL, timeout_ms, handler,
      reinterpret_cast<void *> (userdata));
    return reply_outcome (result == ZLINK_SUBMIT_OK ? 2 : 1, result);
}

ZLINK_JAVA_EXPORT uint64_t
zlink_java_router_request_transport_pair_two (
  void *router,
  const zlink_routed_submit_target_t *target,
  zlink_msg_t *first_source,
  zlink_msg_t *second_source,
  zlink_send_flags_t flags,
  uint32_t timeout_ms,
  zlink_reply_handler_fn handler,
  uintptr_t userdata)
{
    if (!router || !target || !first_source || !second_source) {
        errno = EINVAL;
        return reply_outcome (0, ZLINK_SUBMIT_INVALID_ARGUMENT);
    }

    const zlink_submit_result_t first_result =
      zlink_router_request_transport_pair_part (
        router, &target->peer_rid, target->transport_pair_id,
        target->transport_pair_generation, first_source, flags, ZLINK_PART_MORE, 0,
        NULL, NULL);
    if (first_result != ZLINK_SUBMIT_OK)
        return reply_outcome (0, first_result);
    const zlink_submit_result_t result =
      zlink_router_request_transport_pair_part (
      router, &target->peer_rid, target->transport_pair_id,
      target->transport_pair_generation, second_source, flags, ZLINK_PART_FINAL,
      timeout_ms, handler, reinterpret_cast<void *> (userdata));
    return reply_outcome (result == ZLINK_SUBMIT_OK ? 2 : 1, result);
}
