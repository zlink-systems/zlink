#ifndef PERF_ZLINK_PART_HELPERS_HPP
#define PERF_ZLINK_PART_HELPERS_HPP

#include <zlink.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

inline zlink_submit_result_t perf_zlink_send_parts (void *socket,
                                                    zlink_msg_t *parts,
                                                    size_t part_count,
                                                    zlink_send_flags_t flags)
{
    if (!parts || part_count == 0)
        return ZLINK_SUBMIT_INVALID_ARGUMENT;
    return zlink_send (socket, parts, part_count, flags, NULL, NULL);
}

inline size_t perf_measurement_part_count ()
{
    // Read once per process: PERF_PART_COUNT is a launch-time configuration
    // knob (bindings/c/perf/run_benchmarks*.sh), never mutated after the
    // measurement harness starts, so a per-call getenv only adds hot-path
    // noise to every send/recv (see G-A/G-5 profiling).
    static const size_t value = [] () -> size_t {
        const char *const raw = std::getenv ("PERF_PART_COUNT");
        return raw && raw[0] == '1' && raw[1] == '\0' ? 1u : 2u;
    } ();
    return value;
}

template <typename SubmitFn>
inline zlink_submit_result_t perf_zlink_submit_measurement_parts (
  zlink_msg_t *payload, SubmitFn submit)
{
    if (perf_measurement_part_count () == 1u)
        return submit (payload, 1u);

    zlink_msg_t parts[2];
    if (zlink_msg_init (&parts[0]) != 0)
        return ZLINK_SUBMIT_INTERNAL_ERROR;
    if (zlink_msg_init (&parts[1]) != 0) {
        zlink_msg_close (&parts[0]);
        return ZLINK_SUBMIT_INTERNAL_ERROR;
    }
    if (zlink_msg_move (&parts[0], payload) != 0) {
        zlink_multipart_close (parts, 2u);
        return ZLINK_SUBMIT_INTERNAL_ERROR;
    }

    const zlink_submit_result_t rc = submit (parts, 2u);
    zlink_multipart_close (parts, 2u);
    return rc;
}

inline zlink_submit_result_t perf_zlink_send_measurement_parts (
  void *socket, zlink_msg_t *payload, zlink_send_flags_t flags)
{
    if (!socket || !payload)
        return ZLINK_SUBMIT_INVALID_ARGUMENT;
    return perf_zlink_submit_measurement_parts (
      payload, [&] (zlink_msg_t *parts, size_t part_count) {
          return zlink_send (socket, parts, part_count, flags, NULL, NULL);
      });
}

inline zlink_submit_result_t perf_zlink_send_rid_parts (void *socket,
                                                        const zlink_routing_id_t *target_rid,
                                                        zlink_msg_t *parts,
                                                        size_t part_count,
                                                        zlink_send_flags_t flags)
{
    if (!parts || part_count == 0)
        return ZLINK_SUBMIT_INVALID_ARGUMENT;
    return zlink_send_rid (socket, target_rid, parts, part_count, flags, NULL, NULL);
}

inline zlink_submit_result_t perf_zlink_send_rid_measurement_parts (
  void *socket, const zlink_routing_id_t *target_rid, zlink_msg_t *payload,
  zlink_send_flags_t flags)
{
    if (!socket || !target_rid || !payload)
        return ZLINK_SUBMIT_INVALID_ARGUMENT;
    return perf_zlink_submit_measurement_parts (
      payload, [&] (zlink_msg_t *parts, size_t part_count) {
          return zlink_send_rid (
            socket, target_rid, parts, part_count, flags, NULL, NULL);
      });
}

inline zlink_submit_result_t perf_zlink_publish_parts (void *subject,
                                                       const char *topic_id,
                                                       zlink_msg_t *parts,
                                                       size_t part_count,
                                                       zlink_send_flags_t flags)
{
    if (!parts || part_count == 0)
        return ZLINK_SUBMIT_INVALID_ARGUMENT;
    return zlink_publish (subject, topic_id, parts, part_count, flags);
}

inline zlink_submit_result_t perf_zlink_publish_measurement_parts (
  void *subject, const char *topic_id, zlink_msg_t *payload,
  zlink_send_flags_t flags)
{
    if (!subject || !topic_id || !payload)
        return ZLINK_SUBMIT_INVALID_ARGUMENT;
    return perf_zlink_submit_measurement_parts (
      payload, [&] (zlink_msg_t *parts, size_t part_count) {
          return zlink_publish (subject, topic_id, parts, part_count, flags);
      });
}

inline zlink_submit_result_t perf_zlink_dealer_request_measurement_part (
  void *dealer, zlink_msg_t *payload, zlink_send_flags_t flags,
  uint32_t timeout_ms, void *user_context, zlink_completion_id_t *completion_id_out)
{
    if (!dealer || !payload)
        return ZLINK_SUBMIT_INVALID_ARGUMENT;
    return perf_zlink_submit_measurement_parts (
      payload, [&] (zlink_msg_t *parts, size_t part_count) {
          return zlink_request (dealer, NULL, parts, part_count, flags, timeout_ms,
                                user_context, completion_id_out);
      });
}

inline zlink_submit_result_t perf_zlink_router_request_measurement_part (
  void *router, const zlink_routing_id_t *peer_rid, zlink_msg_t *payload,
  zlink_send_flags_t flags, uint32_t timeout_ms, void *user_context,
  zlink_completion_id_t *completion_id_out)
{
    if (!router || !peer_rid || !payload)
        return ZLINK_SUBMIT_INVALID_ARGUMENT;
    return perf_zlink_submit_measurement_parts (
      payload, [&] (zlink_msg_t *parts, size_t part_count) {
          return zlink_request (router, peer_rid, parts, part_count, flags, timeout_ms,
                                user_context, completion_id_out);
      });
}

inline zlink_submit_result_t perf_zlink_router_reply_measurement_part (
  void *router, const zlink_routing_id_t *peer_rid, zlink_reply_token_t reply_token,
  zlink_msg_t *payload)
{
    if (!router || !peer_rid || !payload)
        return ZLINK_SUBMIT_INVALID_ARGUMENT;
    return perf_zlink_submit_measurement_parts (
      payload, [&] (zlink_msg_t *parts, size_t part_count) {
          return zlink_reply (router, peer_rid, reply_token, parts, part_count);
      });
}

inline bool perf_zlink_measurement_parts_valid (zlink_msg_t *parts, size_t part_count)
{
    const size_t expected = perf_measurement_part_count ();
    return parts && part_count == expected
           && (expected == 1u || zlink_msg_size (&parts[1]) == 0u);
}

template <typename RecvFn>
inline zlink_recv_result_t perf_zlink_recv_allocated (zlink_msg_t **parts_out,
                                                      size_t *part_count_out,
                                                      RecvFn recv)
{
    if (!parts_out || !part_count_out)
        return ZLINK_RECV_INTERNAL_ERROR;
    *parts_out = NULL;
    *part_count_out = 0;

    size_t capacity = 4u;
    zlink_msg_t *parts =
      static_cast<zlink_msg_t *> (std::malloc (capacity * sizeof (*parts)));
    if (!parts)
        return ZLINK_RECV_INTERNAL_ERROR;

    size_t part_count = 0;
    for (;;) {
        const zlink_recv_result_t rc = recv (parts, capacity, &part_count);
        if (rc != ZLINK_RECV_BUFFER_TOO_SMALL) {
            if (rc == ZLINK_RECV_OK) {
                if (part_count == 0u || part_count > capacity) {
                    std::free (parts);
                    errno = EPROTO;
                    return ZLINK_RECV_INTERNAL_ERROR;
                }
                *parts_out = parts;
                *part_count_out = part_count;
            } else {
                std::free (parts);
            }
            return rc;
        }
        if (part_count <= capacity) {
            std::free (parts);
            errno = EPROTO;
            return ZLINK_RECV_INTERNAL_ERROR;
        }

        zlink_msg_t *grown = static_cast<zlink_msg_t *> (
          std::realloc (parts, part_count * sizeof (*parts)));
        if (!grown) {
            std::free (parts);
            return ZLINK_RECV_INTERNAL_ERROR;
        }
        parts = grown;
        capacity = part_count;
    }
}

inline zlink_recv_result_t perf_zlink_recv_parts (void *socket,
                                                  const zlink_routing_id_t **source_rid_out,
                                                  zlink_msg_t **parts_out,
                                                  size_t *part_count_out,
                                                  zlink_recv_flags_t flags)
{
    return perf_zlink_recv_allocated (
      parts_out, part_count_out,
      [&] (zlink_msg_t *parts, size_t capacity, size_t *part_count) {
          return zlink_recv (
            socket, source_rid_out, parts, capacity, part_count, flags);
      });
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
    if (!source_node_rid_out || !has_source_node_rid_out)
        return ZLINK_RECV_INTERNAL_ERROR;
    std::memset (source_node_rid_out, 0, sizeof (*source_node_rid_out));
    *has_source_node_rid_out = false;

    const zlink_routing_id_t *borrowed_source_rid = NULL;
    const zlink_recv_result_t rc = perf_zlink_recv_allocated (
      parts_out, part_count_out,
      [&] (zlink_msg_t *parts, size_t capacity, size_t *part_count) {
          return zlink_router_recv (router, &borrowed_source_rid, reply_token_out,
                                    parts, capacity, part_count, flags);
      });
    if (rc == ZLINK_RECV_OK && borrowed_source_rid) {
        *source_node_rid_out = *borrowed_source_rid;
        *has_source_node_rid_out = true;
    }
    return rc;
}

inline zlink_recv_result_t perf_zlink_subscribe_parts (void *subject,
                                                       const zlink_routing_id_t **source_rid_out,
                                                       zlink_msg_t **parts_out,
                                                       size_t *part_count_out,
                                                       char *topic_id_out,
                                                       size_t *topic_id_len_out,
                                                       zlink_recv_flags_t flags)
{
    if (!topic_id_len_out)
        return ZLINK_RECV_INTERNAL_ERROR;
    const size_t topic_capacity = *topic_id_len_out;
    return perf_zlink_recv_allocated (
      parts_out, part_count_out,
      [&] (zlink_msg_t *parts, size_t capacity, size_t *part_count) {
          return zlink_subscribe (subject, source_rid_out, topic_id_out,
                                  topic_capacity, topic_id_len_out, parts,
                                  capacity, part_count, flags);
      });
}

#endif
