#ifndef PERF_ZLINK_PART_HELPERS_HPP
#define PERF_ZLINK_PART_HELPERS_HPP

#include <zlink.h>

#include <cstdlib>

inline zlink_submit_result_t perf_zlink_send_parts (void *socket,
                                                    zlink_msg_t *parts,
                                                    size_t part_count,
                                                    zlink_send_flags_t flags);

inline size_t perf_measurement_part_count ()
{
    const char *const value = std::getenv ("PERF_PART_COUNT");
    return value && value[0] == '1' && value[1] == '\0' ? 1u : 2u;
}

inline zlink_submit_result_t perf_zlink_send_measurement_parts (
  void *socket, zlink_msg_t *payload, zlink_send_flags_t flags)
{
    if (!socket || !payload)
        return ZLINK_SUBMIT_INVALID_ARGUMENT;
    if (perf_measurement_part_count () == 1u)
        return perf_zlink_send_parts (socket, payload, 1u, flags);

    zlink_msg_t empty_part;
    if (zlink_msg_init (&empty_part) != 0)
        return ZLINK_SUBMIT_INTERNAL_ERROR;
    const zlink_submit_result_t payload_rc =
      zlink_send_part (socket, payload, flags, ZLINK_PART_MORE, NULL, NULL);
    if (payload_rc != ZLINK_SUBMIT_OK) {
        zlink_msg_close (&empty_part);
        return payload_rc;
    }
    const zlink_submit_result_t rc =
      zlink_send_part (socket, &empty_part, flags, ZLINK_PART_FINAL, NULL, NULL);
    zlink_msg_close (&empty_part);
    return rc;
}

inline zlink_submit_result_t perf_zlink_send_parts (void *socket,
                                                    zlink_msg_t *parts,
                                                    size_t part_count,
                                                    zlink_send_flags_t flags)
{
    if (!parts || part_count == 0)
        return ZLINK_SUBMIT_INVALID_ARGUMENT;

    for (size_t i = 0; i < part_count; ++i) {
        const zlink_part_flag_t part_flag =
          (i + 1 < part_count) ? ZLINK_PART_MORE : ZLINK_PART_FINAL;
        const zlink_submit_result_t rc =
          zlink_send_part (socket, &parts[i], flags, part_flag, NULL, NULL);
        if (rc != ZLINK_SUBMIT_OK)
            return rc;
    }
    return ZLINK_SUBMIT_OK;
}

inline zlink_submit_result_t perf_zlink_send_rid_parts (void *socket,
                                                        const zlink_routing_id_t *target_rid,
                                                        zlink_msg_t *parts,
                                                        size_t part_count,
                                                        zlink_send_flags_t flags)
{
    if (!parts || part_count == 0)
        return ZLINK_SUBMIT_INVALID_ARGUMENT;

    for (size_t i = 0; i < part_count; ++i) {
        const zlink_part_flag_t part_flag =
          (i + 1 < part_count) ? ZLINK_PART_MORE : ZLINK_PART_FINAL;
        const zlink_submit_result_t rc =
          zlink_send_part_rid (socket, target_rid, &parts[i], flags, part_flag, NULL, NULL);
        if (rc != ZLINK_SUBMIT_OK)
            return rc;
    }
    return ZLINK_SUBMIT_OK;
}

inline zlink_submit_result_t perf_zlink_send_rid_measurement_parts (
  void *socket, const zlink_routing_id_t *target_rid, zlink_msg_t *payload,
  zlink_send_flags_t flags)
{
    if (!socket || !target_rid || !payload)
        return ZLINK_SUBMIT_INVALID_ARGUMENT;
    if (perf_measurement_part_count () == 1u)
        return perf_zlink_send_rid_parts (socket, target_rid, payload, 1u, flags);

    zlink_msg_t empty_part;
    if (zlink_msg_init (&empty_part) != 0)
        return ZLINK_SUBMIT_INTERNAL_ERROR;
    const zlink_submit_result_t payload_rc =
      zlink_send_part_rid (socket, target_rid, payload, flags, ZLINK_PART_MORE, NULL, NULL);
    if (payload_rc != ZLINK_SUBMIT_OK) {
        zlink_msg_close (&empty_part);
        return payload_rc;
    }
    const zlink_submit_result_t rc = zlink_send_part_rid (
      socket, target_rid, &empty_part, flags, ZLINK_PART_FINAL, NULL, NULL);
    zlink_msg_close (&empty_part);
    return rc;
}

inline zlink_submit_result_t perf_zlink_publish_parts (void *subject,
                                                       const char *topic_id,
                                                       zlink_msg_t *parts,
                                                       size_t part_count,
                                                       zlink_send_flags_t flags)
{
    if (!parts || part_count == 0)
        return ZLINK_SUBMIT_INVALID_ARGUMENT;

    for (size_t i = 0; i < part_count; ++i) {
        const zlink_part_flag_t part_flag =
          (i + 1 < part_count) ? ZLINK_PART_MORE : ZLINK_PART_FINAL;
        const zlink_submit_result_t rc =
          zlink_publish_part (subject, topic_id, &parts[i], flags, part_flag);
        if (rc != ZLINK_SUBMIT_OK)
            return rc;
    }
    return ZLINK_SUBMIT_OK;
}

inline zlink_submit_result_t perf_zlink_publish_measurement_parts (
  void *subject, const char *topic_id, zlink_msg_t *payload,
  zlink_send_flags_t flags)
{
    if (!subject || !topic_id || !payload)
        return ZLINK_SUBMIT_INVALID_ARGUMENT;
    if (perf_measurement_part_count () == 1u)
        return perf_zlink_publish_parts (subject, topic_id, payload, 1u, flags);

    zlink_msg_t empty_part;
    if (zlink_msg_init (&empty_part) != 0)
        return ZLINK_SUBMIT_INTERNAL_ERROR;
    const zlink_submit_result_t payload_rc =
      zlink_publish_part (subject, topic_id, payload, flags, ZLINK_PART_MORE);
    if (payload_rc != ZLINK_SUBMIT_OK) {
        zlink_msg_close (&empty_part);
        return payload_rc;
    }
    const zlink_submit_result_t rc =
      zlink_publish_part (subject, topic_id, &empty_part, flags, ZLINK_PART_FINAL);
    zlink_msg_close (&empty_part);
    return rc;
}

inline zlink_submit_result_t perf_zlink_dealer_request_measurement_part (
  void *dealer, zlink_msg_t *payload, zlink_send_flags_t flags,
  uint32_t timeout_ms, void *user_context, zlink_completion_id_t *completion_id_out)
{
    if (!dealer || !payload)
        return ZLINK_SUBMIT_INVALID_ARGUMENT;
    if (perf_measurement_part_count () == 1u)
        return zlink_request_part (dealer, NULL, payload, flags, ZLINK_PART_FINAL,
                                   timeout_ms, user_context, completion_id_out);
    zlink_msg_t empty_part;
    if (zlink_msg_init (&empty_part) != 0)
        return ZLINK_SUBMIT_INTERNAL_ERROR;
    const zlink_submit_result_t payload_rc = zlink_request_part (
      dealer, NULL, payload, flags, ZLINK_PART_MORE, 0, NULL, NULL);
    if (payload_rc != ZLINK_SUBMIT_OK) {
        zlink_msg_close (&empty_part);
        return payload_rc;
    }
    const zlink_submit_result_t rc = zlink_request_part (
      dealer, NULL, &empty_part, flags, ZLINK_PART_FINAL, timeout_ms,
      user_context, completion_id_out);
    zlink_msg_close (&empty_part);
    return rc;
}

inline zlink_submit_result_t perf_zlink_router_request_measurement_part (
  void *router, const zlink_routing_id_t *peer_rid, zlink_msg_t *payload,
  zlink_send_flags_t flags, uint32_t timeout_ms, void *user_context,
  zlink_completion_id_t *completion_id_out)
{
    if (!router || !peer_rid || !payload)
        return ZLINK_SUBMIT_INVALID_ARGUMENT;
    if (perf_measurement_part_count () == 1u)
        return zlink_request_part (router, peer_rid, payload, flags,
                                   ZLINK_PART_FINAL, timeout_ms, user_context,
                                   completion_id_out);
    zlink_msg_t empty_part;
    if (zlink_msg_init (&empty_part) != 0)
        return ZLINK_SUBMIT_INTERNAL_ERROR;
    const zlink_submit_result_t payload_rc = zlink_request_part (
      router, peer_rid, payload, flags, ZLINK_PART_MORE, 0, NULL, NULL);
    if (payload_rc != ZLINK_SUBMIT_OK) {
        zlink_msg_close (&empty_part);
        return payload_rc;
    }
    const zlink_submit_result_t rc = zlink_request_part (
      router, peer_rid, &empty_part, flags, ZLINK_PART_FINAL, timeout_ms,
      user_context, completion_id_out);
    zlink_msg_close (&empty_part);
    return rc;
}

inline zlink_submit_result_t perf_zlink_router_reply_measurement_part (
  void *router, const zlink_routing_id_t *peer_rid, zlink_reply_token_t reply_token,
  zlink_msg_t *payload)
{
    if (!router || !peer_rid || !payload)
        return ZLINK_SUBMIT_INVALID_ARGUMENT;
    if (perf_measurement_part_count () == 1u)
        return zlink_reply_part (router, peer_rid, reply_token, payload,
                                 ZLINK_PART_FINAL);
    zlink_msg_t empty_part;
    if (zlink_msg_init (&empty_part) != 0)
        return ZLINK_SUBMIT_INTERNAL_ERROR;
    const zlink_submit_result_t payload_rc = zlink_reply_part (
      router, peer_rid, reply_token, payload, ZLINK_PART_MORE);
    if (payload_rc != ZLINK_SUBMIT_OK) {
        zlink_msg_close (&empty_part);
        return payload_rc;
    }
    const zlink_submit_result_t rc = zlink_reply_part (
      router, peer_rid, reply_token, &empty_part, ZLINK_PART_FINAL);
    zlink_msg_close (&empty_part);
    return rc;
}

typedef zlink_recv_result_t (*perf_zlink_recv_next_fn) (void *socket,
                                                        zlink_msg_t *part_out,
                                                        zlink_part_flag_t *has_more_out);

inline zlink_recv_result_t perf_zlink_recv_next_plain (
  void *socket, zlink_msg_t *part_out, zlink_part_flag_t *has_more_out);
inline zlink_recv_result_t perf_zlink_recv_next_router (
  void *socket, zlink_msg_t *part_out, zlink_part_flag_t *has_more_out);
inline zlink_recv_result_t perf_zlink_recv_next_subscribe (
  void *socket, zlink_msg_t *part_out, zlink_part_flag_t *has_more_out);

inline bool perf_zlink_recv_measurement_tail (void *socket,
                                              zlink_part_flag_t has_more,
                                              zlink_recv_flags_t flags,
                                              perf_zlink_recv_next_fn recv_next)
{
    if (perf_measurement_part_count () == 1u)
        return has_more == ZLINK_PART_FINAL;
    if (has_more != ZLINK_PART_MORE || !recv_next)
        return false;

    zlink_msg_t empty_part;
    zlink_part_flag_t tail_more = ZLINK_PART_FINAL;
    if (zlink_msg_init (&empty_part) != 0)
        return false;
    // The next-part functions are non-blocking. The first part was admitted
    // as one multipart record, so its trailing empty frame is already ready.
    (void) flags;
    const zlink_recv_result_t rc = recv_next (socket, &empty_part, &tail_more);
    const bool ok = rc == ZLINK_RECV_OK && tail_more == ZLINK_PART_FINAL
                    && zlink_msg_size (&empty_part) == 0;
    zlink_msg_close (&empty_part);
    return ok;
}

inline zlink_recv_result_t perf_zlink_collect_recv_parts (void *socket,
                                                          zlink_msg_t first,
                                                          zlink_part_flag_t has_more,
                                                          zlink_msg_t **parts_out,
                                                          size_t *part_count_out,
                                                          perf_zlink_recv_next_fn recv_next)
{
    size_t count = 1;
    size_t capacity = has_more ? 4u : 1u;
    zlink_msg_t *parts = static_cast<zlink_msg_t *> (std::malloc (capacity * sizeof (*parts)));
    if (!parts) {
        zlink_msg_close (&first);
        return ZLINK_RECV_INTERNAL_ERROR;
    }

    parts[0] = first;
    while (has_more) {
        zlink_msg_t next;
        zlink_part_flag_t next_has_more = ZLINK_PART_FINAL;
        if (zlink_msg_init (&next) != 0) {
            zlink_multipart_close (parts, count);
            std::free (parts);
            return ZLINK_RECV_INTERNAL_ERROR;
        }

        const zlink_recv_result_t rc = recv_next (socket, &next, &next_has_more);
        if (rc != ZLINK_RECV_OK) {
            zlink_msg_close (&next);
            zlink_multipart_close (parts, count);
            std::free (parts);
            return rc;
        }

        if (count == capacity) {
            const size_t new_capacity = capacity * 2u;
            zlink_msg_t *grown =
              static_cast<zlink_msg_t *> (std::realloc (parts, new_capacity * sizeof (*parts)));
            if (!grown) {
                zlink_msg_close (&next);
                zlink_multipart_close (parts, count);
                std::free (parts);
                return ZLINK_RECV_INTERNAL_ERROR;
            }
            parts = grown;
            capacity = new_capacity;
        }

        parts[count++] = next;
        has_more = next_has_more;
    }

    *parts_out = parts;
    *part_count_out = count;
    return ZLINK_RECV_OK;
}

inline zlink_recv_result_t
perf_zlink_recv_next_plain (void *socket, zlink_msg_t *part_out, zlink_part_flag_t *has_more_out)
{
    const zlink_routing_id_t *source_rid = NULL;
    return zlink_recv_part (socket, &source_rid, part_out, has_more_out, ZLINK_RECV_FLAGS_DONTWAIT);
}

inline zlink_recv_result_t
perf_zlink_recv_next_router (void *socket, zlink_msg_t *part_out, zlink_part_flag_t *has_more_out)
{
    const zlink_routing_id_t *source_node_rid = NULL;
    zlink_reply_token_t reply_token = 0;
    return zlink_router_recv_part (socket, &source_node_rid, &reply_token, part_out, has_more_out,
                                   ZLINK_RECV_FLAGS_DONTWAIT);
}

inline zlink_recv_result_t perf_zlink_recv_next_subscribe (void *socket,
                                                           zlink_msg_t *part_out,
                                                           zlink_part_flag_t *has_more_out)
{
    const zlink_routing_id_t *source_rid = NULL;
    char topic[256];
    size_t topic_len = sizeof (topic);
    return zlink_subscribe_part (socket, &source_rid, topic, sizeof (topic), &topic_len, part_out,
                                 has_more_out, ZLINK_RECV_FLAGS_DONTWAIT);
}

inline zlink_recv_result_t perf_zlink_recv_parts (void *socket,
                                                  const zlink_routing_id_t **source_rid_out,
                                                  zlink_msg_t **parts_out,
                                                  size_t *part_count_out,
                                                  zlink_recv_flags_t flags)
{
    zlink_msg_t first;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    if (!parts_out || !part_count_out)
        return ZLINK_RECV_INTERNAL_ERROR;
    *parts_out = NULL;
    *part_count_out = 0;
    if (zlink_msg_init (&first) != 0)
        return ZLINK_RECV_INTERNAL_ERROR;

    const zlink_recv_result_t rc =
      zlink_recv_part (socket, source_rid_out, &first, &has_more, flags);
    if (rc != ZLINK_RECV_OK) {
        zlink_msg_close (&first);
        return rc;
    }

    return perf_zlink_collect_recv_parts (socket, first, has_more, parts_out, part_count_out,
                                          perf_zlink_recv_next_plain);
}

inline zlink_recv_result_t
perf_zlink_router_recv_parts (void *router,
                              zlink_routing_id_t *source_node_rid_out,
                              bool *has_source_node_rid_out,
                              zlink_reply_token_t *reply_token_out,
                              zlink_msg_t **parts_out,
                              size_t *part_count_out,
                              zlink_recv_flags_t flags)
{
    zlink_msg_t first;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    if (!source_node_rid_out || !has_source_node_rid_out || !parts_out
        || !part_count_out)
        return ZLINK_RECV_INTERNAL_ERROR;
    std::memset (source_node_rid_out, 0, sizeof (*source_node_rid_out));
    *has_source_node_rid_out = false;
    *parts_out = NULL;
    *part_count_out = 0;
    if (zlink_msg_init (&first) != 0)
        return ZLINK_RECV_INTERNAL_ERROR;

    const zlink_routing_id_t *borrowed_source_rid = NULL;
    const zlink_recv_result_t rc = zlink_router_recv_part (
      router, &borrowed_source_rid, reply_token_out, &first, &has_more, flags);
    if (rc != ZLINK_RECV_OK) {
        zlink_msg_close (&first);
        return rc;
    }

    // A routing id is a socket-owned view invalidated by the next data recv.
    // Preserve it before collecting the remaining multipart frames.
    if (borrowed_source_rid) {
        *source_node_rid_out = *borrowed_source_rid;
        *has_source_node_rid_out = true;
    }

    return perf_zlink_collect_recv_parts (router, first, has_more, parts_out, part_count_out,
                                          perf_zlink_recv_next_router);
}

inline zlink_recv_result_t perf_zlink_subscribe_parts (void *subject,
                                                       const zlink_routing_id_t **source_rid_out,
                                                       zlink_msg_t **parts_out,
                                                       size_t *part_count_out,
                                                       char *topic_id_out,
                                                       size_t *topic_id_len_out,
                                                       zlink_recv_flags_t flags)
{
    zlink_msg_t first;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    if (!parts_out || !part_count_out || !topic_id_len_out)
        return ZLINK_RECV_INTERNAL_ERROR;
    *parts_out = NULL;
    *part_count_out = 0;
    if (zlink_msg_init (&first) != 0)
        return ZLINK_RECV_INTERNAL_ERROR;

    const size_t topic_capacity = *topic_id_len_out;
    const zlink_recv_result_t rc =
      zlink_subscribe_part (subject, source_rid_out, topic_id_out, topic_capacity, topic_id_len_out,
                            &first, &has_more, flags);
    if (rc != ZLINK_RECV_OK) {
        zlink_msg_close (&first);
        return rc;
    }

    return perf_zlink_collect_recv_parts (subject, first, has_more, parts_out, part_count_out,
                                          perf_zlink_recv_next_subscribe);
}

#endif
