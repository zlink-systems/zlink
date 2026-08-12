/* SPDX-License-Identifier: MPL-2.0 */

#include "addon_core_api.h"
#include "addon_core_options.h"
#include "addon_monitor_status_values.h"
#include "addon_message_values.h"
#include "addon_message_parts.h"
#include "addon_request_callbacks.h"
#include "addon_submit_results.h"
#include "addon_tsfn_slots.h"
#include <algorithm>
#include <errno.h>
#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{

static const size_t k_stream_slot_count = 8;
static const size_t k_send_ready_handler_slot_count = 8;
static const size_t k_socket_monitor_handler_slot_count = 8;
static const int32_t k_stream_dispatch_len32be = 1;
struct stream_js_payload_t
{
    stream_js_payload_t () : packet_count (0), body_materialization (0) {}
    ~stream_js_payload_t ()
    {
        if (packet_count > 0)
            close_recv_parts (packets, packet_count);
    }

    std::vector<unsigned char> routing_id;
    zlink_msg_t packets[2];
    size_t packet_count;
    int body_materialization;
};

struct socket_monitor_handler_js_payload_t
{
    zlink_monitor_event_t event;
};

struct completion_control_js_payload_t
{
    std::vector<unsigned char> routing_id;
    std::vector<std::vector<unsigned char>> parts;
};

struct stream_js_state_t
{
    stream_js_state_t () : used (false), socket (NULL), env (NULL), tsfn (NULL),
                           stop_requested (0), body_materialization (0)
    {
    }

    bool used;
    void *socket;
    napi_env env;
    napi_threadsafe_function tsfn;
    std::atomic<int> stop_requested;
    int body_materialization;
};

static std::mutex g_stream_slots_mu;
static stream_js_state_t g_stream_slots[k_stream_slot_count];

struct send_ready_handler_js_state_t
{
    send_ready_handler_js_state_t () : used (false), socket (NULL), env (NULL), tsfn (NULL) {}

    bool used;
    void *socket;
    napi_env env;
    napi_threadsafe_function tsfn;
};

struct socket_monitor_handler_js_state_t
{
    socket_monitor_handler_js_state_t () : used (false), monitor (NULL), env (NULL), tsfn (NULL) {}

    bool used;
    void *monitor;
    napi_env env;
    napi_threadsafe_function tsfn;
};

struct completion_control_handler_js_state_t
{
    completion_control_handler_js_state_t ()
        : socket (NULL), env (NULL), tsfn (NULL)
    {
    }

    void *socket;
    napi_env env;
    napi_threadsafe_function tsfn;
};

static std::mutex g_send_ready_handler_slots_mu;
static send_ready_handler_js_state_t g_send_ready_handler_slots[k_send_ready_handler_slot_count];
static std::mutex g_completion_control_handler_slots_mu;
static std::mutex g_completion_control_handler_registration_mu;
static std::unordered_map<void *, std::vector<completion_control_handler_js_state_t *> >
  g_completion_control_handler_states;
static std::mutex g_socket_monitor_handler_slots_mu;
static socket_monitor_handler_js_state_t
  g_socket_monitor_handler_slots[k_socket_monitor_handler_slot_count];

stream_js_state_t *find_stream_slot_by_socket_unsafe (void *socket)
{
    return find_tsfn_slot_by_subject (g_stream_slots, k_stream_slot_count,
                                      &stream_js_state_t::socket, socket);
}

send_ready_handler_js_state_t *find_send_ready_handler_slot_by_socket_unsafe (void *socket)
{
    return find_tsfn_slot_by_subject (g_send_ready_handler_slots, k_send_ready_handler_slot_count,
                                      &send_ready_handler_js_state_t::socket, socket);
}

socket_monitor_handler_js_state_t *
find_socket_monitor_handler_slot_by_monitor_unsafe (void *monitor)
{
    return find_tsfn_slot_by_subject (g_socket_monitor_handler_slots,
                                      k_socket_monitor_handler_slot_count,
                                      &socket_monitor_handler_js_state_t::monitor, monitor);
}

void reset_stream_slot_unsafe (stream_js_state_t *state)
{
    if (!state)
        return;
    reset_tsfn_slot_base (state);
    state->socket = NULL;
    state->stop_requested.store (0, std::memory_order_release);
    state->body_materialization = 0;
}

void reset_send_ready_handler_slot_unsafe (send_ready_handler_js_state_t *state)
{
    if (!state)
        return;
    reset_tsfn_slot_base (state);
    state->socket = NULL;
}

void reset_socket_monitor_handler_slot_unsafe (socket_monitor_handler_js_state_t *state)
{
    if (!state)
        return;
    reset_tsfn_slot_base (state);
    state->monitor = NULL;
}

zlink_socket_type_t translate_socket_type (int32_t type)
{
    switch (type) {
        case ZLINK_SOCKET_PAIR:
            return ZLINK_SOCKET_PAIR;
        case ZLINK_SOCKET_PUB:
            return ZLINK_SOCKET_PUB;
        case ZLINK_SOCKET_SUB:
            return ZLINK_SOCKET_SUB;
        case ZLINK_SOCKET_DEALER:
            return ZLINK_SOCKET_DEALER;
        case ZLINK_SOCKET_ROUTER:
            return ZLINK_SOCKET_ROUTER;
        case ZLINK_SOCKET_XPUB:
            return ZLINK_SOCKET_XPUB;
        case ZLINK_SOCKET_XSUB:
            return ZLINK_SOCKET_XSUB;
        case ZLINK_SOCKET_STREAM:
            return ZLINK_SOCKET_STREAM;
        default:
            return static_cast<zlink_socket_type_t> (type);
    }
}

bool init_msg_from_bytes (zlink_msg_t *msg, const void *data, size_t len)
{
    if (zlink_msg_init_size (msg, len) != 0)
        return false;
    if (len > 0 && data)
        memcpy (zlink_msg_data (msg), data, len);
    return true;
}

int subscribe_parts (void *sock,
                     zlink_routing_id_t *routing_id,
                     char *topic,
                     size_t topic_capacity,
                     size_t *topic_len,
                     std::vector<zlink_msg_t> *parts,
                     int32_t flags)
{
    const zlink_routing_id_t *source_rid = NULL;
    zlink_msg_t first_part;
    if (zlink_msg_init (&first_part) != 0)
        return ZLINK_RECV_INTERNAL_ERROR;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;

    copy_routing_id (routing_id, NULL);
    if (parts)
        parts->clear ();

    int rc = zlink_subscribe_part (sock, &source_rid, topic, topic_capacity, topic_len, &first_part,
                                   &has_more, static_cast<zlink_recv_flags_t> (flags));
    if (rc != ZLINK_RECV_OK) {
        zlink_msg_close (&first_part);
        return rc;
    }

    copy_routing_id (routing_id, source_rid);
    if (!parts) {
        zlink_msg_close (&first_part);
        errno = EFAULT;
        return ZLINK_RECV_INTERNAL_ERROR;
    }
    parts->clear ();
    if (!append_msg_move (parts, &first_part)) {
        zlink_msg_close (&first_part);
        errno = ENOMEM;
        return ZLINK_RECV_INTERNAL_ERROR;
    }
    while (has_more) {
        const zlink_routing_id_t *next_source_rid = NULL;
        char next_topic[256];
        size_t next_topic_len = 0;
        zlink_msg_t next_part;
        if (zlink_msg_init (&next_part) != 0) {
            close_msg_vector (*parts);
            parts->clear ();
            return ZLINK_RECV_INTERNAL_ERROR;
        }
        zlink_part_flag_t more = ZLINK_PART_FINAL;
        rc = zlink_subscribe_part (sock, &next_source_rid, next_topic, sizeof (next_topic),
                                   &next_topic_len, &next_part, &more, ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc != ZLINK_RECV_OK) {
            zlink_msg_close (&next_part);
            close_msg_vector (*parts);
            parts->clear ();
            return rc;
        }
        if (!append_msg_move (parts, &next_part)) {
            zlink_msg_close (&next_part);
            close_msg_vector (*parts);
            parts->clear ();
            errno = ENOMEM;
            return ZLINK_RECV_INTERNAL_ERROR;
        }
        has_more = more;
    }
    return ZLINK_RECV_OK;
}

int router_recv_parts (void *router,
                       zlink_routing_id_t *peer_rid,
                       uint64_t *request_seq,
                       std::vector<zlink_msg_t> *parts,
                       int32_t flags,
                       uint64_t *transport_pair_id = NULL,
                       uint64_t *transport_pair_generation = NULL)
{
    const zlink_routing_id_t *peer_rid_ptr = NULL;
    zlink_msg_t first_part;
    if (zlink_msg_init (&first_part) != 0)
        return ZLINK_RECV_INTERNAL_ERROR;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;

    copy_routing_id (peer_rid, NULL);
    if (request_seq)
        *request_seq = 0;
    if (transport_pair_id)
        *transport_pair_id = 0;
    if (transport_pair_generation)
        *transport_pair_generation = 0;
    if (parts)
        parts->clear ();

    int rc = (transport_pair_id && transport_pair_generation)
      ? zlink_router_recv_part_v2 (router, &peer_rid_ptr, request_seq,
                                   transport_pair_id, transport_pair_generation,
                                   &first_part, &has_more,
                                   static_cast<zlink_recv_flags_t> (flags))
      : zlink_router_recv_part (router, &peer_rid_ptr, request_seq, &first_part,
                                &has_more, static_cast<zlink_recv_flags_t> (flags));
    if (rc != ZLINK_RECV_OK) {
        zlink_msg_close (&first_part);
        return rc;
    }

    copy_routing_id (peer_rid, peer_rid_ptr);
    if (!parts) {
        zlink_msg_close (&first_part);
        errno = EFAULT;
        return ZLINK_RECV_INTERNAL_ERROR;
    }
    parts->clear ();
    if (!append_msg_move (parts, &first_part)) {
        zlink_msg_close (&first_part);
        errno = ENOMEM;
        return ZLINK_RECV_INTERNAL_ERROR;
    }
    while (has_more) {
        const zlink_routing_id_t *next_peer_rid = NULL;
        uint64_t next_request_seq = 0;
        zlink_msg_t next_part;
        if (zlink_msg_init (&next_part) != 0) {
            close_msg_vector (*parts);
            parts->clear ();
            return ZLINK_RECV_INTERNAL_ERROR;
        }
        zlink_part_flag_t more = ZLINK_PART_FINAL;
        rc = zlink_router_recv_part (router, &next_peer_rid, &next_request_seq,
                                     &next_part, &more, ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc != ZLINK_RECV_OK) {
            zlink_msg_close (&next_part);
            close_msg_vector (*parts);
            parts->clear ();
            return rc;
        }
        if (!append_msg_move (parts, &next_part)) {
            zlink_msg_close (&next_part);
            close_msg_vector (*parts);
            parts->clear ();
            errno = ENOMEM;
            return ZLINK_RECV_INTERNAL_ERROR;
        }
        has_more = more;
    }
    return ZLINK_RECV_OK;
}

int send_parts (void *sock, zlink_msg_t *parts, size_t part_count, zlink_send_flags_t flags)
{
    return submit_msg_parts (parts, part_count, [sock, flags] (zlink_msg_t *part,
                                                               zlink_part_flag_t part_flag,
                                                               bool) {
        return zlink_send_part (sock, part, flags, part_flag);
    });
}

int send_parts_rid (void *sock,
                    const zlink_routing_id_t *routing_id,
                    zlink_msg_t *parts,
                    size_t part_count,
                    zlink_send_flags_t flags)
{
    return submit_msg_parts (parts, part_count, [sock, routing_id, flags] (
                                                  zlink_msg_t *part,
                                                  zlink_part_flag_t part_flag, bool) {
        return zlink_send_part_rid (sock, routing_id, part, flags, part_flag);
    });
}

int publish_parts (
  void *sock, const char *topic, zlink_msg_t *parts, size_t part_count, zlink_send_flags_t flags)
{
    return submit_msg_parts (parts, part_count, [sock, topic, flags] (zlink_msg_t *part,
                                                                      zlink_part_flag_t part_flag,
                                                                      bool) {
        return zlink_publish_part (sock, topic, part, flags, part_flag);
    });
}

int dealer_request_parts (void *dealer,
                          zlink_msg_t *parts,
                          size_t part_count,
                          zlink_reply_handler_fn handler,
                          void *userdata,
                          zlink_send_flags_t flags,
                          uint32_t timeout_ms)
{
    return submit_msg_parts (parts, part_count, [dealer, handler, userdata, flags, timeout_ms] (
                                                  zlink_msg_t *part,
                                                  zlink_part_flag_t part_flag, bool is_final) {
        return zlink_dealer_request_part (dealer, part, flags, part_flag,
                                          is_final ? timeout_ms : 0u,
                                          is_final ? handler : NULL,
                                          is_final ? userdata : NULL);
    });
}

int router_request_parts (void *router,
                          const zlink_routing_id_t *peer_rid,
                          zlink_msg_t *parts,
                          size_t part_count,
                          zlink_reply_handler_fn handler,
                          void *userdata,
                          zlink_send_flags_t flags,
                          uint32_t timeout_ms)
{
    return submit_msg_parts (parts, part_count, [router, peer_rid, handler, userdata, flags,
                                                 timeout_ms] (zlink_msg_t *part,
                                                              zlink_part_flag_t part_flag,
                                                              bool is_final) {
        return zlink_router_request_part (router, peer_rid, part, flags, part_flag,
                                          is_final ? timeout_ms : 0u,
                                          is_final ? handler : NULL,
                                          is_final ? userdata : NULL);
    });
}

int router_request_transport_pair_parts (
  void *router,
  const zlink_routing_id_t *peer_rid,
  uint64_t pair_id,
  uint64_t pair_generation,
  zlink_msg_t *parts,
  size_t part_count,
  zlink_reply_handler_fn handler,
  void *userdata,
  zlink_send_flags_t flags,
  uint32_t timeout_ms)
{
    return submit_msg_parts (parts, part_count,
      [router, peer_rid, pair_id, pair_generation, handler, userdata, flags, timeout_ms]
      (zlink_msg_t *part, zlink_part_flag_t part_flag, bool is_final) {
          return zlink_router_request_transport_pair_part (
            router, peer_rid, pair_id, pair_generation, part, flags, part_flag,
            is_final ? timeout_ms : 0u,
            is_final ? handler : NULL,
            is_final ? userdata : NULL);
      });
}

int router_reply_parts (void *router,
                        const zlink_routing_id_t *peer_rid,
                        uint64_t request_seq,
                        zlink_msg_t *parts,
                        size_t part_count)
{
    return submit_msg_parts (parts, part_count, [router, peer_rid, request_seq] (
                                                  zlink_msg_t *part,
                                                  zlink_part_flag_t part_flag, bool) {
        return zlink_router_reply_part (router, peer_rid, request_seq, part, part_flag);
    });
}

napi_value create_recv_message_value (napi_env env,
                                      const zlink_routing_id_t &routing_id,
                                      zlink_msg_t *parts,
                                      size_t part_count)
{
    napi_value obj;
    napi_create_object (env, &obj);

    // Hot path: this is an internal raw shape consumed by the TypeScript
    // materializer, not a public Received object. Do not add unused per-
    // message fields here; each property set is paid on every recv().
    if (part_count == 1) {
        // A one-part receive needs neither a parts array nor a part snapshot.
        // Allocate the payload as a JS-owned Buffer immediately. This keeps
        // data() and close() on the JS side for every payload size.
        napi_value data = create_received_message_buffer (env, &parts[0]);
        if (!data)
            return NULL;
        napi_set_named_property (env, obj, "data", data);
        if (routing_id.size > 0) {
            napi_value rid = create_routing_id_value (env, routing_id);
            napi_set_named_property (env, obj, "routingId", rid);
        }
        return obj;
    }

    napi_value parts_array;
    napi_create_array_with_length (env, part_count, &parts_array);
    for (size_t i = 0; i < part_count; ++i) {
        napi_value part = create_message_snapshot_value (env, &routing_id, &parts[i]);
        napi_set_element (env, parts_array, static_cast<uint32_t> (i), part);
    }

    napi_set_named_property (env, obj, "parts", parts_array);
    if (routing_id.size > 0) {
        napi_value rid = create_routing_id_value (env, routing_id);
        napi_set_named_property (env, obj, "routingId", rid);
    }
    return obj;
}

napi_value create_router_recv_message_value (napi_env env,
                                             const zlink_routing_id_t &routing_id,
                                             uint64_t request_seq,
                                             uint64_t transport_pair_id,
                                             uint64_t transport_pair_generation,
                                             zlink_msg_t *parts,
                                             size_t part_count)
{
    napi_value obj = create_recv_message_value (env, routing_id, parts, part_count);
    napi_value request_seq_value;
    if (request_seq == 0) {
        napi_get_null (env, &request_seq_value);
    } else {
        napi_create_bigint_uint64 (env, request_seq, &request_seq_value);
    }
    napi_set_named_property (env, obj, "requestSeq", request_seq_value);
    napi_value pair_id_value;
    napi_value pair_generation_value;
    napi_create_bigint_uint64 (env, transport_pair_id, &pair_id_value);
    napi_create_bigint_uint64 (env, transport_pair_generation, &pair_generation_value);
    napi_set_named_property (env, obj, "transportPairId", pair_id_value);
    napi_set_named_property (env, obj, "transportPairGeneration", pair_generation_value);
    return obj;
}

napi_value create_subscription_event_value (napi_env env,
                                            const zlink_routing_id_t &routing_id,
                                            int subscribed,
                                            const char *topic,
                                            size_t topic_len)
{
    napi_value obj;
    napi_create_object (env, &obj);

    napi_value rid = create_routing_id_value (env, routing_id);
    napi_value topic_value;
    napi_create_string_utf8 (env, topic ? topic : "", topic ? topic_len : 0, &topic_value);
    napi_value subscribed_value;
    napi_get_boolean (env, subscribed != 0, &subscribed_value);

    napi_set_named_property (env, obj, "routingId", rid);
    napi_set_named_property (env, obj, "topic", topic_value);
    napi_set_named_property (env, obj, "subscribed", subscribed_value);
    return obj;
}

napi_value create_socket_monitor_event_value (napi_env env, const zlink_monitor_event_t &event)
{
    napi_value obj;
    napi_create_object (env, &obj);

    napi_value value;
    napi_create_int64 (env, static_cast<int64_t> (event.event), &value);
    napi_set_named_property (env, obj, "event", value);

    napi_create_int64 (env, static_cast<int64_t> (event.value), &value);
    napi_set_named_property (env, obj, "value", value);

    napi_value routing_id = create_routing_id_value (env, event.routing_id);
    napi_set_named_property (env, obj, "routingId", routing_id);

    napi_value local;
    napi_create_string_utf8 (env, event.local_addr, NAPI_AUTO_LENGTH, &local);
    napi_set_named_property (env, obj, "local", local);

    napi_value remote;
    napi_create_string_utf8 (env, event.remote_addr, NAPI_AUTO_LENGTH, &remote);
    napi_set_named_property (env, obj, "remote", remote);

    napi_value bigint;
    napi_create_bigint_uint64 (env, event.connection_id, &bigint);
    napi_set_named_property (env, obj, "connectionId", bigint);

    napi_create_bigint_uint64 (env, event.transport_pair_id, &bigint);
    napi_set_named_property (env, obj, "transportPairId", bigint);

    napi_create_bigint_uint64 (env, event.transport_pair_generation, &bigint);
    napi_set_named_property (env, obj, "transportPairGeneration", bigint);

    napi_create_uint32 (env, event.transport_lane, &value);
    napi_set_named_property (env, obj, "transportLane", value);

    napi_create_uint32 (env, event.flags, &value);
    napi_set_named_property (env, obj, "flags", value);

    return obj;
}

napi_value create_subscribed_value (napi_env env,
                                    const zlink_routing_id_t &routing_id,
                                    const char *topic,
                                    size_t topic_len,
                                    zlink_msg_t *parts,
                                    size_t part_count)
{
    napi_value obj;
    napi_create_object (env, &obj);

    if (part_count == 1) {
        napi_value data = create_received_message_buffer (env, &parts[0]);
        if (!data)
            return NULL;
        napi_set_named_property (env, obj, "data", data);
    } else {
        napi_value parts_array;
        napi_create_array_with_length (env, part_count, &parts_array);
        for (size_t i = 0; i < part_count; ++i) {
            napi_value part = create_message_snapshot_value (env, &routing_id, &parts[i]);
            napi_set_element (env, parts_array, static_cast<uint32_t> (i), part);
        }
        napi_set_named_property (env, obj, "parts", parts_array);
    }

    napi_value topic_value;
    napi_create_string_utf8 (env, topic ? topic : "", topic ? topic_len : 0, &topic_value);

    if (routing_id.size > 0) {
        // Hot path: plain SUB messages normally have no source routing id.
        // Leaving the internal raw field absent preserves the public null
        // routing id after TypeScript materialization and avoids creating a
        // per-message JS null property on the common receive path.
        napi_value rid = create_routing_id_value (env, routing_id);
        napi_set_named_property (env, obj, "routingId", rid);
    }
    napi_set_named_property (env, obj, "topic", topic_value);
    return obj;
}

void stream_tsfn_finalize (napi_env env, void *finalize_data, void *finalize_hint)
{
    (void) env;
    (void) finalize_hint;
    stream_js_state_t *state = static_cast<stream_js_state_t *> (finalize_data);
    if (!state)
        return;
    std::lock_guard<std::mutex> lock (g_stream_slots_mu);
    reset_stream_slot_unsafe (state);
}

void send_ready_handler_tsfn_finalize (napi_env env, void *finalize_data, void *finalize_hint)
{
    (void) env;
    (void) finalize_hint;
    send_ready_handler_js_state_t *state =
      static_cast<send_ready_handler_js_state_t *> (finalize_data);
    if (!state)
        return;
    std::lock_guard<std::mutex> lock (g_send_ready_handler_slots_mu);
    reset_send_ready_handler_slot_unsafe (state);
}

void completion_control_handler_tsfn_finalize (
  napi_env env, void *finalize_data, void *finalize_hint)
{
    (void) env;
    (void) finalize_hint;
    completion_control_handler_js_state_t *state =
      static_cast<completion_control_handler_js_state_t *> (finalize_data);
    if (!state)
        return;
    {
        std::lock_guard<std::mutex> lock (
          g_completion_control_handler_slots_mu);
        if (state->socket) {
            std::unordered_map<
              void *,
              std::vector<completion_control_handler_js_state_t *> >::iterator
              entry = g_completion_control_handler_states.find (state->socket);
            if (entry != g_completion_control_handler_states.end ()) {
                std::vector<completion_control_handler_js_state_t *> &states =
                  entry->second;
                states.erase (
                  std::remove (states.begin (), states.end (), state),
                  states.end ());
                if (states.empty ())
                    g_completion_control_handler_states.erase (entry);
            }
        }
    }
    delete state;
}

void socket_monitor_handler_tsfn_finalize (napi_env env, void *finalize_data, void *finalize_hint)
{
    (void) env;
    (void) finalize_hint;
    socket_monitor_handler_js_state_t *state =
      static_cast<socket_monitor_handler_js_state_t *> (finalize_data);
    if (!state)
        return;
    std::lock_guard<std::mutex> lock (g_socket_monitor_handler_slots_mu);
    reset_socket_monitor_handler_slot_unsafe (state);
}

void stream_tsfn_call_js (napi_env env, napi_value js_cb, void *context, void *data)
{
    std::unique_ptr<stream_js_payload_t> payload (static_cast<stream_js_payload_t *> (data));
    stream_js_state_t *state = static_cast<stream_js_state_t *> (context);
    if (!env || !js_cb || !state || !payload)
        return;

    napi_value argv[2];
    if (napi_create_buffer_copy (env, payload->routing_id.size (),
                                 payload->routing_id.empty () ? NULL : payload->routing_id.data (),
                                 NULL, &argv[0])
        != napi_ok) {
        return;
    }
    if (napi_create_array_with_length (env, payload->packet_count, &argv[1]) != napi_ok) {
        return;
    }
    for (size_t i = 0; i < payload->packet_count; ++i) {
        napi_value packet_value = i == 1 && payload->body_materialization == 0
          ? move_message_to_native_frame_value (env, &payload->packets[i])
          : create_received_message_buffer (env, &payload->packets[i]);
        if (!packet_value) {
            return;
        }
        napi_set_element (env, argv[1], static_cast<uint32_t> (i), packet_value);
    }

    napi_value recv;
    napi_value this_arg;
    napi_get_undefined (env, &this_arg);
    napi_status call_status = napi_call_function (env, this_arg, js_cb, 2, argv, &recv);
    if (call_status != napi_ok) {
        state->stop_requested.store (1, std::memory_order_release);
        return;
    }

    int32_t ret = 0;
    if (napi_get_value_int32 (env, recv, &ret) == napi_ok && ret != 0) {
        state->stop_requested.store (1, std::memory_order_release);
    }
}

void send_ready_handler_tsfn_call_js (napi_env env, napi_value js_cb, void *context, void *data)
{
    std::unique_ptr<int> payload (static_cast<int *> (data));
    (void) context;
    if (!env || !js_cb || !payload)
        return;

    napi_value recv;
    napi_value this_arg;
    napi_get_undefined (env, &this_arg);
    (void) napi_call_function (env, this_arg, js_cb, 0, NULL, &recv);
}

void completion_control_handler_tsfn_call_js (
  napi_env env, napi_value js_cb, void *context, void *data)
{
    std::unique_ptr<completion_control_js_payload_t> payload (
      static_cast<completion_control_js_payload_t *> (data));
    (void) context;
    if (!env || !js_cb || !payload)
        return;

    napi_value argv[2];
    if (napi_create_buffer_copy (
          env, payload->routing_id.size (),
          payload->routing_id.empty () ? NULL : payload->routing_id.data (),
          NULL, &argv[0])
        != napi_ok)
        return;
    if (napi_create_array_with_length (env, payload->parts.size (), &argv[1])
        != napi_ok)
        return;
    for (size_t i = 0; i < payload->parts.size (); ++i) {
        napi_value part;
        const std::vector<unsigned char> &bytes = payload->parts[i];
        if (napi_create_buffer_copy (
              env, bytes.size (), bytes.empty () ? NULL : bytes.data (), NULL,
              &part)
            != napi_ok)
            return;
        napi_set_element (env, argv[1], static_cast<uint32_t> (i), part);
    }

    napi_value recv;
    napi_value this_arg;
    napi_get_undefined (env, &this_arg);
    (void) napi_call_function (env, this_arg, js_cb, 2, argv, &recv);
}

void socket_monitor_handler_tsfn_call_js (napi_env env, napi_value js_cb, void *context, void *data)
{
    std::unique_ptr<socket_monitor_handler_js_payload_t> payload (
      static_cast<socket_monitor_handler_js_payload_t *> (data));
    (void) context;
    if (!env || !js_cb || !payload)
        return;

    napi_value argv[1];
    argv[0] = create_socket_monitor_event_value (env, payload->event);
    napi_value recv;
    napi_value this_arg;
    napi_get_undefined (env, &this_arg);
    (void) napi_call_function (env, this_arg, js_cb, 1, argv, &recv);
}

template <size_t Slot>
void stream_on_packet_slot (void *stream_,
                            const zlink_routing_id_t *rid_,
                            zlink_msg_t *header_,
                            zlink_msg_t *body_,
                            void *userdata_)
{
    (void) stream_;
    (void) userdata_;

    const auto close_messages = [header_, body_] () {
        if (header_)
            (void) zlink_msg_close (header_);
        if (body_)
            (void) zlink_msg_close (body_);
    };

    if (!rid_ || !header_ || !body_) {
        close_messages ();
        return;
    }

    stream_js_state_t *state = &g_stream_slots[Slot];
    napi_threadsafe_function tsfn = NULL;
    std::unique_ptr<stream_js_payload_t> payload;
    {
        std::lock_guard<std::mutex> lock (g_stream_slots_mu);
        if (!state->used || !state->tsfn) {
            close_messages ();
            return;
        }
        if (state->stop_requested.load (std::memory_order_acquire) != 0) {
            close_messages ();
            return;
        }
        tsfn = state->tsfn;
        payload.reset (new stream_js_payload_t ());
        payload->body_materialization = state->body_materialization;
        payload->routing_id.assign (rid_->data, rid_->data + rid_->size);
        if (zlink_msg_init (&payload->packets[0]) != 0) {
            close_messages ();
            return;
        }
        payload->packet_count = 1;
        if (zlink_msg_move (&payload->packets[0], header_) != 0) {
            close_messages ();
            return;
        }
        if (zlink_msg_init (&payload->packets[1]) != 0) {
            close_messages ();
            return;
        }
        payload->packet_count = 2;
        if (zlink_msg_move (&payload->packets[1], body_) != 0) {
            close_messages ();
            return;
        }
    }

    close_messages ();

    // This callback runs on the Core socket I/O thread. Waiting for the JS
    // queue here can prevent the same I/O thread from delivering another
    // request completions needed by the session dispatch that consumes this
    // packet. The TSFN queue is unbounded and preserves FIFO order, while the
    // framework serializes dispatch per session, so enqueue without blocking
    // and return ownership to Core immediately.
    if (napi_call_threadsafe_function (tsfn, payload.get (), napi_tsfn_nonblocking) == napi_ok) {
        payload.release ();
    }
}

typedef void (*stream_slot_packet_callback_t) (
  void *, const zlink_routing_id_t *, zlink_msg_t *, zlink_msg_t *, void *);

#define STREAM_SLOT_PACKET_CALLBACK(N) &stream_on_packet_slot<N>
static stream_slot_packet_callback_t g_stream_slot_packet_callbacks[k_stream_slot_count] = {
  STREAM_SLOT_PACKET_CALLBACK (0), STREAM_SLOT_PACKET_CALLBACK (1), STREAM_SLOT_PACKET_CALLBACK (2),
  STREAM_SLOT_PACKET_CALLBACK (3), STREAM_SLOT_PACKET_CALLBACK (4), STREAM_SLOT_PACKET_CALLBACK (5),
  STREAM_SLOT_PACKET_CALLBACK (6), STREAM_SLOT_PACKET_CALLBACK (7),
};
#undef STREAM_SLOT_PACKET_CALLBACK

void stream_release_slot (void *socket)
{
    napi_threadsafe_function tsfn = NULL;
    {
        std::lock_guard<std::mutex> lock (g_stream_slots_mu);
        stream_js_state_t *state = find_stream_slot_by_socket_unsafe (socket);
        if (!state)
            return;
        tsfn = state->tsfn;
        // The TSFN finalizer owns the transition back to an unused slot. If
        // close makes this slot reusable first, the old finalizer can reset a
        // new STREAM handler that acquired the same slot in the meantime.
        state->socket = NULL;
        state->stop_requested.store (1, std::memory_order_release);
    }
    if (tsfn)
        (void) napi_release_threadsafe_function (tsfn, napi_tsfn_abort);
}

#define SEND_READY_HANDLER_SLOT_CALLBACK(N) &send_ready_handler_slot_callback<N>
typedef void (*send_ready_handler_slot_callback_t) (void *, void *);
template <size_t Slot> void send_ready_handler_slot_callback (void *subject_, void *userdata_)
{
    (void) userdata_;
    send_ready_handler_js_state_t *state = &g_send_ready_handler_slots[Slot];
    napi_threadsafe_function tsfn = NULL;
    {
        std::lock_guard<std::mutex> lock (g_send_ready_handler_slots_mu);
        if (!state->used || !state->tsfn)
            return;
        tsfn = state->tsfn;
    }

    std::unique_ptr<int> payload (new int (1));
    if (napi_call_threadsafe_function (tsfn, payload.get (), napi_tsfn_nonblocking) == napi_ok) {
        payload.release ();
    }
}

void completion_control_handler_callback (
  const zlink_routing_id_t *source_rid_, zlink_msg_t *parts_,
  size_t part_count_, void *userdata_)
{
    completion_control_handler_js_state_t *state =
      static_cast<completion_control_handler_js_state_t *> (userdata_);
    napi_threadsafe_function tsfn = NULL;
    {
        std::lock_guard<std::mutex> lock (g_completion_control_handler_slots_mu);
        if (state && state->tsfn)
            tsfn = state->tsfn;
    }

    std::unique_ptr<completion_control_js_payload_t> payload;
    if (tsfn && source_rid_ && parts_ && part_count_ > 0) {
        payload.reset (new completion_control_js_payload_t ());
        payload->routing_id.assign (
          source_rid_->data, source_rid_->data + source_rid_->size);
        payload->parts.reserve (part_count_);
        for (size_t i = 0; i < part_count_; ++i) {
            const size_t size = zlink_msg_size (&parts_[i]);
            const unsigned char *data = static_cast<const unsigned char *> (
              zlink_msg_data (&parts_[i]));
            if (size == 0)
                payload->parts.emplace_back ();
            else
                payload->parts.emplace_back (data, data + size);
        }
    }
    if (parts_)
        zlink_multipart_close (parts_, part_count_);

    if (payload
        && napi_call_threadsafe_function (
             tsfn, payload.get (), napi_tsfn_nonblocking)
             == napi_ok)
        payload.release ();
}

#define SOCKET_MONITOR_HANDLER_SLOT_CALLBACK(N) &socket_monitor_handler_slot_callback<N>
typedef void (*socket_monitor_handler_slot_callback_t) (const zlink_monitor_event_t *, void *);
template <size_t Slot>
void socket_monitor_handler_slot_callback (const zlink_monitor_event_t *event_, void *userdata_)
{
    (void) userdata_;
    std::unique_ptr<socket_monitor_handler_js_payload_t> payload (
      new socket_monitor_handler_js_payload_t ());
    socket_monitor_handler_js_state_t *state = &g_socket_monitor_handler_slots[Slot];
    napi_threadsafe_function tsfn = NULL;
    {
        std::lock_guard<std::mutex> lock (g_socket_monitor_handler_slots_mu);
        if (!state->used || !state->tsfn)
            return;
        tsfn = state->tsfn;
        if (event_)
            payload->event = *event_;
        else
            memset (&payload->event, 0, sizeof (payload->event));
    }

    if (napi_call_threadsafe_function (tsfn, payload.get (), napi_tsfn_nonblocking) == napi_ok) {
        payload.release ();
    }
}
static socket_monitor_handler_slot_callback_t
  g_socket_monitor_handler_slot_callbacks[k_socket_monitor_handler_slot_count] = {
    SOCKET_MONITOR_HANDLER_SLOT_CALLBACK (0), SOCKET_MONITOR_HANDLER_SLOT_CALLBACK (1),
    SOCKET_MONITOR_HANDLER_SLOT_CALLBACK (2), SOCKET_MONITOR_HANDLER_SLOT_CALLBACK (3),
    SOCKET_MONITOR_HANDLER_SLOT_CALLBACK (4), SOCKET_MONITOR_HANDLER_SLOT_CALLBACK (5),
    SOCKET_MONITOR_HANDLER_SLOT_CALLBACK (6), SOCKET_MONITOR_HANDLER_SLOT_CALLBACK (7),
};
#undef SOCKET_MONITOR_HANDLER_SLOT_CALLBACK

static send_ready_handler_slot_callback_t
  g_send_ready_handler_slot_callbacks[k_send_ready_handler_slot_count] = {
    SEND_READY_HANDLER_SLOT_CALLBACK (0), SEND_READY_HANDLER_SLOT_CALLBACK (1),
    SEND_READY_HANDLER_SLOT_CALLBACK (2), SEND_READY_HANDLER_SLOT_CALLBACK (3),
    SEND_READY_HANDLER_SLOT_CALLBACK (4), SEND_READY_HANDLER_SLOT_CALLBACK (5),
    SEND_READY_HANDLER_SLOT_CALLBACK (6), SEND_READY_HANDLER_SLOT_CALLBACK (7),
};
#undef SEND_READY_HANDLER_SLOT_CALLBACK

bool attach_send_ready_handler (napi_env env, void *socket, napi_value handler)
{
    size_t slot_index = 0;
    send_ready_handler_js_state_t *slot = reserve_tsfn_subject_slot (
      env, g_send_ready_handler_slots_mu, g_send_ready_handler_slots,
      k_send_ready_handler_slot_count, &send_ready_handler_js_state_t::socket, socket,
      "sendReadyHandler already attached", "no free sendReadyHandler slot", &slot_index);
    if (!slot)
        return false;

    napi_threadsafe_function tsfn = NULL;
    if (!create_tsfn_slot_queue (env, handler, slot, "zlink-send-ready-handler",
                                 send_ready_handler_tsfn_finalize,
                                 send_ready_handler_tsfn_call_js,
                                 "sendReadyHandler failed to create callback queue", true, &tsfn))
        return false;

    {
        std::lock_guard<std::mutex> lock (g_send_ready_handler_slots_mu);
        bind_tsfn_subject_slot_unsafe (slot, &send_ready_handler_js_state_t::socket, socket, env,
                                       tsfn);
    }

    int rc =
      zlink_send_ready_handler (socket, g_send_ready_handler_slot_callbacks[slot_index], slot);
    if (rc != 0) {
        release_socket_send_ready_handler_slot (socket);
        throw_last_error (env, "sendReadyHandler failed");
        return false;
    }
    return true;
}

bool attach_completion_control_handler (
  napi_env env, void *socket, napi_value handler)
{
    completion_control_handler_js_state_t *state =
      new completion_control_handler_js_state_t ();

    napi_threadsafe_function tsfn = NULL;
    if (!create_tsfn_slot_queue (
          env, handler, state, "zlink-completion-control-handler",
          completion_control_handler_tsfn_finalize,
          completion_control_handler_tsfn_call_js,
          "completionControlHandler failed to create callback queue", true,
          &tsfn)) {
        delete state;
        return false;
    }

    state->socket = socket;
    state->env = env;
    state->tsfn = tsfn;

    zlink_handler_result_t rc;
    {
        // Serialize replacement for one socket. The previous TSFN remains
        // available until socket close so a Core callback already in flight
        // can still enqueue the payload it accepted.
        std::lock_guard<std::mutex> registration_lock (
          g_completion_control_handler_registration_mu);
        rc = zlink_router_completion_control_handler (
          socket, completion_control_handler_callback, state);
        if (rc == ZLINK_HANDLER_OK) {
            std::lock_guard<std::mutex> state_lock (
              g_completion_control_handler_slots_mu);
            std::vector<completion_control_handler_js_state_t *> &states =
              g_completion_control_handler_states[socket];
            states.push_back (state);
        }
    }
    if (rc != ZLINK_HANDLER_OK) {
        state->socket = NULL;
        (void) napi_release_threadsafe_function (tsfn, napi_tsfn_abort);
        throw_last_error (env, "completionControlHandler failed");
        return false;
    }
    return true;
}

bool attach_socket_monitor_handler (napi_env env, void *monitor, napi_value handler)
{
    size_t slot_index = 0;
    socket_monitor_handler_js_state_t *slot = reserve_tsfn_subject_slot (
      env, g_socket_monitor_handler_slots_mu, g_socket_monitor_handler_slots,
      k_socket_monitor_handler_slot_count, &socket_monitor_handler_js_state_t::monitor, monitor,
      "monitorHandler already attached", "no free monitorHandler slot", &slot_index);
    if (!slot)
        return false;

    napi_threadsafe_function tsfn = NULL;
    if (!create_tsfn_slot_queue (env, handler, slot, "zlink-monitor-handler",
                                 socket_monitor_handler_tsfn_finalize,
                                 socket_monitor_handler_tsfn_call_js,
                                 "monitorHandler failed to create callback queue", true, &tsfn))
        return false;

    {
        std::lock_guard<std::mutex> lock (g_socket_monitor_handler_slots_mu);
        bind_tsfn_subject_slot_unsafe (slot, &socket_monitor_handler_js_state_t::monitor, monitor,
                                       env, tsfn);
    }

    int rc = zlink_socket_monitor_handler (
      monitor, g_socket_monitor_handler_slot_callbacks[slot_index], slot);
    if (rc != 0) {
        release_socket_monitor_handler_slot (monitor);
        throw_last_error (env, "monitorHandler failed");
        return false;
    }
    return true;
}

} // namespace

void release_socket_send_ready_handler_slot (void *socket)
{
    napi_threadsafe_function tsfn = NULL;
    {
        std::lock_guard<std::mutex> lock (g_send_ready_handler_slots_mu);
        send_ready_handler_js_state_t *state =
          find_send_ready_handler_slot_by_socket_unsafe (socket);
        if (!state)
            return;
        tsfn = state->tsfn;
        reset_send_ready_handler_slot_unsafe (state);
    }
    if (tsfn)
        (void) napi_release_threadsafe_function (tsfn, napi_tsfn_abort);
}

void release_socket_completion_control_handler_slot (void *socket)
{
    std::vector<completion_control_handler_js_state_t *> states;
    {
        std::lock_guard<std::mutex> lock (
          g_completion_control_handler_slots_mu);
        std::unordered_map<
          void *, std::vector<completion_control_handler_js_state_t *> >::iterator
          entry = g_completion_control_handler_states.find (socket);
        if (entry == g_completion_control_handler_states.end ())
            return;
        states.swap (entry->second);
        g_completion_control_handler_states.erase (entry);
        for (size_t i = 0; i < states.size (); ++i)
            states[i]->socket = NULL;
    }
    for (size_t i = 0; i < states.size (); ++i) {
        if (states[i]->tsfn)
            (void) napi_release_threadsafe_function (
              states[i]->tsfn, napi_tsfn_release);
    }
}

void release_socket_monitor_handler_slot (void *monitor)
{
    napi_threadsafe_function tsfn = NULL;
    {
        std::lock_guard<std::mutex> lock (g_socket_monitor_handler_slots_mu);
        socket_monitor_handler_js_state_t *state =
          find_socket_monitor_handler_slot_by_monitor_unsafe (monitor);
        if (!state)
            return;
        tsfn = state->tsfn;
        reset_socket_monitor_handler_slot_unsafe (state);
    }
    if (tsfn)
        (void) napi_release_threadsafe_function (tsfn, napi_tsfn_abort);
}

napi_value throw_last_error (napi_env env, const char *prefix)
{
    int err = zlink_errno ();
    const char *msg = zlink_strerror (err);
    char buf[256];
    snprintf (buf, sizeof (buf), "%s: %s", prefix, msg ? msg : "error");
    napi_throw_error (env, NULL, buf);
    return NULL;
}

std::string get_string (napi_env env, napi_value val)
{
    size_t len = 0;
    napi_get_value_string_utf8 (env, val, NULL, 0, &len);
    std::string out (len, '\0');
    napi_get_value_string_utf8 (env, val, out.data (), len + 1, &len);
    return out;
}

const char *get_c_string_arg (
  napi_env env, napi_value val, char *stack_buf, size_t stack_buf_size, std::string *heap_buf)
{
    size_t len = 0;
    napi_get_value_string_utf8 (env, val, NULL, 0, &len);
    if (len + 1 <= stack_buf_size) {
        napi_get_value_string_utf8 (env, val, stack_buf, stack_buf_size, &len);
        return stack_buf;
    }
    heap_buf->assign (len, '\0');
    napi_get_value_string_utf8 (env, val, heap_buf->data (), len + 1, &len);
    return heap_buf->c_str ();
}

bool get_uint64_like (napi_env env, napi_value value, uint64_t *out);

static void close_built_msg_vector (std::vector<zlink_msg_t> *parts, size_t built)
{
    if (!parts)
        return;
    for (size_t i = 0; i < built; ++i)
        zlink_msg_close (&(*parts)[i]);
    parts->clear ();
}

bool build_msg_vector (napi_env env, napi_value arr, std::vector<zlink_msg_t> *out)
{
    uint32_t len = 0;
    if (napi_get_array_length (env, arr, &len) != napi_ok) {
        napi_throw_type_error (env, NULL, "parts must be an array");
        return false;
    }
    out->clear ();
    out->resize (len);
    size_t built = 0;
    for (uint32_t i = 0; i < len; i++) {
        napi_value val;
        if (napi_get_element (env, arr, i, &val) != napi_ok) {
            for (size_t j = 0; j < built; j++)
                zlink_msg_close (&(*out)[j]);
            napi_throw_type_error (env, NULL, "parts element read failed");
            return false;
        }
        if (!init_msg_from_value (env, val, &(*out)[i])) {
            close_built_msg_vector (out, built);
            return false;
        }
        built++;
    }
    return true;
}

bool build_msg_vector_or_single (napi_env env, napi_value value, std::vector<zlink_msg_t> *out)
{
    bool is_array = false;
    if (napi_is_array (env, value, &is_array) == napi_ok && is_array)
        return build_msg_vector (env, value, out);

    out->clear ();
    out->resize (1);
    if (!init_msg_from_value (env, value, &(*out)[0])) {
        out->clear ();
        return false;
    }
    return true;
}

void close_msg_vector (std::vector<zlink_msg_t> &parts)
{
    for (size_t i = 0; i < parts.size (); i++)
        zlink_msg_close (&parts[i]);
}

bool get_uint64_like (napi_env env, napi_value value, uint64_t *out)
{
    bool lossless = false;
    if (napi_get_value_bigint_uint64 (env, value, out, &lossless) == napi_ok)
        return true;

    napi_valuetype type = napi_undefined;
    if (napi_typeof (env, value, &type) != napi_ok)
        return false;
    if (type == napi_number) {
        double number = 0;
        if (napi_get_value_double (env, value, &number) != napi_ok || number < 0)
            return false;
        *out = static_cast<uint64_t> (number);
        return true;
    }

    napi_value coerced;
    if (napi_coerce_to_string (env, value, &coerced) != napi_ok)
        return false;
    std::string text = get_string (env, coerced);
    char *end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull (text.c_str (), &end, 10);
    if (errno != 0 || end == text.c_str () || (end && *end != '\0'))
        return false;
    *out = static_cast<uint64_t> (parsed);
    return true;
}

bool init_msg_from_value (napi_env env,
                          napi_value value,
                          zlink_msg_t *msg,
                          bool *contains_native_frame)
{
    if (contains_native_frame)
        *contains_native_frame = false;
    bool is_buf = false;
    if (napi_is_buffer (env, value, &is_buf) == napi_ok && is_buf) {
        void *data = NULL;
        size_t len = 0;
        if (napi_get_buffer_info (env, value, &data, &len) != napi_ok) {
            napi_throw_type_error (env, NULL, "send buffer invalid");
            return false;
        }
        if (!init_msg_from_bytes (msg, data, len))
            return false;
        return true;
    }

    bool has_native_message = false;
    if (napi_has_named_property (env, value, "nativeMessage", &has_native_message) != napi_ok) {
        napi_throw_type_error (env, NULL, "message snapshot native frame lookup failed");
        return false;
    }
    if (has_native_message) {
        napi_value native_message;
        if (napi_get_named_property (env, value, "nativeMessage", &native_message) != napi_ok) {
            napi_throw_type_error (env, NULL, "message snapshot native frame read failed");
            return false;
        }
        native_message_frame_t *frame = get_native_message_frame (
          env, native_message, "message snapshot native frame is invalid");
        if (!frame) {
            return false;
        }
        if (zlink_msg_init (msg) != 0)
            return false;
        if (zlink_msg_copy (msg, &frame->message) != 0) {
            zlink_msg_close (msg);
            return false;
        }
        if (contains_native_frame)
            *contains_native_frame = true;
        return true;
    }

    napi_value data_value;
    if (napi_get_named_property (env, value, "data", &data_value) != napi_ok) {
        napi_throw_type_error (env, NULL, "message must be a Buffer or message snapshot");
        return false;
    }
    void *data = NULL;
    size_t len = 0;
    if (napi_get_buffer_info (env, data_value, &data, &len) != napi_ok) {
        napi_throw_type_error (env, NULL, "message snapshot data must be a Buffer");
        return false;
    }
    if (!init_msg_from_bytes (msg, data, len))
        return false;

    return true;
}

struct message_value_span_t
{
    const unsigned char *data;
    size_t size;
};

bool get_message_value_span (napi_env env, napi_value value, message_value_span_t *span)
{
    bool is_buf = false;
    if (napi_is_buffer (env, value, &is_buf) == napi_ok && is_buf) {
        void *data = NULL;
        if (napi_get_buffer_info (env, value, &data, &span->size) != napi_ok) {
            napi_throw_type_error (env, NULL, "stream send buffer invalid");
            return false;
        }
        span->data = static_cast<const unsigned char *> (data);
        return true;
    }

    bool has_native_message = false;
    if (napi_has_named_property (env, value, "nativeMessage", &has_native_message) != napi_ok) {
        napi_throw_type_error (env, NULL, "stream message snapshot native frame lookup failed");
        return false;
    }
    if (has_native_message) {
        napi_value native_message;
        if (napi_get_named_property (env, value, "nativeMessage", &native_message) != napi_ok) {
            napi_throw_type_error (env, NULL, "stream message snapshot native frame read failed");
            return false;
        }
        native_message_frame_t *frame = get_native_message_frame (
          env, native_message, "stream message snapshot native frame is invalid");
        if (!frame)
            return false;
        span->size = zlink_msg_size (&frame->message);
        span->data = static_cast<const unsigned char *> (zlink_msg_data (&frame->message));
        return true;
    }

    napi_value data_value;
    if (napi_get_named_property (env, value, "data", &data_value) != napi_ok) {
        napi_throw_type_error (env, NULL, "stream message must be a Buffer or message snapshot");
        return false;
    }
    void *data = NULL;
    if (napi_get_buffer_info (env, data_value, &data, &span->size) != napi_ok) {
        napi_throw_type_error (env, NULL, "stream message snapshot data must be a Buffer");
        return false;
    }
    span->data = static_cast<const unsigned char *> (data);
    return true;
}

bool init_contiguous_msg_from_array (napi_env env, napi_value value, zlink_msg_t *msg)
{
    bool is_array = false;
    if (napi_is_array (env, value, &is_array) != napi_ok || !is_array) {
        napi_throw_type_error (env, NULL, "stream parts must be an array");
        return false;
    }

    uint32_t length = 0;
    if (napi_get_array_length (env, value, &length) != napi_ok || length == 0) {
        napi_throw_type_error (env, NULL, "stream parts must not be empty");
        return false;
    }

    std::vector<message_value_span_t> spans (length);
    size_t total = 0;
    for (uint32_t index = 0; index < length; ++index) {
        napi_value part;
        if (napi_get_element (env, value, index, &part) != napi_ok
            || !get_message_value_span (env, part, &spans[index]))
            return false;
        if (spans[index].size > std::numeric_limits<size_t>::max () - total) {
            napi_throw_range_error (env, NULL, "stream parts are too large");
            return false;
        }
        total += spans[index].size;
    }

    if (zlink_msg_init_size (msg, total) != 0)
        return false;
    unsigned char *write = static_cast<unsigned char *> (zlink_msg_data (msg));
    for (size_t index = 0; index < spans.size (); ++index) {
        if (spans[index].size > 0) {
            memcpy (write, spans[index].data, spans[index].size);
            write += spans[index].size;
        }
    }
    return true;
}

void consume_native_message_value (napi_env env, napi_value value)
{
    bool is_array = false;
    if (napi_is_array (env, value, &is_array) == napi_ok && is_array) {
        uint32_t length = 0;
        napi_get_array_length (env, value, &length);
        for (uint32_t index = 0; index < length; ++index) {
            napi_value part;
            if (napi_get_element (env, value, index, &part) == napi_ok)
                consume_native_message_value (env, part);
        }
        return;
    }
    napi_valuetype value_type = napi_undefined;
    if (napi_typeof (env, value, &value_type) != napi_ok || value_type != napi_object)
        return;
    bool has_native_message = false;
    if (napi_has_named_property (env, value, "nativeMessage", &has_native_message) != napi_ok
        || !has_native_message)
        return;
    napi_value native_message;
    native_message_frame_handle_t *handle = NULL;
    if (napi_get_named_property (env, value, "nativeMessage", &native_message) != napi_ok
        || napi_get_value_external (
             env, native_message, reinterpret_cast<void **> (&handle)) != napi_ok
        || !handle || !handle->frame)
        return;
    zlink_msg_close (&handle->frame->message);
    zlink_msg_init (&handle->frame->message);
}

napi_value message_from_buffer (napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    if (argc < 1) {
        napi_throw_type_error (env, NULL, "messageFromBuffer requires a Buffer");
        return NULL;
    }
    void *data = NULL;
    size_t size = 0;
    if (napi_get_buffer_info (env, argv[0], &data, &size) != napi_ok) {
        napi_throw_type_error (env, NULL, "messageFromBuffer requires a Buffer");
        return NULL;
    }
    native_message_frame_t *frame = new (std::nothrow) native_message_frame_t;
    if (!frame) {
        napi_throw_error (env, NULL, "native message frame allocation failed");
        return NULL;
    }
    if (!init_msg_from_bytes (&frame->message, data, size)) {
        delete frame;
        return throw_last_error (env, "message native allocation failed");
    }
    return create_native_message_value (env, frame);
}

napi_value message_allocate (napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    int64_t size = 0;
    if (argc < 1 || napi_get_value_int64 (env, argv[0], &size) != napi_ok || size < 0) {
        napi_throw_range_error (env, NULL, "messageAllocate requires a non-negative size");
        return NULL;
    }
    native_message_frame_t *frame = new (std::nothrow) native_message_frame_t;
    if (!frame) {
        napi_throw_error (env, NULL, "native message frame allocation failed");
        return NULL;
    }
    if (zlink_msg_init_size (&frame->message, static_cast<size_t> (size)) != 0) {
        delete frame;
        return throw_last_error (env, "message native allocation failed");
    }
    return create_native_message_value (env, frame);
}

napi_value message_frame_data (napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    if (argc < 1) {
        napi_throw_type_error (env, NULL, "messageFrameData requires a native message frame");
        return NULL;
    }
    native_message_frame_t *frame = get_native_message_frame (
      env, argv[0], "messageFrameData requires a native message frame");
    if (!frame)
        return NULL;
    return create_native_message_data_buffer (env, frame);
}

napi_value message_frame_size (napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    if (argc < 1) {
        napi_throw_type_error (env, NULL, "messageFrameSize requires a native message frame");
        return NULL;
    }
    native_message_frame_t *frame = get_native_message_frame (
      env, argv[0], "messageFrameSize requires a native message frame");
    if (!frame)
        return NULL;
    napi_value size;
    napi_create_double (env, static_cast<double> (zlink_msg_size (&frame->message)), &size);
    return size;
}

napi_value message_frame_close (napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    native_message_frame_handle_t *handle = NULL;
    if (argc < 1
        || napi_get_value_external (
             env, argv[0], reinterpret_cast<void **> (&handle)) != napi_ok
        || !handle) {
        napi_throw_type_error (env, NULL, "messageFrameClose requires a native message frame");
        return NULL;
    }
    release_native_message_frame (handle->frame);
    handle->frame = NULL;
    napi_value out;
    napi_get_undefined (env, &out);
    return out;
}

napi_value version (napi_env env, napi_callback_info info)
{
    int major = 0, minor = 0, patch = 0;
    zlink_version (&major, &minor, &patch);
    napi_value arr;
    napi_create_array_with_length (env, 3, &arr);
    napi_value v0, v1, v2;
    napi_create_int32 (env, major, &v0);
    napi_create_int32 (env, minor, &v1);
    napi_create_int32 (env, patch, &v2);
    napi_set_element (env, arr, 0, v0);
    napi_set_element (env, arr, 1, v1);
    napi_set_element (env, arr, 2, v2);
    return arr;
}

napi_value errno_value (napi_env env, napi_callback_info info)
{
    napi_value out;
    napi_create_int32 (env, zlink_errno (), &out);
    return out;
}

napi_value strerror_value (napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    int32_t errnum = 0;
    if (argc >= 1)
        napi_get_value_int32 (env, argv[0], &errnum);

    const char *message = zlink_strerror (errnum);
    napi_value out;
    napi_create_string_utf8 (env, message ? message : "", NAPI_AUTO_LENGTH, &out);
    return out;
}

napi_value has (napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    if (argc < 1) {
        napi_throw_type_error (env, NULL, "has requires a capability string");
        return NULL;
    }

    std::string capability = get_string (env, argv[0]);
    napi_value out;
    napi_get_boolean (env, zlink_has (capability.c_str ()), &out);
    return out;
}

napi_value proxy (napi_env env, napi_callback_info info)
{
    napi_value argv[3];
    size_t argc = 3;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    if (argc < 2) {
        napi_throw_type_error (env, NULL, "proxy requires frontend and backend");
        return NULL;
    }

    void *frontend = NULL;
    void *backend = NULL;
    void *capture = NULL;
    napi_get_value_external (env, argv[0], &frontend);
    napi_get_value_external (env, argv[1], &backend);
    if (argc >= 3) {
        napi_valuetype capture_type = napi_undefined;
        napi_typeof (env, argv[2], &capture_type);
        if (capture_type != napi_undefined && capture_type != napi_null)
            napi_get_value_external (env, argv[2], &capture);
    }

    if (zlink_proxy (frontend, backend, capture) != ZLINK_CONFIG_OK)
        return throw_last_error (env, "proxy failed");
    napi_value out;
    napi_get_undefined (env, &out);
    return out;
}

napi_value proxy_steerable (napi_env env, napi_callback_info info)
{
    napi_value argv[4];
    size_t argc = 4;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    if (argc < 4) {
        napi_throw_type_error (env, NULL,
                               "proxySteerable requires frontend, backend, capture, control");
        return NULL;
    }

    void *frontend = NULL;
    void *backend = NULL;
    void *capture = NULL;
    void *control = NULL;
    napi_get_value_external (env, argv[0], &frontend);
    napi_get_value_external (env, argv[1], &backend);
    napi_valuetype capture_type = napi_undefined;
    napi_typeof (env, argv[2], &capture_type);
    if (capture_type != napi_undefined && capture_type != napi_null)
        napi_get_value_external (env, argv[2], &capture);
    napi_get_value_external (env, argv[3], &control);

    if (zlink_proxy_steerable (frontend, backend, capture, control) != ZLINK_CONFIG_OK)
        return throw_last_error (env, "proxySteerable failed");
    napi_value out;
    napi_get_undefined (env, &out);
    return out;
}

napi_value sleep (napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    int32_t seconds = 0;
    if (argc >= 1)
        napi_get_value_int32 (env, argv[0], &seconds);

    zlink_sleep (seconds);
    napi_value out;
    napi_get_undefined (env, &out);
    return out;
}

napi_value ctx_new (napi_env env, napi_callback_info info)
{
    void *ctx = zlink_ctx_new ();
    if (!ctx)
        return throw_last_error (env, "ctx_new failed");
    napi_value ext;
    napi_create_external (env, ctx, NULL, NULL, &ext);
    return ext;
}

napi_value ctx_shutdown (napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *ctx = NULL;
    napi_get_value_external (env, argv[0], &ctx);
    int rc = zlink_ctx_shutdown (ctx);
    if (rc != 0)
        return throw_last_error (env, "ctx_shutdown failed");
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value ctx_term (napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *ctx = NULL;
    napi_get_value_external (env, argv[0], &ctx);
    int rc;
    do {
        rc = zlink_ctx_term (ctx);
    } while (rc != 0 && zlink_errno () == EINTR);
    if (rc != 0)
        return throw_last_error (env, "ctx_term failed");
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value ctx_setopt (napi_env env, napi_callback_info info)
{
    napi_value argv[3];
    size_t argc = 3;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *ctx = NULL;
    napi_get_value_external (env, argv[0], &ctx);
    int32_t opt = 0;
    int32_t value = 0;
    napi_get_value_int32 (env, argv[1], &opt);
    bool is_buffer = false;
    napi_is_buffer (env, argv[2], &is_buffer);
    if (is_buffer) {
        void *data = NULL;
        size_t length = 0;
        napi_get_buffer_info (env, argv[2], &data, &length);
        zlink_config_result_t rc =
          zlink_ctx_set_data (ctx, static_cast<zlink_ctx_option_t> (opt), data, length);
        if (rc != ZLINK_CONFIG_OK)
            return throw_last_error (env, "ctx_setopt failed");
        napi_value ok;
        napi_get_undefined (env, &ok);
        return ok;
    }
    napi_get_value_int32 (env, argv[2], &value);
    int rc = zlink_ctx_set (ctx, static_cast<zlink_ctx_option_t> (opt), value);
    if (rc != 0)
        return throw_last_error (env, "ctx_setopt failed");
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value ctx_getopt (napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *ctx = NULL;
    napi_get_value_external (env, argv[0], &ctx);
    int32_t opt = 0;
    napi_get_value_int32 (env, argv[1], &opt);
    zlink_config_result_t err = ZLINK_CONFIG_OK;
    const int rc = zlink_ctx_get (ctx, static_cast<zlink_ctx_option_t> (opt), &err);
    if (err != ZLINK_CONFIG_OK)
        return throw_last_error (env, "ctx_getopt failed");
    napi_value out;
    napi_create_int32 (env, rc, &out);
    return out;
}

napi_value ctx_getopt_data (napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *ctx = NULL;
    napi_get_value_external (env, argv[0], &ctx);
    int32_t opt = 0;
    napi_get_value_int32 (env, argv[1], &opt);
    uint64_t value = 0;
    size_t size = sizeof (value);
    const zlink_config_result_t rc = zlink_ctx_get_data (
      ctx, static_cast<zlink_ctx_option_t> (opt), &value, &size);
    if (rc != ZLINK_CONFIG_OK)
        return throw_last_error (env, "ctx_getopt_data failed");
    if (size != sizeof (value)) {
        napi_throw_error (env, NULL, "ctx_getopt_data returned an unexpected value size");
        return NULL;
    }
    napi_value out;
    napi_create_buffer_copy (env, sizeof (value), &value, NULL, &out);
    return out;
}

napi_value ctx_recalculate_auto_hwm (napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *ctx = NULL;
    napi_get_value_external (env, argv[0], &ctx);
    zlink_config_result_t rc = zlink_ctx_auto_hwm_recalculate (ctx);
    if (rc != ZLINK_CONFIG_OK)
        return throw_last_error (env, "ctx_auto_hwm_recalculate failed");
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value socket_new (napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *ctx = NULL;
    int32_t type = 0;
    napi_get_value_external (env, argv[0], &ctx);
    napi_get_value_int32 (env, argv[1], &type);
    void *sock = zlink_socket (ctx, translate_socket_type (type));
    if (!sock)
        return throw_last_error (env, "socket failed");
    napi_value ext;
    napi_create_external (env, sock, NULL, NULL, &ext);
    return ext;
}

napi_value socket_close (napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *sock = NULL;
    napi_get_value_external (env, argv[0], &sock);
    int rc = zlink_close (sock);
    if (rc != 0)
        return throw_last_error (env, "close failed");
    stream_release_slot (sock);
    release_socket_send_ready_handler_slot (sock);
    release_socket_completion_control_handler_slot (sock);
    release_socket_request_dispatcher (sock);
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value socket_request_completion_handler (napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    if (argc < 2) {
        napi_throw_type_error (env, NULL, "socketRequestCompletionHandler requires socket and handler");
        return NULL;
    }
    void *socket = NULL;
    napi_get_value_external (env, argv[0], &socket);
    napi_valuetype handler_type = napi_undefined;
    napi_typeof (env, argv[1], &handler_type);
    if (handler_type != napi_function) {
        napi_throw_type_error (env, NULL, "request completion handler must be a function");
        return NULL;
    }
    if (!set_socket_request_completion_handler (env, socket, argv[1]))
        return NULL;
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value socket_bind (napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *sock = NULL;
    napi_get_value_external (env, argv[0], &sock);
    std::string addr = get_string (env, argv[1]);
    int rc = zlink_bind (sock, addr.c_str ());
    if (rc != 0) {
        char buf[128];
        snprintf (buf, sizeof (buf), "bind failed (result=%d)", rc);
        napi_throw_error (env, NULL, buf);
        return NULL;
    }
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value socket_unbind (napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *sock = NULL;
    napi_get_value_external (env, argv[0], &sock);
    std::string addr = get_string (env, argv[1]);
    int rc = zlink_unbind (sock, addr.c_str ());
    if (rc != 0)
        return throw_last_error (env, "unbind failed");
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value socket_connect (napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *sock = NULL;
    napi_get_value_external (env, argv[0], &sock);
    std::string addr = get_string (env, argv[1]);
    int rc = zlink_connect (sock, addr.c_str ());
    if (rc != 0)
        return throw_last_error (env, "connect failed");
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value socket_disconnect (napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *sock = NULL;
    napi_get_value_external (env, argv[0], &sock);
    std::string addr = get_string (env, argv[1]);
    int rc = zlink_disconnect (sock, addr.c_str ());
    if (rc != 0)
        return throw_last_error (env, "disconnect failed");
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value socket_disconnect_rid (napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *sock = NULL;
    napi_get_value_external (env, argv[0], &sock);
    zlink_routing_id_t peer_rid;
    if (!parse_routing_id_value (env, argv[1], &peer_rid))
        return NULL;
    int rc = zlink_disconnect_rid (sock, &peer_rid);
    if (rc != 0)
        return throw_last_error (env, "disconnect_rid failed");
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value socket_disconnect_transport_pair (napi_env env, napi_callback_info info)
{
    napi_value argv[3];
    size_t argc = 3;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *sock = NULL;
    napi_get_value_external (env, argv[0], &sock);
    uint64_t pair_id = 0;
    uint64_t pair_generation = 0;
    bool pair_id_lossless = false;
    bool pair_generation_lossless = false;
    if (napi_get_value_bigint_uint64 (
          env, argv[1], &pair_id, &pair_id_lossless) != napi_ok
        || napi_get_value_bigint_uint64 (
             env, argv[2], &pair_generation, &pair_generation_lossless) != napi_ok
        || !pair_id_lossless || !pair_generation_lossless) {
        napi_throw_type_error (env, NULL, "transport pair identity must use bigint values");
        return NULL;
    }
    int rc = zlink_disconnect_transport_pair (sock, pair_id, pair_generation);
    if (rc != 0)
        return throw_last_error (env, "disconnect transport pair failed");
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value socket_set_tls_server (napi_env env, napi_callback_info info)
{
    napi_value argv[4];
    size_t argc = 4;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *sock = NULL;
    napi_get_value_external (env, argv[0], &sock);
    std::string cert = get_string (env, argv[1]);
    std::string key = get_string (env, argv[2]);
    int32_t require_client = 0;
    if (argc >= 4)
        napi_get_value_int32 (env, argv[3], &require_client);
    int rc = zlink_set_tls_server (sock, cert.c_str (), key.c_str (), require_client);
    if (rc != 0)
        return throw_last_error (env, "socket_set_tls_server failed");
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value socket_set_tls_client (napi_env env, napi_callback_info info)
{
    napi_value argv[4];
    size_t argc = 4;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *sock = NULL;
    napi_get_value_external (env, argv[0], &sock);
    std::string ca = get_string (env, argv[1]);
    std::string host = get_string (env, argv[2]);
    int32_t trust = 0;
    if (argc >= 4)
        napi_get_value_int32 (env, argv[3], &trust);
    int rc = zlink_set_tls_client (sock, ca.c_str (), host.c_str (), trust);
    if (rc != 0)
        return throw_last_error (env, "socket_set_tls_client failed");
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}


napi_value socket_send (napi_env env, napi_callback_info info)
{
    napi_value argv[3];
    size_t argc = 3;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *sock = NULL;
    napi_get_value_external (env, argv[0], &sock);
    int32_t flags = 0;
    napi_get_value_int32 (env, argv[2], &flags);
    zlink_msg_t msg;
    if (!init_msg_from_value (env, argv[1], &msg))
        return throw_last_error (env, "send failed");
    const size_t len = zlink_msg_size (&msg);
    int rc = send_parts (sock, &msg, 1, static_cast<zlink_send_flags_t> (flags));
    if (rc != ZLINK_SUBMIT_OK)
        return throw_last_error (env, "send failed");
    consume_native_message_value (env, argv[1]);
    napi_value out;
    napi_create_int32 (env, static_cast<int32_t> (len), &out);
    return out;
}

napi_value socket_send_parts (napi_env env, napi_callback_info info)
{
    napi_value argv[3];
    size_t argc = 3;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *sock = NULL;
    napi_get_value_external (env, argv[0], &sock);

    std::vector<zlink_msg_t> parts;
    if (!build_msg_vector_or_single (env, argv[1], &parts))
        return NULL;

    int32_t flags = 0;
    napi_get_value_int32 (env, argv[2], &flags);
    size_t total = 0;
    for (size_t i = 0; i < parts.size (); ++i)
        total += zlink_msg_size (&parts[i]);
    int rc =
      send_parts (sock, parts.data (), parts.size (), static_cast<zlink_send_flags_t> (flags));
    if (rc != ZLINK_SUBMIT_OK) {
        return throw_last_error (env, "sendParts failed");
    }
    consume_native_message_value (env, argv[1]);

    napi_value out;
    napi_create_int32 (env, static_cast<int32_t> (total), &out);
    return out;
}

napi_value socket_publish (napi_env env, napi_callback_info info)
{
    napi_value argv[4];
    size_t argc = 4;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *sock = NULL;
    napi_get_value_external (env, argv[0], &sock);
    char topic_stack[256];
    std::string topic_heap;
    const char *topic =
      get_c_string_arg (env, argv[1], topic_stack, sizeof (topic_stack), &topic_heap);

    std::vector<zlink_msg_t> parts;
    zlink_msg_t single_part;
    bool use_single_part = false;
    bool is_array = false;
    napi_is_array (env, argv[2], &is_array);
    bool is_buf = false;
    napi_valuetype payload_type = napi_undefined;
    napi_typeof (env, argv[2], &payload_type);
    if (is_array) {
        if (!build_msg_vector (env, argv[2], &parts))
            return NULL;
    } else if ((napi_is_buffer (env, argv[2], &is_buf) == napi_ok && is_buf)
               || payload_type == napi_object) {
        if (!init_msg_from_value (env, argv[2], &single_part))
            return throw_last_error (env, "publish failed");
        use_single_part = true;
    } else if (!build_msg_vector (env, argv[2], &parts)) {
        return NULL;
    }

    int32_t flags = 0;
    napi_get_value_int32 (env, argv[3], &flags);
    size_t total = 0;
    if (use_single_part) {
        total = zlink_msg_size (&single_part);
    } else {
        for (size_t i = 0; i < parts.size (); ++i)
            total += zlink_msg_size (&parts[i]);
    }
    int rc =
      publish_parts (sock, topic, use_single_part ? &single_part : parts.data (),
                     use_single_part ? 1 : parts.size (), static_cast<zlink_send_flags_t> (flags));
    if (rc != ZLINK_SUBMIT_OK) {
        return throw_last_error (env, "publish failed");
    }
    consume_native_message_value (env, argv[2]);

    napi_value out;
    napi_create_int32 (env, static_cast<int32_t> (total), &out);
    return out;
}

napi_value socket_try_publish (napi_env env, napi_callback_info info)
{
    napi_value argv[3];
    size_t argc = 3;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *sock = NULL;
    napi_get_value_external (env, argv[0], &sock);
    char topic_stack[256];
    std::string topic_heap;
    const char *topic =
      get_c_string_arg (env, argv[1], topic_stack, sizeof (topic_stack), &topic_heap);

    std::vector<zlink_msg_t> parts;
    zlink_msg_t single_part;
    bool use_single_part = false;
    bool is_array = false;
    napi_is_array (env, argv[2], &is_array);
    bool is_buf = false;
    napi_valuetype payload_type = napi_undefined;
    napi_typeof (env, argv[2], &payload_type);
    if (is_array) {
        if (!build_msg_vector (env, argv[2], &parts))
            return NULL;
    } else if (napi_is_buffer (env, argv[2], &is_buf) == napi_ok && is_buf) {
        void *data = NULL;
        size_t len = 0;
        if (napi_get_buffer_info (env, argv[2], &data, &len) != napi_ok) {
            napi_throw_type_error (env, NULL, "publish buffer invalid");
            return NULL;
        }
        if (!init_msg_from_bytes (&single_part, data, len))
            return throw_last_error (env, "publishNoWaitResult failed");
        use_single_part = true;
    } else if (payload_type == napi_object) {
        if (!init_msg_from_value (env, argv[2], &single_part))
            return throw_last_error (env, "publishNoWaitResult failed");
        use_single_part = true;
    } else if (!build_msg_vector (env, argv[2], &parts)) {
        return NULL;
    }

    int rc = publish_parts (sock, topic, use_single_part ? &single_part : parts.data (),
                            use_single_part ? 1 : parts.size (), ZLINK_SEND_FLAGS_DONTWAIT);
    if (rc != ZLINK_SUBMIT_OK)
        rc = classify_try_send_errno ();
    if (rc < 0) {
        return throw_last_error (env, "publishNoWaitResult failed");
    }
    if (rc == ZLINK_SUBMIT_OK)
        consume_native_message_value (env, argv[2]);
    napi_value out;
    napi_create_int32 (env, rc, &out);
    return out;
}

napi_value socket_try_send (napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *sock = NULL;
    napi_get_value_external (env, argv[0], &sock);
    zlink_msg_t msg;
    bool contains_native_frame = false;
    if (!init_msg_from_value (env, argv[1], &msg, &contains_native_frame))
        return throw_last_error (env, "sendNoWaitResult failed");
    int rc = send_parts (sock, &msg, 1, ZLINK_SEND_FLAGS_DONTWAIT);
    if (rc != ZLINK_SUBMIT_OK)
        rc = classify_try_send_errno ();
    if (rc < 0)
        return throw_last_error (env, "sendNoWaitResult failed");
    if (rc == ZLINK_SUBMIT_OK && contains_native_frame)
        consume_native_message_value (env, argv[1]);
    napi_value out;
    napi_create_int32 (env, rc, &out);
    return out;
}

napi_value socket_try_send_parts (napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *sock = NULL;
    napi_get_value_external (env, argv[0], &sock);

    std::vector<zlink_msg_t> parts;
    if (!build_msg_vector_or_single (env, argv[1], &parts))
        return NULL;

    int rc = send_parts (sock, parts.data (), parts.size (), ZLINK_SEND_FLAGS_DONTWAIT);
    if (rc != ZLINK_SUBMIT_OK)
        rc = classify_try_send_errno ();
    if (rc < 0) {
        return throw_last_error (env, "trySendParts failed");
    }
    if (rc == ZLINK_SUBMIT_OK)
        consume_native_message_value (env, argv[1]);
    napi_value out;
    napi_create_int32 (env, rc, &out);
    return out;
}

napi_value socket_try_send_routing (napi_env env, napi_callback_info info)
{
    napi_value argv[3];
    size_t argc = 3;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *sock = NULL;
    napi_get_value_external (env, argv[0], &sock);

    zlink_routing_id_t routing_id;
    if (!parse_routing_id_value (env, argv[1], &routing_id))
        return NULL;

    zlink_msg_t msg;
    if (!init_msg_from_value (env, argv[2], &msg))
        return throw_last_error (env, "trySendTo failed");
    int rc = send_parts_rid (sock, &routing_id, &msg, 1, ZLINK_SEND_FLAGS_DONTWAIT);
    if (rc != ZLINK_SUBMIT_OK)
        rc = classify_try_send_errno ();
    if (rc < 0)
        return throw_last_error (env, "trySendTo failed");
    if (rc == ZLINK_SUBMIT_OK)
        consume_native_message_value (env, argv[2]);
    napi_value out;
    napi_create_int32 (env, rc, &out);
    return out;
}

napi_value socket_try_send_routing_parts (napi_env env, napi_callback_info info)
{
    napi_value argv[3];
    size_t argc = 3;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *sock = NULL;
    napi_get_value_external (env, argv[0], &sock);

    zlink_routing_id_t routing_id;
    if (!parse_routing_id_value (env, argv[1], &routing_id))
        return NULL;

    std::vector<zlink_msg_t> parts;
    if (!build_msg_vector_or_single (env, argv[2], &parts))
        return NULL;

    int rc =
      send_parts_rid (sock, &routing_id, parts.data (), parts.size (), ZLINK_SEND_FLAGS_DONTWAIT);
    if (rc != ZLINK_SUBMIT_OK)
        rc = classify_try_send_errno ();
    if (rc < 0) {
        return throw_last_error (env, "trySendPartsTo failed");
    }
    if (rc == ZLINK_SUBMIT_OK)
        consume_native_message_value (env, argv[2]);
    napi_value out;
    napi_create_int32 (env, rc, &out);
    return out;
}

napi_value socket_stream_try_send_routing_parts (napi_env env, napi_callback_info info)
{
    napi_value argv[3];
    size_t argc = 3;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *sock = NULL;
    napi_get_value_external (env, argv[0], &sock);

    zlink_routing_id_t routing_id;
    if (!parse_routing_id_value (env, argv[1], &routing_id))
        return NULL;

    zlink_msg_t msg;
    if (!init_contiguous_msg_from_array (env, argv[2], &msg))
        return NULL;

    int rc = send_parts_rid (sock, &routing_id, &msg, 1, ZLINK_SEND_FLAGS_DONTWAIT);
    // Core consumes the temporary message only on successful submission.
    // Closing an already-consumed message is safe and releases the allocation
    // when backpressure or another error leaves ownership with this addon.
    zlink_msg_close (&msg);
    if (rc != ZLINK_SUBMIT_OK)
        rc = classify_try_send_errno ();
    if (rc < 0)
        return throw_last_error (env, "tryStreamSendPartsTo failed");
    if (rc == ZLINK_SUBMIT_OK)
        consume_native_message_value (env, argv[2]);
    napi_value out;
    napi_create_int32 (env, rc, &out);
    return out;
}

napi_value socket_send_routing (napi_env env, napi_callback_info info)
{
    napi_value argv[4];
    size_t argc = 4;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *sock = NULL;
    napi_get_value_external (env, argv[0], &sock);

    zlink_routing_id_t routing_id;
    if (!parse_routing_id_value (env, argv[1], &routing_id))
        return NULL;

    zlink_msg_t msg;
    if (!init_msg_from_value (env, argv[2], &msg))
        return throw_last_error (env, "send failed");
    const size_t len = zlink_msg_size (&msg);

    int32_t flags = 0;
    napi_get_value_int32 (env, argv[3], &flags);
    int rc = send_parts_rid (sock, &routing_id, &msg, 1, static_cast<zlink_send_flags_t> (flags));
    if (rc != ZLINK_SUBMIT_OK)
        return throw_last_error (env, "send failed");
    consume_native_message_value (env, argv[2]);

    napi_value out;
    napi_create_int32 (env, static_cast<int32_t> (len), &out);
    return out;
}

napi_value socket_send_routing_parts (napi_env env, napi_callback_info info)
{
    napi_value argv[4];
    size_t argc = 4;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *sock = NULL;
    napi_get_value_external (env, argv[0], &sock);

    zlink_routing_id_t routing_id;
    if (!parse_routing_id_value (env, argv[1], &routing_id))
        return NULL;

    std::vector<zlink_msg_t> parts;
    if (!build_msg_vector_or_single (env, argv[2], &parts))
        return NULL;

    int32_t flags = 0;
    napi_get_value_int32 (env, argv[3], &flags);
    int rc = send_parts_rid (sock, &routing_id, parts.data (), parts.size (),
                             static_cast<zlink_send_flags_t> (flags));
    if (rc != ZLINK_SUBMIT_OK)
        return throw_last_error (env, "sendPartsTo failed");
    consume_native_message_value (env, argv[2]);

    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value socket_stream_send_routing_parts (napi_env env, napi_callback_info info)
{
    napi_value argv[4];
    size_t argc = 4;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *sock = NULL;
    napi_get_value_external (env, argv[0], &sock);

    zlink_routing_id_t routing_id;
    if (!parse_routing_id_value (env, argv[1], &routing_id))
        return NULL;

    zlink_msg_t msg;
    if (!init_contiguous_msg_from_array (env, argv[2], &msg))
        return NULL;

    int32_t flags = 0;
    napi_get_value_int32 (env, argv[3], &flags);
    int rc = send_parts_rid (sock, &routing_id, &msg, 1,
                             static_cast<zlink_send_flags_t> (flags));
    zlink_msg_close (&msg);
    if (rc != ZLINK_SUBMIT_OK)
        return throw_last_error (env, "streamSendPartsTo failed");
    consume_native_message_value (env, argv[2]);

    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

int recv_message_value (napi_env env,
                        void *sock,
                        int32_t flags,
                        napi_value *out,
                        size_t *received_bytes = NULL)
{
    if (!out)
        return ZLINK_RECV_INTERNAL_ERROR;

    *out = NULL;
    zlink_routing_id_t routing_id;
    zlink_msg_t first_part;
    if (zlink_msg_init (&first_part) != 0)
        return ZLINK_RECV_INTERNAL_ERROR;

    const zlink_routing_id_t *source_rid = NULL;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    const int rc = zlink_recv_part (sock, &source_rid, &first_part, &has_more,
                                    static_cast<zlink_recv_flags_t> (flags));
    if (rc != ZLINK_RECV_OK) {
        zlink_msg_close (&first_part);
        return rc;
    }

    copy_routing_id (&routing_id, source_rid);
    if (has_more == ZLINK_PART_FINAL) {
        if (received_bytes)
            *received_bytes = zlink_msg_size (&first_part);
        if (routing_id.size == 0) {
            *out = create_received_message_buffer (env, &first_part);
            zlink_msg_close (&first_part);
            return *out ? ZLINK_RECV_OK : ZLINK_RECV_INTERNAL_ERROR;
        }
        *out = create_recv_message_value (env, routing_id, &first_part, 1);
        zlink_msg_close (&first_part);
        return *out ? ZLINK_RECV_OK : ZLINK_RECV_INTERNAL_ERROR;
    }

    std::vector<zlink_msg_t> parts;
    const int collect_rc = collect_recv_parts (sock, &first_part, has_more, &parts);
    if (collect_rc != ZLINK_RECV_OK)
        return collect_rc;
    if (received_bytes) {
        *received_bytes = 0;
        for (size_t index = 0; index < parts.size (); ++index)
            *received_bytes += zlink_msg_size (&parts[index]);
    }
    *out = create_recv_message_value (env, routing_id, parts.data (), parts.size ());
    close_msg_vector (parts);
    return *out ? ZLINK_RECV_OK : ZLINK_RECV_INTERNAL_ERROR;
}

napi_value socket_recv_message (napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *sock = NULL;
    napi_get_value_external (env, argv[0], &sock);
    int32_t flags = 0;
    if (argc >= 2)
        napi_get_value_int32 (env, argv[1], &flags);

    napi_value out;
    int rc = recv_message_value (env, sock, flags, &out);
    if (rc != ZLINK_RECV_OK)
        return throw_last_error (env, "recv failed");
    return out;
}

napi_value socket_try_recv_message (napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *sock = NULL;
    napi_get_value_external (env, argv[0], &sock);

    napi_value out;
    int rc = recv_message_value (env, sock, static_cast<int32_t> (ZLINK_RECV_FLAGS_DONTWAIT),
                                 &out);
    if (rc != ZLINK_RECV_OK) {
        if (zlink_errno () == EAGAIN) {
            napi_value none;
            napi_get_null (env, &none);
            return none;
        }
        return throw_last_error (env, "tryReceive failed");
    }
    return out;
}

class subscribe_topic_buffer_t
{
  public:
    subscribe_topic_buffer_t () : data_ (stack_), size_ (sizeof (stack_)) {}

    char *data () { return data_; }
    size_t size () const { return size_; }

    void resize (size_t required_size)
    {
        heap_.assign (required_size > 0 ? required_size : 1, '\0');
        data_ = heap_.data ();
        size_ = heap_.size ();
    }

  private:
    char stack_[256] = {};
    std::vector<char> heap_;
    char *data_;
    size_t size_;
};

napi_value socket_subscribe_message (napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *sock = NULL;
    napi_get_value_external (env, argv[0], &sock);

    subscribe_topic_buffer_t topic;
    zlink_routing_id_t routing_id;
    std::vector<zlink_msg_t> parts;
    size_t topic_len = topic.size ();

    for (;;) {
        memset (&routing_id, 0, sizeof (routing_id));
        int rc = subscribe_parts (sock, &routing_id, topic.data (), topic.size (), &topic_len,
                                  &parts, ZLINK_RECV_FLAGS_NONE);
        if (rc == ZLINK_RECV_OK) {
            napi_value out = create_subscribed_value (env, routing_id, topic.data (), topic_len,
                                                      parts.data (), parts.size ());
            close_msg_vector (parts);
            return out;
        }
        if (zlink_errno () != EMSGSIZE)
            return throw_last_error (env, "subscribe failed");
        topic.resize (topic_len);
    }
}

int try_subscribe_message_value (napi_env env,
                                 void *sock,
                                 napi_value *out,
                                 size_t *received_bytes = NULL)
{
    subscribe_topic_buffer_t topic;
    zlink_routing_id_t routing_id;
    size_t topic_len = topic.size ();

    for (;;) {
        memset (&routing_id, 0, sizeof (routing_id));
        const zlink_routing_id_t *source_rid = NULL;
        zlink_msg_t first_part;
        if (zlink_msg_init (&first_part) != 0)
            return ZLINK_RECV_INTERNAL_ERROR;
        zlink_part_flag_t has_more = ZLINK_PART_FINAL;
        int rc = zlink_subscribe_part (sock, &source_rid, topic.data (), topic.size (), &topic_len,
                                       &first_part, &has_more, ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc == ZLINK_RECV_OK) {
            copy_routing_id (&routing_id, source_rid);
            if (!has_more) {
                if (received_bytes)
                    *received_bytes = zlink_msg_size (&first_part);
                *out = create_subscribed_value (env, routing_id, topic.data (), topic_len,
                                                &first_part, 1);
                zlink_msg_close (&first_part);
                return *out ? ZLINK_RECV_OK : ZLINK_RECV_INTERNAL_ERROR;
            }

            std::vector<zlink_msg_t> parts;
            rc = collect_recv_parts (sock, &first_part, has_more, &parts);
            if (rc == ZLINK_RECV_OK) {
                if (received_bytes) {
                    *received_bytes = 0;
                    for (size_t index = 0; index < parts.size (); ++index)
                        *received_bytes += zlink_msg_size (&parts[index]);
                }
                *out = create_subscribed_value (env, routing_id, topic.data (), topic_len,
                                                parts.data (), parts.size ());
                close_msg_vector (parts);
                return *out ? ZLINK_RECV_OK : ZLINK_RECV_INTERNAL_ERROR;
            }
        }
        const int err = zlink_errno ();
        zlink_msg_close (&first_part);
        if (err == EAGAIN)
            return rc;
        if (err != EMSGSIZE)
            return rc;
        topic.resize (topic_len);
    }
}

napi_value socket_try_subscribe_message (napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *sock = NULL;
    napi_get_value_external (env, argv[0], &sock);
    napi_value out = NULL;
    const int rc = try_subscribe_message_value (env, sock, &out);
    if (rc == ZLINK_RECV_OK)
        return out;
    if (zlink_errno () == EAGAIN) {
        napi_value none;
        napi_get_null (env, &none);
        return none;
    }
    return throw_last_error (env, "subscribeNoWait failed");
}

napi_value socket_send_ready_handler (napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    if (argc < 2) {
        napi_throw_type_error (env, NULL, "sendReadyHandler requires (socket, handler)");
        return NULL;
    }
    void *sock = NULL;
    napi_get_value_external (env, argv[0], &sock);
    napi_valuetype handler_type = napi_undefined;
    napi_typeof (env, argv[1], &handler_type);
    if (handler_type != napi_function) {
        napi_throw_type_error (env, NULL, "sendReadyHandler handler must be a function");
        return NULL;
    }
    if (!attach_send_ready_handler (env, sock, argv[1]))
        return NULL;
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value socket_subscription_event (napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *sock = NULL;
    napi_get_value_external (env, argv[0], &sock);

    std::vector<char> topic (256, '\0');
    zlink_routing_id_t routing_id;
    int subscribed = 0;
    size_t topic_len = topic.size ();

    for (;;) {
        const zlink_routing_id_t *source_rid = NULL;
        memset (&routing_id, 0, sizeof (routing_id));
        int rc = zlink_xpub_recv_part (sock, &source_rid, &subscribed, topic.data (), topic.size (),
                                       &topic_len, ZLINK_RECV_FLAGS_NONE);
        if (rc == ZLINK_RECV_OK) {
            copy_routing_id (&routing_id, source_rid);
            return create_subscription_event_value (env, routing_id, subscribed, topic.data (),
                                                    topic_len);
        }
        if (zlink_errno () != EMSGSIZE)
            return throw_last_error (env, "receiveSubscriptionEvent failed");
        topic.assign (topic_len > 0 ? topic_len : 1, '\0');
    }
}

napi_value socket_try_subscription_event (napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *sock = NULL;
    napi_get_value_external (env, argv[0], &sock);

    std::vector<char> topic (256, '\0');
    zlink_routing_id_t routing_id;
    int subscribed = 0;
    size_t topic_len = topic.size ();

    for (;;) {
        const zlink_routing_id_t *source_rid = NULL;
        memset (&routing_id, 0, sizeof (routing_id));
        int rc = zlink_xpub_recv_part (sock, &source_rid, &subscribed, topic.data (), topic.size (),
                                       &topic_len, ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc == ZLINK_RECV_OK) {
            copy_routing_id (&routing_id, source_rid);
            return create_subscription_event_value (env, routing_id, subscribed, topic.data (),
                                                    topic_len);
        }
        const int err = zlink_errno ();
        if (err == EAGAIN) {
            napi_value none;
            napi_get_null (env, &none);
            return none;
        }
        if (err != EMSGSIZE)
            return throw_last_error (env, "tryReceiveSubscriptionEvent failed");
        topic.assign (topic_len > 0 ? topic_len : 1, '\0');
    }
}

napi_value subscription_at (napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    if (argc < 2) {
        napi_throw_type_error (env, NULL, "subscriptionAt expects handle, index");
        return NULL;
    }

    void *handle = NULL;
    napi_get_value_external (env, argv[0], &handle);
    uint32_t index = 0;
    napi_get_value_uint32 (env, argv[1], &index);

    std::vector<char> filter (256, '\0');
    size_t filter_len = filter.size ();
    int is_pattern = 0;
    for (;;) {
        zlink_config_result_t rc = zlink_subscription_at (handle, static_cast<size_t> (index),
                                                          filter.data (), &filter_len, &is_pattern);
        if (rc == ZLINK_CONFIG_OK) {
            napi_value obj;
            napi_create_object (env, &obj);
            napi_value filter_value;
            napi_create_string_utf8 (env, filter.data (), filter_len, &filter_value);
            napi_set_named_property (env, obj, "filter", filter_value);
            napi_value pattern_value;
            napi_get_boolean (env, is_pattern != 0, &pattern_value);
            napi_set_named_property (env, obj, "isPattern", pattern_value);
            return obj;
        }
        if (rc == ZLINK_CONFIG_NOT_FOUND) {
            napi_value none;
            napi_get_null (env, &none);
            return none;
        }
        if (zlink_errno () != EMSGSIZE)
            return throw_last_error (env, "subscription_at failed");
        filter.assign (filter_len > 0 ? filter_len : 1, '\0');
    }
}

napi_value socket_stream_attach (napi_env env, napi_callback_info info)
{
    napi_value argv[4];
    size_t argc = 4;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    if (argc < 2) {
        napi_throw_type_error (
          env, NULL,
          "streamAttach requires (socket, handler[, mode[, bodyMaterialization]])");
        return NULL;
    }

    void *sock = NULL;
    napi_get_value_external (env, argv[0], &sock);

    napi_valuetype handler_type = napi_undefined;
    napi_typeof (env, argv[1], &handler_type);
    if (handler_type != napi_function) {
        napi_throw_type_error (env, NULL, "streamAttach handler must be a function");
        return NULL;
    }

    int32_t mode = k_stream_dispatch_len32be;
    if (argc >= 3)
        napi_get_value_int32 (env, argv[2], &mode);
    if (mode != k_stream_dispatch_len32be) {
        napi_throw_range_error (env, NULL, "streamAttach mode must be PACKET(1)");
        return NULL;
    }

    int32_t body_materialization = 0;
    if (argc >= 4)
        napi_get_value_int32 (env, argv[3], &body_materialization);
    if (body_materialization != 0 && body_materialization != 1) {
        napi_throw_range_error (env, NULL,
                                "streamAttach body materialization must be Native(0) or Managed(1)");
        return NULL;
    }

    size_t slot_index = 0;
    stream_js_state_t *slot =
      reserve_tsfn_subject_slot (env, g_stream_slots_mu, g_stream_slots, k_stream_slot_count,
                                 &stream_js_state_t::socket, sock, "STREAM callback already attached",
                                 "no free STREAM callback slot (max 8 attached sockets)",
                                 &slot_index);
    if (!slot)
        return NULL;

    napi_threadsafe_function tsfn = NULL;
    if (!create_tsfn_slot_queue (env, argv[1], slot, "zlink-stream", stream_tsfn_finalize,
                                 stream_tsfn_call_js,
                                 "streamAttach failed to create callback queue", true, &tsfn))
        return NULL;

    {
        std::lock_guard<std::mutex> lock (g_stream_slots_mu);
        bind_tsfn_subject_slot_unsafe (slot, &stream_js_state_t::socket, sock, env, tsfn);
        slot->stop_requested.store (0, std::memory_order_release);
        slot->body_materialization = body_materialization;
    }

    zlink_handler_result_t rc =
      zlink_stream_packet_handler (sock, g_stream_slot_packet_callbacks[slot_index], NULL);
    if (rc != ZLINK_HANDLER_OK) {
        stream_release_slot (sock);
        return throw_last_error (env, "streamAttach failed");
    }

    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value socket_setopt (napi_env env, napi_callback_info info)
{
    napi_value argv[3];
    size_t argc = 3;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *sock = NULL;
    napi_get_value_external (env, argv[0], &sock);
    int32_t opt = 0;
    napi_get_value_int32 (env, argv[1], &opt);
    void *data = NULL;
    size_t len = 0;
    if (napi_get_buffer_info (env, argv[2], &data, &len) != napi_ok) {
        napi_throw_type_error (env, NULL, "option value must be Buffer");
        return NULL;
    }
    int rc = set_socket_option (sock, opt, data, len);
    if (rc != 0)
        return throw_last_error (env, "setsockopt failed");
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value socket_getopt (napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *sock = NULL;
    napi_get_value_external (env, argv[0], &sock);
    int32_t opt = 0;
    napi_get_value_int32 (env, argv[1], &opt);
    size_t len = initial_getopt_buffer_len (opt);
    void *data = NULL;
    napi_value buf;
    napi_create_buffer (env, len, &data, &buf);
    int rc = get_socket_option (sock, opt, data, &len);
    if (rc != 0)
        return throw_last_error (env, "getsockopt failed");
    if (len == initial_getopt_buffer_len (opt))
        return buf;
    napi_value out;
    napi_create_buffer_copy (env, len, data, NULL, &out);
    return out;
}

napi_value socket_set_subscription (napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *sock = NULL;
    napi_get_value_external (env, argv[0], &sock);
    char topic_stack[256];
    std::string topic_heap;
    const char *topic =
      get_c_string_arg (env, argv[1], topic_stack, sizeof (topic_stack), &topic_heap);
    int rc = zlink_set_subscription (sock, topic);
    if (rc != 0)
        return throw_last_error (env, "set subscription failed");
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value socket_unset_subscription (napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *sock = NULL;
    napi_get_value_external (env, argv[0], &sock);
    char topic_stack[256];
    std::string topic_heap;
    const char *topic =
      get_c_string_arg (env, argv[1], topic_stack, sizeof (topic_stack), &topic_heap);
    int rc = zlink_unset_subscription (sock, topic);
    if (rc != 0)
        return throw_last_error (env, "unset subscription failed");
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value handle_set_routing_id (napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *handle = NULL;
    napi_get_value_external (env, argv[0], &handle);
    zlink_routing_id_t routing_id;
    if (!parse_routing_id_value (env, argv[1], &routing_id))
        return NULL;
    int rc = zlink_set_routing_id (handle, routing_id.data, routing_id.size);
    if (rc != 0)
        return throw_last_error (env, "set_routing_id failed");
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value handle_get_routing_id (napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *handle = NULL;
    napi_get_value_external (env, argv[0], &handle);
    zlink_routing_id_t routing_id;
    memset (&routing_id, 0, sizeof (routing_id));
    int rc = zlink_get_routing_id (handle, &routing_id);
    if (rc != 0)
        return throw_last_error (env, "get_routing_id failed");
    return create_routing_id_value (env, routing_id);
}

napi_value dealer_request (napi_env env, napi_callback_info info)
{
    napi_value argv[5];
    size_t argc = 5;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    if (argc < 5) {
        napi_throw_type_error (env, NULL,
                               "dealerRequest requires (socket, parts, handler, flags, timeoutMs)");
        return NULL;
    }
    void *dealer = NULL;
    napi_get_value_external (env, argv[0], &dealer);
    std::vector<zlink_msg_t> parts;
    if (!build_msg_vector_or_single (env, argv[1], &parts))
        return NULL;
    int32_t flags = 0;
    napi_get_value_int32 (env, argv[3], &flags);
    int32_t timeout_ms = 0;
    napi_get_value_int32 (env, argv[4], &timeout_ms);
    uint64_t token = 0;
    if (!get_uint64_like (env, argv[2], &token))
        return NULL;
    request_js_state_t *state = create_core_request_js_state (env, dealer, token);
    if (!state) {
        close_msg_vector (parts);
        return NULL;
    }
    int rc = dealer_request_parts (
      dealer, parts.data (), parts.size (), request_reply_callback_trampoline, state,
      static_cast<zlink_send_flags_t> (flags), static_cast<uint32_t> (timeout_ms));
    if (rc != ZLINK_SUBMIT_OK) {
        abort_request_js_state (state);
        return throw_last_error (env, "dealerRequest failed");
    }
    consume_native_message_value (env, argv[1]);
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value router_request (napi_env env, napi_callback_info info)
{
    napi_value argv[6];
    size_t argc = 6;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    if (argc < 6) {
        napi_throw_type_error (
          env, NULL,
          "routerRequest requires (socket, routingId, parts, handler, flags, timeoutMs)");
        return NULL;
    }
    void *router = NULL;
    napi_get_value_external (env, argv[0], &router);
    zlink_routing_id_t peer_rid;
    if (!parse_routing_id_value (env, argv[1], &peer_rid))
        return NULL;
    std::vector<zlink_msg_t> parts;
    if (!build_msg_vector_or_single (env, argv[2], &parts))
        return NULL;
    napi_valuetype handler_type = napi_undefined;
    napi_typeof (env, argv[3], &handler_type);
    uint64_t token = 0;
    if (!get_uint64_like (env, argv[3], &token)) {
        close_msg_vector (parts);
        return NULL;
    }
    int32_t flags = 0;
    napi_get_value_int32 (env, argv[4], &flags);
    int32_t timeout_ms = 0;
    napi_get_value_int32 (env, argv[5], &timeout_ms);
    request_js_state_t *state = create_core_request_js_state (env, router, token);
    if (!state) {
        close_msg_vector (parts);
        return NULL;
    }
    int rc = router_request_parts (
      router, &peer_rid, parts.data (), parts.size (), request_reply_callback_trampoline, state,
      static_cast<zlink_send_flags_t> (flags), static_cast<uint32_t> (timeout_ms));
    if (rc != ZLINK_SUBMIT_OK) {
        abort_request_js_state (state);
        return throw_last_error (env, "routerRequest failed");
    }
    consume_native_message_value (env, argv[2]);
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value router_request_transport_pair (napi_env env, napi_callback_info info)
{
    napi_value argv[8];
    size_t argc = 8;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    if (argc < 8) {
        napi_throw_type_error (
          env, NULL,
          "routerRequestTransportPair requires (socket, routingId, pairId, pairGeneration, parts, handler, flags, timeoutMs)");
        return NULL;
    }
    void *router = NULL;
    napi_get_value_external (env, argv[0], &router);
    zlink_routing_id_t peer_rid;
    if (!parse_routing_id_value (env, argv[1], &peer_rid))
        return NULL;
    uint64_t pair_id = 0;
    uint64_t pair_generation = 0;
    if (!get_uint64_like (env, argv[2], &pair_id)
        || !get_uint64_like (env, argv[3], &pair_generation)
        || pair_id == 0 || pair_generation == 0) {
        napi_throw_type_error (env, NULL, "transport pair identity must be non-zero uint64 values");
        return NULL;
    }
    std::vector<zlink_msg_t> parts;
    if (!build_msg_vector_or_single (env, argv[4], &parts))
        return NULL;
    napi_valuetype handler_type = napi_undefined;
    napi_typeof (env, argv[5], &handler_type);
    uint64_t token = 0;
    if (!get_uint64_like (env, argv[5], &token)) {
        close_msg_vector (parts);
        return NULL;
    }
    int32_t flags = 0;
    int32_t timeout_ms = 0;
    napi_get_value_int32 (env, argv[6], &flags);
    napi_get_value_int32 (env, argv[7], &timeout_ms);
    request_js_state_t *state = create_core_request_js_state (env, router, token);
    if (!state) {
        close_msg_vector (parts);
        return NULL;
    }
    int rc = router_request_transport_pair_parts (
      router, &peer_rid, pair_id, pair_generation, parts.data (), parts.size (),
      request_reply_callback_trampoline, state,
      static_cast<zlink_send_flags_t> (flags), static_cast<uint32_t> (timeout_ms));
    if (rc != ZLINK_SUBMIT_OK) {
        abort_request_js_state (state);
        return throw_last_error (env, "routerRequestTransportPair failed");
    }
    consume_native_message_value (env, argv[4]);
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value router_send_transport_pair (napi_env env, napi_callback_info info)
{
    napi_value argv[6];
    size_t argc = 6;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    if (argc < 6) {
        napi_throw_type_error (
          env, NULL,
          "routerSendTransportPair requires (socket, routingId, pairId, pairGeneration, parts, flags)");
        return NULL;
    }
    void *router = NULL;
    napi_get_value_external (env, argv[0], &router);
    zlink_routing_id_t peer_rid;
    if (!parse_routing_id_value (env, argv[1], &peer_rid))
        return NULL;
    uint64_t pair_id = 0;
    uint64_t pair_generation = 0;
    if (!get_uint64_like (env, argv[2], &pair_id)
        || !get_uint64_like (env, argv[3], &pair_generation)
        || pair_id == 0 || pair_generation == 0) {
        napi_throw_type_error (env, NULL, "transport pair identity must be non-zero uint64 values");
        return NULL;
    }
    std::vector<zlink_msg_t> parts;
    if (!build_msg_vector_or_single (env, argv[4], &parts))
        return NULL;
    int32_t flags = 0;
    napi_get_value_int32 (env, argv[5], &flags);
    int rc = submit_msg_parts (
      parts.data (), parts.size (),
      [router, &peer_rid, pair_id, pair_generation, flags] (
        zlink_msg_t *part, zlink_part_flag_t part_flag, bool) {
          return zlink_send_part_transport_pair (
            router, &peer_rid, pair_id, pair_generation, part,
            static_cast<zlink_send_flags_t> (flags), part_flag);
      });
    parts.clear ();
    if (rc != ZLINK_SUBMIT_OK)
        return throw_last_error (env, "routerSendTransportPair failed");
    consume_native_message_value (env, argv[4]);
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value router_reply (napi_env env, napi_callback_info info)
{
    napi_value argv[4];
    size_t argc = 4;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    if (argc < 4) {
        napi_throw_type_error (env, NULL,
                               "routerReply requires (socket, routingId, requestSeq, parts)");
        return NULL;
    }
    void *router = NULL;
    napi_get_value_external (env, argv[0], &router);
    zlink_routing_id_t peer_rid;
    if (!parse_routing_id_value (env, argv[1], &peer_rid))
        return NULL;
    uint64_t request_seq = 0;
    if (!get_uint64_like (env, argv[2], &request_seq)) {
        napi_throw_type_error (env, NULL, "requestSeq must be uint64");
        return NULL;
    }
    std::vector<zlink_msg_t> parts;
    zlink_msg_t single_part;
    bool use_single_part = false;
    bool is_array = false;
    if (napi_is_array (env, argv[3], &is_array) == napi_ok && is_array) {
        if (!build_msg_vector (env, argv[3], &parts))
            return NULL;
    } else {
        if (!init_msg_from_value (env, argv[3], &single_part))
            return NULL;
        use_single_part = true;
    }
    int rc = router_reply_parts (router, &peer_rid, request_seq,
                                 use_single_part ? &single_part : parts.data (),
                                 use_single_part ? 1 : parts.size ());
    if (rc != ZLINK_SUBMIT_OK) {
        return throw_last_error (env, "routerReply failed");
    }
    consume_native_message_value (env, argv[3]);
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value router_try_send_completion_control (
  napi_env env, napi_callback_info info)
{
    napi_value argv[3];
    size_t argc = 3;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    if (argc < 3) {
        napi_throw_type_error (
          env, NULL,
          "routerTrySendCompletionControl requires (socket, routingId, parts)");
        return NULL;
    }
    void *router = NULL;
    napi_get_value_external (env, argv[0], &router);
    zlink_routing_id_t peer_rid;
    if (!parse_routing_id_value (env, argv[1], &peer_rid))
        return NULL;
    std::vector<zlink_msg_t> parts;
    if (!build_msg_vector_or_single (env, argv[2], &parts))
        return NULL;
    if (parts.empty ()) {
        napi_throw_range_error (env, NULL, "parts must not be empty");
        return NULL;
    }

    int rc = submit_msg_parts (
      parts.data (), parts.size (),
      [&] (zlink_msg_t *part_, zlink_part_flag_t part_flag_, bool) {
          return zlink_router_completion_control_part (
            router, &peer_rid, part_, part_flag_);
      });
    parts.clear ();
    if (rc != ZLINK_SUBMIT_OK && rc != ZLINK_SUBMIT_BACKPRESSURED)
        return throw_last_error (env, "routerTrySendCompletionControl failed");
    if (rc == ZLINK_SUBMIT_OK)
        consume_native_message_value (env, argv[2]);

    napi_value result;
    napi_get_boolean (env, rc == ZLINK_SUBMIT_OK, &result);
    return result;
}

napi_value router_try_send_completion_control_transport_pair (
  napi_env env, napi_callback_info info)
{
    napi_value argv[5];
    size_t argc = 5;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    if (argc < 5) {
        napi_throw_type_error (env, NULL,
          "routerTrySendCompletionControlTransportPair requires (socket, routingId, pairId, pairGeneration, parts)");
        return NULL;
    }
    void *router = NULL;
    napi_get_value_external (env, argv[0], &router);
    zlink_routing_id_t peer_rid;
    if (!parse_routing_id_value (env, argv[1], &peer_rid))
        return NULL;
    uint64_t pair_id = 0;
    uint64_t pair_generation = 0;
    if (!get_uint64_like (env, argv[2], &pair_id)
        || !get_uint64_like (env, argv[3], &pair_generation)
        || pair_id == 0 || pair_generation == 0) {
        napi_throw_type_error (env, NULL, "transport pair identity must be non-zero uint64 values");
        return NULL;
    }
    std::vector<zlink_msg_t> parts;
    if (!build_msg_vector_or_single (env, argv[4], &parts))
        return NULL;
    if (parts.empty ()) {
        napi_throw_range_error (env, NULL, "parts must not be empty");
        return NULL;
    }
    int rc = submit_msg_parts (
      parts.data (), parts.size (),
      [router, &peer_rid, pair_id, pair_generation] (
        zlink_msg_t *part, zlink_part_flag_t part_flag, bool) {
          return zlink_router_completion_control_transport_pair_part (
            router, &peer_rid, pair_id, pair_generation, part, part_flag);
      });
    parts.clear ();
    if (rc != ZLINK_SUBMIT_OK && rc != ZLINK_SUBMIT_BACKPRESSURED)
        return throw_last_error (env, "routerTrySendCompletionControlTransportPair failed");
    if (rc == ZLINK_SUBMIT_OK)
        consume_native_message_value (env, argv[4]);
    napi_value result;
    napi_get_boolean (env, rc == ZLINK_SUBMIT_OK, &result);
    return result;
}

napi_value router_completion_control_handler (
  napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    if (argc < 2) {
        napi_throw_type_error (
          env, NULL,
          "routerCompletionControlHandler requires (socket, handler)");
        return NULL;
    }
    void *router = NULL;
    napi_get_value_external (env, argv[0], &router);
    napi_valuetype handler_type = napi_undefined;
    napi_typeof (env, argv[1], &handler_type);
    if (handler_type != napi_function) {
        napi_throw_type_error (
          env, NULL,
          "routerCompletionControlHandler handler must be a function");
        return NULL;
    }
    if (!attach_completion_control_handler (env, router, argv[1]))
        return NULL;
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value router_recv_message (napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *router = NULL;
    napi_get_value_external (env, argv[0], &router);
    int32_t flags = 0;
    if (argc >= 2)
        napi_get_value_int32 (env, argv[1], &flags);

    zlink_routing_id_t peer_rid;
    uint64_t request_seq = 0;
    uint64_t transport_pair_id = 0;
    uint64_t transport_pair_generation = 0;
    std::vector<zlink_msg_t> parts;
    int rc = router_recv_parts (router, &peer_rid, &request_seq, &parts, flags,
                                &transport_pair_id, &transport_pair_generation);
    if (rc != ZLINK_RECV_OK)
        return throw_last_error (env, "routerRecvMessage failed");

    napi_value out =
      create_router_recv_message_value (env, peer_rid, request_seq, transport_pair_id,
                                        transport_pair_generation, parts.data (), parts.size ());
    close_msg_vector (parts);
    return out;
}

napi_value router_try_recv_message (napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *router = NULL;
    napi_get_value_external (env, argv[0], &router);

    zlink_routing_id_t peer_rid;
    uint64_t request_seq = 0;
    uint64_t transport_pair_id = 0;
    uint64_t transport_pair_generation = 0;
    std::vector<zlink_msg_t> parts;
    int rc = router_recv_parts (router, &peer_rid, &request_seq, &parts,
                                ZLINK_RECV_FLAGS_DONTWAIT,
                                &transport_pair_id, &transport_pair_generation);
    if (rc != ZLINK_RECV_OK) {
        if (zlink_errno () == EAGAIN) {
            napi_value none;
            napi_get_null (env, &none);
            return none;
        }
        return throw_last_error (env, "routerRecvMessageNoWait failed");
    }

    napi_value out =
      create_router_recv_message_value (env, peer_rid, request_seq, transport_pair_id,
                                        transport_pair_generation, parts.data (), parts.size ());
    close_msg_vector (parts);
    return out;
}

napi_value monitor_open (napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *sock = NULL;
    napi_get_value_external (env, argv[0], &sock);
    int32_t events = 0;
    napi_get_value_int32 (env, argv[1], &events);
    zlink_socket_monitor_open_options_t options;
    options.events = static_cast<zlink_socket_monitor_event_mask_t> (events);
    void *mon = zlink_socket_monitor_open (sock, &options);
    if (!mon)
        return throw_last_error (env, "monitor_open failed");
    napi_value ext;
    napi_create_external (env, mon, NULL, NULL, &ext);
    return ext;
}

napi_value monitor_handler (napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *mon = NULL;
    napi_get_value_external (env, argv[0], &mon);

    napi_valuetype handler_type = napi_undefined;
    napi_typeof (env, argv[1], &handler_type);
    if (handler_type != napi_function) {
        napi_throw_type_error (env, NULL, "monitorHandler handler must be a function");
        return NULL;
    }

    if (!attach_socket_monitor_handler (env, mon, argv[1]))
        return NULL;

    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value monitor_recv (napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *mon = NULL;
    napi_get_value_external (env, argv[0], &mon);
    (void) argv;
    zlink_monitor_event_t evt;
    int rc = zlink_socket_monitor_recv_v2 (mon, &evt, ZLINK_RECV_FLAGS_NONE);
    if (rc != 0)
        return throw_last_error (env, "monitor_recv failed");
    return create_socket_monitor_event_value (env, evt);
}

napi_value monitor_try_recv (napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *mon = NULL;
    napi_get_value_external (env, argv[0], &mon);

    zlink_monitor_event_t evt;
    int rc = zlink_socket_monitor_recv_v2 (mon, &evt, ZLINK_RECV_FLAGS_DONTWAIT);
    if (rc != 0) {
        if (zlink_errno () == EAGAIN) {
            napi_value none;
            napi_get_null (env, &none);
            return none;
        }
        return throw_last_error (env, "monitor_try_recv failed");
    }

    return create_socket_monitor_event_value (env, evt);
}

napi_value monitor_status (napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *monitor = NULL;
    napi_get_value_external (env, argv[0], &monitor);

    zlink_monitor_status_t snapshot;
    memset (&snapshot, 0, sizeof (snapshot));
    int rc = zlink_monitor_status (monitor, &snapshot);
    if (rc != 0)
        return throw_last_error (env, "monitor_status failed");
    if (snapshot.abi_version != ZLINK_MONITOR_STATUS_ABI_VERSION
        || snapshot.struct_size != sizeof (zlink_monitor_status_t)) {
        napi_throw_error (env, NULL, "monitor_status returned an incompatible ABI snapshot");
        return NULL;
    }

    return create_monitor_status_value (env, snapshot);
}

napi_value monitor_close (napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *monitor = NULL;
    napi_get_value_external (env, argv[0], &monitor);
    release_socket_monitor_handler_slot (monitor);
    void *tmp = monitor;
    int rc = zlink_monitor_close (&tmp);
    if (rc != 0)
        return throw_last_error (env, "monitor_close failed");
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

static napi_value create_external_or_null (napi_env env, void *ptr)
{
    napi_value out;
    if (!ptr) {
        napi_get_null (env, &out);
        return out;
    }
    napi_create_external (env, ptr, NULL, NULL, &out);
    return out;
}

static napi_value create_userdata_value (napi_env env, void *ptr)
{
    napi_value out;
    if (!ptr) {
        napi_get_null (env, &out);
        return out;
    }
    napi_create_bigint_uint64 (env, static_cast<uint64_t> (reinterpret_cast<uintptr_t> (ptr)),
                               &out);
    return out;
}

static void *get_external_or_null (napi_env env, napi_value value)
{
    if (!value)
        return NULL;
    napi_valuetype type;
    if (napi_typeof (env, value, &type) != napi_ok)
        return NULL;
    if (type == napi_null || type == napi_undefined)
        return NULL;
    if (type == napi_bigint) {
        uint64_t raw = 0;
        bool lossless = false;
        if (napi_get_value_bigint_uint64 (env, value, &raw, &lossless) == napi_ok && lossless)
            return reinterpret_cast<void *> (static_cast<uintptr_t> (raw));
        return NULL;
    }
    if (type == napi_number) {
        uint32_t raw = 0;
        if (napi_get_value_uint32 (env, value, &raw) == napi_ok)
            return reinterpret_cast<void *> (static_cast<uintptr_t> (raw));
        return NULL;
    }
    if (type != napi_external)
        return NULL;
    void *ptr = NULL;
    if (napi_get_value_external (env, value, &ptr) != napi_ok)
        return NULL;
    return ptr;
}

static napi_value create_poller_event_value (napi_env env, const zlink_poller_event_t &event)
{
    napi_value obj;
    napi_create_object (env, &obj);

    napi_value value;
    napi_create_uint32 (env, static_cast<uint32_t> (event.source_kind), &value);
    napi_set_named_property (env, obj, "sourceKind", value);
    napi_set_named_property (env, obj, "socket", create_external_or_null (env, event.socket));
    napi_create_int64 (env, static_cast<int64_t> (event.fd), &value);
    napi_set_named_property (env, obj, "fd", value);
    napi_set_named_property (env, obj, "timer", create_external_or_null (env, event.timer));
    napi_set_named_property (env, obj, "userData", create_userdata_value (env, event.user_data));
    napi_create_int32 (env, static_cast<int32_t> (event.events), &value);
    napi_set_named_property (env, obj, "events", value);
    return obj;
}

static napi_value
create_poller_event_result (napi_env env, const zlink_poller_event_t &event, int rc)
{
    if (rc <= 0) {
        napi_value none;
        napi_get_null (env, &none);
        return none;
    }
    return create_poller_event_value (env, event);
}

struct timer_handler_js_state_t
{
    timer_handler_js_state_t () : used (false), timer (NULL), env (NULL), tsfn (NULL) {}

    bool used;
    void *timer;
    napi_env env;
    napi_threadsafe_function tsfn;
};

static const size_t k_timer_handler_slot_count = 8;
static std::mutex g_timer_handler_slots_mu;
static timer_handler_js_state_t g_timer_handler_slots[k_timer_handler_slot_count];

static timer_handler_js_state_t *find_timer_handler_slot_by_timer_unsafe (void *timer)
{
    return find_tsfn_slot_by_subject (g_timer_handler_slots, k_timer_handler_slot_count,
                                      &timer_handler_js_state_t::timer, timer);
}

static void reset_timer_handler_slot_unsafe (timer_handler_js_state_t *state)
{
    if (!state)
        return;
    reset_tsfn_slot_base (state);
    state->timer = NULL;
}

static void timer_handler_tsfn_finalize (napi_env env, void *finalize_data, void *finalize_hint)
{
    (void) env;
    (void) finalize_hint;
    timer_handler_js_state_t *state = static_cast<timer_handler_js_state_t *> (finalize_data);
    if (!state)
        return;
    std::lock_guard<std::mutex> lock (g_timer_handler_slots_mu);
    reset_timer_handler_slot_unsafe (state);
}

static void timer_handler_tsfn_call_js (napi_env env, napi_value js_cb, void *context, void *data)
{
    (void) context;
    std::unique_ptr<uint64_t> fire_count (static_cast<uint64_t *> (data));
    if (!env || !js_cb || !fire_count)
        return;

    napi_value argv[1];
    napi_create_bigint_uint64 (env, *fire_count, &argv[0]);
    napi_value recv;
    napi_value this_arg;
    napi_get_undefined (env, &this_arg);
    (void) napi_call_function (env, this_arg, js_cb, 1, argv, &recv);
}

static void release_timer_handler_slot (void *timer)
{
    napi_threadsafe_function tsfn = NULL;
    {
        std::lock_guard<std::mutex> lock (g_timer_handler_slots_mu);
        timer_handler_js_state_t *state = find_timer_handler_slot_by_timer_unsafe (timer);
        if (!state)
            return;
        tsfn = state->tsfn;
        reset_timer_handler_slot_unsafe (state);
    }
    if (tsfn)
        (void) napi_release_threadsafe_function (tsfn, napi_tsfn_abort);
}

static void timer_handler_dispatch (void *timer_, uint64_t fire_count_, void *closure)
{
    (void) timer_;
    timer_handler_js_state_t *state = static_cast<timer_handler_js_state_t *> (closure);
    if (!state)
        return;

    napi_threadsafe_function tsfn = NULL;
    {
        std::lock_guard<std::mutex> lock (g_timer_handler_slots_mu);
        if (!state->used || !state->tsfn)
            return;
        tsfn = state->tsfn;
    }

    std::unique_ptr<uint64_t> payload (new uint64_t (fire_count_));
    if (napi_call_threadsafe_function (tsfn, payload.get (), napi_tsfn_nonblocking) != napi_ok) {
        return;
    }
    (void) payload.release ();
}

static napi_value create_stopwatch_value (napi_env env, unsigned long value)
{
    napi_value out;
    napi_create_double (env, static_cast<double> (value), &out);
    return out;
}

napi_value poller_new (napi_env env, napi_callback_info info)
{
    (void) info;
    void *poller = zlink_poller_new ();
    if (!poller)
        return throw_last_error (env, "poller_new failed");
    napi_value out;
    napi_create_external (env, poller, NULL, NULL, &out);
    return out;
}

napi_value poller_destroy (napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *poller = NULL;
    napi_get_value_external (env, argv[0], &poller);
    void *tmp = poller;
    int rc = zlink_poller_destroy (&tmp);
    if (rc != 0)
        return throw_last_error (env, "poller_destroy failed");
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value poller_size (napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *poller = NULL;
    napi_get_value_external (env, argv[0], &poller);
    zlink_config_result_t err = ZLINK_CONFIG_OK;
    int size = zlink_poller_size (poller, &err);
    if (err != ZLINK_CONFIG_OK)
        return throw_last_error (env, "poller_size failed");
    napi_value out;
    napi_create_int32 (env, size, &out);
    return out;
}

napi_value poller_add (napi_env env, napi_callback_info info)
{
    napi_value argv[4];
    size_t argc = 4;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *poller = NULL;
    void *socket = NULL;
    napi_get_value_external (env, argv[0], &poller);
    napi_get_value_external (env, argv[1], &socket);
    int32_t events = 0;
    napi_get_value_int32 (env, argv[3], &events);
    void *user_data = argc >= 3 ? get_external_or_null (env, argv[2]) : NULL;
    zlink_config_result_t rc = zlink_poller_add (poller, socket, user_data, (short) events);
    if (rc != ZLINK_CONFIG_OK)
        return throw_last_error (env, "poller_add failed");
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value poller_modify (napi_env env, napi_callback_info info)
{
    napi_value argv[3];
    size_t argc = 3;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *poller = NULL;
    void *socket = NULL;
    napi_get_value_external (env, argv[0], &poller);
    napi_get_value_external (env, argv[1], &socket);
    int32_t events = 0;
    napi_get_value_int32 (env, argv[2], &events);
    zlink_config_result_t rc = zlink_poller_modify (poller, socket, (short) events);
    if (rc != ZLINK_CONFIG_OK)
        return throw_last_error (env, "poller_modify failed");
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value poller_remove (napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *poller = NULL;
    void *socket = NULL;
    napi_get_value_external (env, argv[0], &poller);
    napi_get_value_external (env, argv[1], &socket);
    zlink_config_result_t rc = zlink_poller_remove (poller, socket);
    if (rc != ZLINK_CONFIG_OK)
        return throw_last_error (env, "poller_remove failed");
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value poller_add_fd (napi_env env, napi_callback_info info)
{
    napi_value argv[4];
    size_t argc = 4;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *poller = NULL;
    napi_get_value_external (env, argv[0], &poller);
    int32_t fd = 0, events = 0;
    napi_get_value_int32 (env, argv[1], &fd);
    napi_get_value_int32 (env, argv[3], &events);
    void *user_data = argc >= 3 ? get_external_or_null (env, argv[2]) : NULL;
    zlink_config_result_t rc = zlink_poller_add_fd (poller, fd, user_data, (short) events);
    if (rc != ZLINK_CONFIG_OK)
        return throw_last_error (env, "poller_add_fd failed");
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value poller_modify_fd (napi_env env, napi_callback_info info)
{
    napi_value argv[3];
    size_t argc = 3;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *poller = NULL;
    napi_get_value_external (env, argv[0], &poller);
    int32_t fd = 0, events = 0;
    napi_get_value_int32 (env, argv[1], &fd);
    napi_get_value_int32 (env, argv[2], &events);
    zlink_config_result_t rc = zlink_poller_modify_fd (poller, fd, (short) events);
    if (rc != ZLINK_CONFIG_OK)
        return throw_last_error (env, "poller_modify_fd failed");
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value poller_remove_fd (napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *poller = NULL;
    napi_get_value_external (env, argv[0], &poller);
    int32_t fd = 0;
    napi_get_value_int32 (env, argv[1], &fd);
    zlink_config_result_t rc = zlink_poller_remove_fd (poller, fd);
    if (rc != ZLINK_CONFIG_OK)
        return throw_last_error (env, "poller_remove_fd failed");
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value poller_add_timer (napi_env env, napi_callback_info info)
{
    napi_value argv[3];
    size_t argc = 3;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *poller = NULL;
    void *timer = NULL;
    napi_get_value_external (env, argv[0], &poller);
    napi_get_value_external (env, argv[1], &timer);
    void *user_data = argc >= 3 ? get_external_or_null (env, argv[2]) : NULL;
    zlink_config_result_t rc = zlink_poller_add_timer (poller, timer, user_data);
    if (rc != ZLINK_CONFIG_OK)
        return throw_last_error (env, "poller_add_timer failed");
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value poller_remove_timer (napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *poller = NULL;
    void *timer = NULL;
    napi_get_value_external (env, argv[0], &poller);
    napi_get_value_external (env, argv[1], &timer);
    zlink_config_result_t rc = zlink_poller_remove_timer (poller, timer);
    if (rc != ZLINK_CONFIG_OK)
        return throw_last_error (env, "poller_remove_timer failed");
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value poller_wait (napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *poller = NULL;
    napi_get_value_external (env, argv[0], &poller);
    int32_t timeout = 0;
    napi_get_value_int32 (env, argv[1], &timeout);
    zlink_poller_event_t event;
    memset (&event, 0, sizeof (event));
    zlink_config_result_t err = ZLINK_CONFIG_OK;
    int rc = zlink_poller_wait (poller, &event, 1, timeout, &err);
    if (err != ZLINK_CONFIG_OK)
        return throw_last_error (env, "poller_wait failed");
    return create_poller_event_result (env, event, rc);
}

napi_value poll_events_new (napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    int32_t capacity = 0;
    napi_get_value_int32 (env, argv[0], &capacity);
    if (capacity <= 0) {
        napi_throw_range_error (env, NULL, "capacity must be a positive integer");
        return NULL;
    }
    zlink_poller_event_t *events = new zlink_poller_event_t[static_cast<size_t> (capacity)]();
    napi_value out;
    napi_create_external (env, events, NULL, NULL, &out);
    return out;
}

napi_value poll_events_destroy (napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *ptr = NULL;
    napi_get_value_external (env, argv[0], &ptr);
    delete[] static_cast<zlink_poller_event_t *> (ptr);
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

static bool poll_events_args (napi_env env,
                              napi_callback_info info,
                              zlink_poller_event_t **events,
                              int32_t *index)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *ptr = NULL;
    napi_get_value_external (env, argv[0], &ptr);
    napi_get_value_int32 (env, argv[1], index);
    if (!ptr || *index < 0) {
        napi_throw_range_error (env, NULL, "invalid poll event index");
        return false;
    }
    *events = static_cast<zlink_poller_event_t *> (ptr);
    return true;
}

napi_value poll_events_source_kind (napi_env env, napi_callback_info info)
{
    zlink_poller_event_t *events = NULL;
    int32_t index = 0;
    if (!poll_events_args (env, info, &events, &index))
        return NULL;
    napi_value out;
    napi_create_int32 (env, static_cast<int32_t> (events[index].source_kind), &out);
    return out;
}

napi_value poll_events_slot (napi_env env, napi_callback_info info)
{
    zlink_poller_event_t *events = NULL;
    int32_t index = 0;
    if (!poll_events_args (env, info, &events, &index))
        return NULL;
    napi_value out;
    napi_create_double (
      env, static_cast<double> (reinterpret_cast<uintptr_t> (events[index].user_data)), &out);
    return out;
}

napi_value poll_events_revents (napi_env env, napi_callback_info info)
{
    zlink_poller_event_t *events = NULL;
    int32_t index = 0;
    if (!poll_events_args (env, info, &events, &index))
        return NULL;
    napi_value out;
    napi_create_int32 (env, static_cast<int32_t> (events[index].events), &out);
    return out;
}

napi_value poll_events_fd (napi_env env, napi_callback_info info)
{
    zlink_poller_event_t *events = NULL;
    int32_t index = 0;
    if (!poll_events_args (env, info, &events, &index))
        return NULL;
    napi_value out;
    napi_create_int64 (env, static_cast<int64_t> (events[index].fd), &out);
    return out;
}

napi_value poller_wait_into (napi_env env, napi_callback_info info)
{
    napi_value argv[4];
    size_t argc = 4;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *poller = NULL;
    void *events_ptr = NULL;
    int32_t capacity = 0;
    int32_t timeout = 0;
    napi_get_value_external (env, argv[0], &poller);
    napi_get_value_external (env, argv[1], &events_ptr);
    napi_get_value_int32 (env, argv[2], &capacity);
    napi_get_value_int32 (env, argv[3], &timeout);
    if (!events_ptr || capacity <= 0) {
        napi_throw_range_error (env, NULL, "events capacity must be positive");
        return NULL;
    }
    zlink_config_result_t err = ZLINK_CONFIG_OK;
    int rc = zlink_poller_wait (poller, static_cast<zlink_poller_event_t *> (events_ptr), capacity,
                                timeout, &err);
    if (err != ZLINK_CONFIG_OK)
        return throw_last_error (env, "poller_wait_into failed");
    napi_value out;
    napi_create_int32 (env, rc, &out);
    return out;
}

napi_value timer_new (napi_env env, napi_callback_info info)
{
    (void) info;
    void *timer = zlink_timer_new ();
    if (!timer)
        return throw_last_error (env, "timer_new failed");
    napi_value out;
    napi_create_external (env, timer, NULL, NULL, &out);
    return out;
}

napi_value timer_destroy (napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *timer = NULL;
    napi_get_value_external (env, argv[0], &timer);
    release_timer_handler_slot (timer);
    void *tmp = timer;
    int rc = zlink_timer_destroy (&tmp);
    if (rc != 0)
        return throw_last_error (env, "timer_destroy failed");
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value timer_start (napi_env env, napi_callback_info info)
{
    napi_value argv[3];
    size_t argc = 3;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *timer = NULL;
    napi_get_value_external (env, argv[0], &timer);
    bool lossless = false;
    uint64_t interval = 0;
    uint64_t repeat = 0;
    napi_get_value_bigint_uint64 (env, argv[1], &interval, &lossless);
    napi_get_value_bigint_uint64 (env, argv[2], &repeat, &lossless);
    zlink_config_result_t rc = zlink_timer_start (timer, interval, repeat);
    if (rc != ZLINK_CONFIG_OK)
        return throw_last_error (env, "timer_start failed");
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value timer_stop (napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *timer = NULL;
    napi_get_value_external (env, argv[0], &timer);
    zlink_config_result_t rc = zlink_timer_stop (timer);
    if (rc != ZLINK_CONFIG_OK)
        return throw_last_error (env, "timer_stop failed");
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value timer_recv (napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *timer = NULL;
    napi_get_value_external (env, argv[0], &timer);
    int32_t flags = 0;
    if (argc >= 2)
        napi_get_value_int32 (env, argv[1], &flags);
    uint64_t fire_count = 0;
    zlink_recv_result_t rc = zlink_timer_recv (timer, &fire_count);
    if (rc != ZLINK_RECV_OK)
        return throw_last_error (env, "timer_recv failed");
    napi_value out;
    napi_create_bigint_uint64 (env, fire_count, &out);
    return out;
}

napi_value timer_handler (napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *timer = NULL;
    napi_get_value_external (env, argv[0], &timer);
    napi_value handler = argv[1];

    timer_handler_js_state_t *slot = reserve_tsfn_subject_slot (
      env, g_timer_handler_slots_mu, g_timer_handler_slots, k_timer_handler_slot_count,
      &timer_handler_js_state_t::timer, timer, "timer handler already attached",
      "no free timer handler slot", NULL);
    if (!slot)
        return NULL;

    napi_threadsafe_function tsfn = NULL;
    if (!create_tsfn_slot_queue (env, handler, slot, "zlink-timer-handler",
                                 timer_handler_tsfn_finalize, timer_handler_tsfn_call_js,
                                 "timer handler failed to create callback queue", false, &tsfn))
        return NULL;

    {
        std::lock_guard<std::mutex> lock (g_timer_handler_slots_mu);
        bind_tsfn_subject_slot_unsafe (slot, &timer_handler_js_state_t::timer, timer, env, tsfn);
    }

    zlink_handler_result_t rc = zlink_timer_handler (timer, &timer_handler_dispatch, slot);
    if (rc != ZLINK_HANDLER_OK) {
        release_timer_handler_slot (timer);
        return throw_last_error (env, "timer_handler failed");
    }
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value stopwatch_start (napi_env env, napi_callback_info info)
{
    (void) info;
    void *watch = zlink_stopwatch_start ();
    if (!watch)
        return throw_last_error (env, "stopwatch_start failed");
    napi_value out;
    napi_create_external (env, watch, NULL, NULL, &out);
    return out;
}

napi_value stopwatch_intermediate (napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *watch = NULL;
    napi_get_value_external (env, argv[0], &watch);
    unsigned long value = zlink_stopwatch_intermediate (watch);
    return create_stopwatch_value (env, value);
}

napi_value stopwatch_stop (napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *watch = NULL;
    napi_get_value_external (env, argv[0], &watch);
    unsigned long value = zlink_stopwatch_stop (watch);
    return create_stopwatch_value (env, value);
}

napi_value atomic_counter_new (napi_env env, napi_callback_info info)
{
    (void) info;
    void *counter = zlink_atomic_counter_new ();
    if (!counter)
        return throw_last_error (env, "atomic_counter_new failed");
    napi_value out;
    napi_create_external (env, counter, NULL, NULL, &out);
    return out;
}

napi_value atomic_counter_set (napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *counter = NULL;
    int32_t value = 0;
    napi_get_value_external (env, argv[0], &counter);
    napi_get_value_int32 (env, argv[1], &value);
    zlink_atomic_counter_set (counter, value);
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value atomic_counter_inc (napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *counter = NULL;
    napi_get_value_external (env, argv[0], &counter);
    napi_value out;
    napi_create_int32 (env, zlink_atomic_counter_inc (counter), &out);
    return out;
}

napi_value atomic_counter_dec (napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *counter = NULL;
    napi_get_value_external (env, argv[0], &counter);
    napi_value out;
    napi_create_int32 (env, zlink_atomic_counter_dec (counter), &out);
    return out;
}

napi_value atomic_counter_value (napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *counter = NULL;
    napi_get_value_external (env, argv[0], &counter);
    napi_value out;
    napi_create_int32 (env, zlink_atomic_counter_value (counter), &out);
    return out;
}

napi_value atomic_counter_destroy (napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *counter = NULL;
    napi_get_value_external (env, argv[0], &counter);
    zlink_atomic_counter_destroy (&counter);
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}
