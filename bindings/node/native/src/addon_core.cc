/* SPDX-License-Identifier: MPL-2.0 */

#include "addon_core_api.h"
#include "addon_core_options.h"
#include "addon_monitor_status_values.h"
#include "addon_message_values.h"
#include "addon_message_parts.h"
#include "addon_request_callbacks.h"
#include "addon_submit_results.h"
#include "addon_tsfn_slots.h"
#include <errno.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace
{

static const size_t k_stream_slot_count = 8;
static const size_t k_socket_monitor_handler_slot_count = 8;
static const int32_t k_stream_dispatch_len32be = 1;

struct stream_js_payload_t
{
    stream_js_payload_t () : packet_count (0), body_materialization (0) {
        memset (&routing_id, 0, sizeof (routing_id));
    }
    ~stream_js_payload_t ()
    {
        if (packet_count > 0)
            close_recv_parts (packets, packet_count);
    }

    zlink_routing_id_t routing_id;
    zlink_msg_t packets[2];
    size_t packet_count;
    int body_materialization;
};

struct socket_monitor_handler_js_payload_t
{
    zlink_monitor_event_t event;
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
    std::mutex mutex;
    std::atomic<int> stop_requested;
    int body_materialization;
};

static std::mutex g_stream_slots_mu;
static stream_js_state_t g_stream_slots[k_stream_slot_count];

struct socket_monitor_handler_js_state_t
{
    socket_monitor_handler_js_state_t () : used (false), monitor (NULL), env (NULL), tsfn (NULL) {}

    bool used;
    void *monitor;
    napi_env env;
    napi_threadsafe_function tsfn;
    std::mutex mutex;
};

static std::mutex g_socket_monitor_handler_slots_mu;
static socket_monitor_handler_js_state_t
  g_socket_monitor_handler_slots[k_socket_monitor_handler_slot_count];

struct send_completion_state_t
{
    send_completion_state_t ()
        : socket (NULL), env (NULL), tsfn (NULL), js_thread_outstanding (0)
    {
    }

    void *socket;
    napi_env env;
    napi_threadsafe_function tsfn;
    //  Operations whose completion has to arrive through the threadsafe
    //  function. The handler's tsfn is created unreferenced so an idle socket
    //  never keeps a thread alive, but while this count is non-zero the tsfn
    //  must hold a reference: the awaiting JavaScript has no other handle, and
    //  an event loop that drains empty starts tearing the environment down,
    //  after which the queued completion can no longer call into JavaScript.
    //  Only the JavaScript thread touches this counter.
    uint64_t js_thread_outstanding;
};

struct send_async_operation_t
{
    send_async_operation_t (send_completion_state_t *state_, uint64_t token_)
        : state (state_), token (token_), submit_returned (false), completed (false)
    {
        memset (&event, 0, sizeof (event));
    }

    send_completion_state_t *state;
    uint64_t token;
    bool submit_returned;
    bool completed;
    zlink_send_complete_event_t event;
    std::mutex mutex;
};

struct send_completion_js_payload_t
{
    send_completion_js_payload_t () : token (0)
    {
        memset (&event, 0, sizeof (event));
    }

    uint64_t token;
    zlink_send_complete_event_t event;
};

struct held_routed_multipart_test_t
{
    held_routed_multipart_test_t ()
        : socket (NULL), opened (false), finish (false), open_result (-1),
          open_errno (0), final_result (-1), final_errno (0)
    {
        memset (&routing_id, 0, sizeof (routing_id));
    }

    void *socket;
    zlink_routing_id_t routing_id;
    std::mutex mutex;
    std::condition_variable condition;
    bool opened;
    bool finish;
    int open_result;
    int open_errno;
    int final_result;
    int final_errno;
    std::thread worker;
};

struct send_close_stress_counts_t
{
    send_close_stress_counts_t ()
        : attempts (0), single_attempts (0), multipart_attempts (0),
          submitted (0), rejected_einval (0), shutdown (0),
          backpressured (0), other_submit (0), received_records (0),
          bad_records (0), bad_first_parts (0), bad_mixed_parts (0),
          bad_part_counts (0), bad_next_part_results (0),
          close_ok (0), close_busy (0),
          close_shutdown (0), close_other (0)
    {
    }

    std::atomic<uint64_t> attempts;
    std::atomic<uint64_t> single_attempts;
    std::atomic<uint64_t> multipart_attempts;
    std::atomic<uint64_t> submitted;
    std::atomic<uint64_t> rejected_einval;
    std::atomic<uint64_t> shutdown;
    std::atomic<uint64_t> backpressured;
    std::atomic<uint64_t> other_submit;
    std::atomic<uint64_t> received_records;
    std::atomic<uint64_t> bad_records;
    std::atomic<uint64_t> bad_first_parts;
    std::atomic<uint64_t> bad_mixed_parts;
    std::atomic<uint64_t> bad_part_counts;
    std::atomic<uint64_t> bad_next_part_results;
    std::atomic<uint64_t> close_ok;
    std::atomic<uint64_t> close_busy;
    std::atomic<uint64_t> close_shutdown;
    std::atomic<uint64_t> close_other;
};

struct stress_part_payload_t
{
    uint32_t magic;
    uint32_t sender;
    uint32_t sequence;
    uint32_t part_index;
    uint32_t part_count;
    uint32_t checksum;
};

static const uint32_t k_stress_payload_magic = 0x5a4c4e4b;

static thread_local std::unordered_map<void *, send_completion_state_t *>
  g_send_completion_states;

send_completion_state_t *find_send_completion_state (void *socket)
{
    std::unordered_map<void *, send_completion_state_t *>::iterator entry =
      g_send_completion_states.find (socket);
    return entry == g_send_completion_states.end () ? NULL : entry->second;
}

stream_js_state_t *find_stream_slot_by_socket_unsafe (void *socket)
{
    return find_tsfn_slot_by_subject (g_stream_slots, k_stream_slot_count,
                                      &stream_js_state_t::socket, socket);
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

void run_held_routed_multipart_test (held_routed_multipart_test_t *state)
{
    static const char first_payload[] = "held-first";
    static const char final_payload[] = "held-final";
    zlink_msg_t first;
    zlink_msg_t final;
    const bool first_initialized = init_msg_from_bytes (
      &first, first_payload, sizeof (first_payload) - 1u);
    const bool final_initialized = init_msg_from_bytes (
      &final, final_payload, sizeof (final_payload) - 1u);

    if (first_initialized && final_initialized) {
        state->open_result = zlink_send_part_rid (
          state->socket, &state->routing_id, &first,
          ZLINK_SEND_FLAGS_NONE, ZLINK_PART_MORE);
        state->open_errno = state->open_result == ZLINK_SUBMIT_OK ? 0 : zlink_errno ();
    } else {
        state->open_result = ZLINK_SUBMIT_OUT_OF_MEMORY;
        state->open_errno = zlink_errno ();
    }

    {
        std::lock_guard<std::mutex> lock (state->mutex);
        state->opened = true;
    }
    state->condition.notify_one ();

    {
        std::unique_lock<std::mutex> lock (state->mutex);
        state->condition.wait_for (
          lock, std::chrono::seconds (5), [state] { return state->finish; });
    }

    if (state->open_result == ZLINK_SUBMIT_OK) {
        state->final_result = zlink_send_part_rid (
          state->socket, &state->routing_id, &final,
          ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL);
        state->final_errno = state->final_result == ZLINK_SUBMIT_OK ? 0 : zlink_errno ();
    }

    if (state->open_result != ZLINK_SUBMIT_OK
        || state->final_result != ZLINK_SUBMIT_OK) {
        if (first_initialized)
            zlink_msg_close (&first);
        if (final_initialized)
            zlink_msg_close (&final);
    }
}

uint32_t stress_payload_checksum (const stress_part_payload_t &payload)
{
    return payload.magic ^ payload.sender ^ payload.sequence
           ^ payload.part_index ^ payload.part_count;
}

bool init_stress_part (zlink_msg_t *part,
                       uint32_t sender,
                       uint32_t sequence,
                       uint32_t part_index,
                       uint32_t part_count)
{
    stress_part_payload_t payload;
    payload.magic = k_stress_payload_magic;
    payload.sender = sender;
    payload.sequence = sequence;
    payload.part_index = part_index;
    payload.part_count = part_count;
    payload.checksum = stress_payload_checksum (payload);
    return init_msg_from_bytes (part, &payload, sizeof (payload));
}

bool read_stress_part (zlink_msg_t *part, stress_part_payload_t *payload)
{
    if (!part || !payload || zlink_msg_size (part) != sizeof (*payload))
        return false;
    memcpy (payload, zlink_msg_data (part), sizeof (*payload));
    return payload->magic == k_stress_payload_magic
           && payload->checksum == stress_payload_checksum (*payload);
}

void run_send_close_stress_sender (void *sender,
                                   uint32_t sender_index,
                                   uint32_t iterations,
                                   std::atomic<uint32_t> *ready,
                                   std::atomic<bool> *start,
                                   send_close_stress_counts_t *counts)
{
    ready->fetch_add (1, std::memory_order_release);
    while (!start->load (std::memory_order_acquire))
        std::this_thread::yield ();

    for (uint32_t sequence = 0; sequence < iterations; ++sequence) {
        const uint32_t part_count = (sequence & 1u) == 0 ? 1u : 3u;
        zlink_msg_t parts[3];
        uint32_t initialized = 0;
        for (; initialized < part_count; ++initialized) {
            if (!init_stress_part (&parts[initialized], sender_index, sequence,
                                   initialized, part_count))
                break;
        }
        if (initialized != part_count) {
            zlink_multipart_close (parts, initialized);
            counts->other_submit.fetch_add (1, std::memory_order_relaxed);
            counts->attempts.fetch_add (1, std::memory_order_relaxed);
            continue;
        }

        if (part_count == 1)
            counts->single_attempts.fetch_add (1, std::memory_order_relaxed);
        else
            counts->multipart_attempts.fetch_add (1, std::memory_order_relaxed);
        counts->attempts.fetch_add (1, std::memory_order_release);

        const int result = send_parts (
          sender, parts, part_count, ZLINK_SEND_FLAGS_DONTWAIT);
        const int native_errno = result == ZLINK_SUBMIT_OK ? 0 : zlink_errno ();
        if (result == ZLINK_SUBMIT_OK) {
            counts->submitted.fetch_add (1, std::memory_order_relaxed);
        } else if ((result == ZLINK_SUBMIT_INVALID_ARGUMENT
                    || result == ZLINK_SUBMIT_INVALID_STATE)
                   && native_errno == EINVAL) {
            counts->rejected_einval.fetch_add (1, std::memory_order_relaxed);
        } else if (native_errno == ESHUTDOWN) {
            counts->shutdown.fetch_add (1, std::memory_order_relaxed);
        } else if (result == ZLINK_SUBMIT_BACKPRESSURED
                   || native_errno == EAGAIN) {
            counts->backpressured.fetch_add (1, std::memory_order_relaxed);
        } else {
            counts->other_submit.fetch_add (1, std::memory_order_relaxed);
        }
    }
}

void run_send_close_stress_receiver (void *receiver,
                                     std::atomic<bool> *senders_done,
                                     send_close_stress_counts_t *counts)
{
    uint32_t empty_after_done = 0;
    while (!senders_done->load (std::memory_order_acquire)
           || empty_after_done < 10000u) {
        zlink_msg_t part;
        if (zlink_msg_init (&part) != ZLINK_CONFIG_OK) {
            counts->bad_records.fetch_add (1, std::memory_order_relaxed);
            return;
        }
        const zlink_routing_id_t *source_routing_id = NULL;
        zlink_part_flag_t has_more = ZLINK_PART_FINAL;
        const int result = zlink_recv_part (
          receiver, &source_routing_id, &part, &has_more,
          ZLINK_RECV_FLAGS_DONTWAIT);
        const int native_errno = result == ZLINK_RECV_OK ? 0 : zlink_errno ();
        if (result == ZLINK_RECV_NO_DATA || native_errno == EAGAIN) {
            zlink_msg_close (&part);
            if (senders_done->load (std::memory_order_acquire))
                ++empty_after_done;
            std::this_thread::yield ();
            continue;
        }
        empty_after_done = 0;
        if (result != ZLINK_RECV_OK) {
            zlink_msg_close (&part);
            counts->bad_records.fetch_add (1, std::memory_order_relaxed);
            continue;
        }

        stress_part_payload_t first_payload = {};
        bool valid = read_stress_part (&part, &first_payload)
                     && first_payload.part_index == 0;
        if (!valid)
            counts->bad_first_parts.fetch_add (1, std::memory_order_relaxed);
        zlink_msg_close (&part);
        uint32_t received_part_count = 1;
        while (has_more) {
            if (zlink_msg_init (&part) != ZLINK_CONFIG_OK) {
                valid = false;
                break;
            }
            const int part_result = zlink_recv_part (
              receiver, &source_routing_id, &part, &has_more,
              ZLINK_RECV_FLAGS_DONTWAIT);
            stress_part_payload_t payload;
            if (part_result != ZLINK_RECV_OK) {
                counts->bad_next_part_results.fetch_add (1, std::memory_order_relaxed);
                valid = false;
            } else if (!read_stress_part (&part, &payload)
                       || payload.sender != first_payload.sender
                       || payload.sequence != first_payload.sequence
                       || payload.part_count != first_payload.part_count
                       || payload.part_index != received_part_count) {
                counts->bad_mixed_parts.fetch_add (1, std::memory_order_relaxed);
                valid = false;
            }
            zlink_msg_close (&part);
            ++received_part_count;
            if (part_result != ZLINK_RECV_OK)
                break;
        }
        if (received_part_count != first_payload.part_count) {
            counts->bad_part_counts.fetch_add (1, std::memory_order_relaxed);
            valid = false;
        }
        counts->received_records.fetch_add (1, std::memory_order_relaxed);
        if (!valid)
            counts->bad_records.fetch_add (1, std::memory_order_relaxed);
    }
}

void run_send_close_stress_closer (void *sender,
                                   uint64_t close_after_attempts,
                                   send_close_stress_counts_t *counts)
{
    while (counts->attempts.load (std::memory_order_acquire)
           < close_after_attempts)
        std::this_thread::yield ();

    for (;;) {
        const int result = zlink_close (sender);
        if (result == ZLINK_CLOSE_OK) {
            counts->close_ok.fetch_add (1, std::memory_order_relaxed);
            break;
        }
        if (result == ZLINK_CLOSE_BUSY) {
            counts->close_busy.fetch_add (1, std::memory_order_relaxed);
            std::this_thread::yield ();
            continue;
        }
        if (result == ZLINK_CLOSE_SHUTDOWN) {
            counts->close_shutdown.fetch_add (1, std::memory_order_relaxed);
            return;
        }
        counts->close_other.fetch_add (1, std::memory_order_relaxed);
        return;
    }

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

int dealer_reply_parts (void *dealer,
                        uint64_t request_seq,
                        zlink_msg_t *parts,
                        size_t part_count)
{
    return submit_msg_parts (parts, part_count, [dealer, request_seq] (
                                                  zlink_msg_t *part,
                                                  zlink_part_flag_t part_flag, bool) {
        return zlink_dealer_reply_part (dealer, request_seq, part, part_flag);
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
                                             size_t part_count,
                                             bool prefer_managed_single_part,
                                             napi_value routing_id_storage)
{
    napi_value obj;
    if (part_count == 1 && !prefer_managed_single_part) {
        // Router relay is a common application path. Keep its sole received
        // frame in msg_t storage until the caller either reads data() or sends
        // it again. A successful send can then transfer the same ownership to
        // Core without a JS Buffer copy between the two native calls.
        napi_create_object (env, &obj);
        napi_value native_message = move_message_to_native_frame_value (env, &parts[0]);
        if (!native_message)
            return NULL;
        napi_set_named_property (env, obj, "nativeMessage", native_message);
        napi_value rid = create_routing_id_value_reusing (
          env, routing_id, routing_id_storage);
        napi_set_named_property (env, obj, "routingId", rid);
    } else {
        // HOT PATH: a stable terminal reader asked for managed storage. Copy
        // the single part while this recv call is already across the N-API
        // boundary instead of returning a frame handle and crossing again
        // from Message.data(). Multipart keeps its existing representation.
        obj = create_recv_message_value (env, routing_id, parts, part_count);
        if (obj && routing_id.size > 0 && routing_id_storage != NULL) {
            napi_value rid = create_routing_id_value_reusing (
              env, routing_id, routing_id_storage);
            napi_set_named_property (env, obj, "routingId", rid);
        }
    }
    if (!obj)
        return NULL;
    if (request_seq != 0) {
        napi_value request_seq_value;
        napi_create_bigint_uint64 (env, request_seq, &request_seq_value);
        napi_set_named_property (env, obj, "requestSeq", request_seq_value);
    }
    if (transport_pair_id != 0 || transport_pair_generation != 0) {
        napi_value pair_id_value;
        napi_value pair_generation_value;
        napi_create_bigint_uint64 (env, transport_pair_id, &pair_id_value);
        napi_create_bigint_uint64 (env, transport_pair_generation, &pair_generation_value);
        napi_set_named_property (env, obj, "transportPairId", pair_id_value);
        napi_set_named_property (env, obj, "transportPairGeneration", pair_generation_value);
    }
    return obj;
}

int router_recv_message_value (napi_env env,
                               void *router,
                               int32_t flags,
                               bool prefer_managed_single_part,
                               napi_value routing_id_storage,
                               napi_value *out)
{
    const zlink_routing_id_t *peer_rid_ptr = NULL;
    zlink_routing_id_t peer_rid;
    uint64_t request_seq = 0;
    uint64_t transport_pair_id = 0;
    uint64_t transport_pair_generation = 0;
    zlink_msg_t first_part;
    if (zlink_msg_init (&first_part) != 0)
        return ZLINK_RECV_INTERNAL_ERROR;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;

    const int rc = zlink_router_recv_part_v2 (
      router, &peer_rid_ptr, &request_seq, &transport_pair_id,
      &transport_pair_generation, &first_part, &has_more,
      static_cast<zlink_recv_flags_t> (flags));
    if (rc != ZLINK_RECV_OK) {
        zlink_msg_close (&first_part);
        return rc;
    }
    copy_routing_id (&peer_rid, peer_rid_ptr);

    if (!has_more) {
        // HOT PATH: routed perf and ordinary RPCs are usually one-part.
        // Avoid constructing a heap-backed vector before moving the frame to
        // its JS owner; multipart receives retain the generic path below.
        *out = create_router_recv_message_value (
          env, peer_rid, request_seq, transport_pair_id, transport_pair_generation,
          &first_part, 1, prefer_managed_single_part, routing_id_storage);
        zlink_msg_close (&first_part);
        return *out ? ZLINK_RECV_OK : ZLINK_RECV_INTERNAL_ERROR;
    }

    std::vector<zlink_msg_t> parts;
    if (!append_msg_move (&parts, &first_part)) {
        zlink_msg_close (&first_part);
        errno = ENOMEM;
        return ZLINK_RECV_INTERNAL_ERROR;
    }
    while (has_more) {
        const zlink_routing_id_t *next_peer_rid = NULL;
        uint64_t next_request_seq = 0;
        zlink_msg_t next_part;
        if (zlink_msg_init (&next_part) != 0) {
            close_msg_vector (parts);
            return ZLINK_RECV_INTERNAL_ERROR;
        }
        zlink_part_flag_t more = ZLINK_PART_FINAL;
        const int next_rc = zlink_router_recv_part (
          router, &next_peer_rid, &next_request_seq, &next_part, &more,
          ZLINK_RECV_FLAGS_DONTWAIT);
        if (next_rc != ZLINK_RECV_OK) {
            zlink_msg_close (&next_part);
            close_msg_vector (parts);
            return next_rc;
        }
        if (!append_msg_move (&parts, &next_part)) {
            zlink_msg_close (&next_part);
            close_msg_vector (parts);
            errno = ENOMEM;
            return ZLINK_RECV_INTERNAL_ERROR;
        }
        has_more = more;
    }

    *out = create_router_recv_message_value (
      env, peer_rid, request_seq, transport_pair_id, transport_pair_generation,
      parts.data (), parts.size (), prefer_managed_single_part, routing_id_storage);
    close_msg_vector (parts);
    return *out ? ZLINK_RECV_OK : ZLINK_RECV_INTERNAL_ERROR;
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

    napi_create_bigint_uint64 (env, event.value, &value);
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

    if (part_count == 1 && routing_id.size == 0) {
        napi_value data = create_received_message_buffer (env, &parts[0]);
        if (!data)
            return NULL;
        napi_value topic_value;
        napi_create_string_utf8 (env, topic ? topic : "", topic ? topic_len : 0, &topic_value);
        napi_create_array_with_length (env, 2, &obj);
        napi_set_element (env, obj, 0, data);
        napi_set_element (env, obj, 1, topic_value);
        return obj;
    }

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
    std::lock_guard<std::mutex> state_lock (state->mutex);
    reset_stream_slot_unsafe (state);
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
    std::lock_guard<std::mutex> state_lock (state->mutex);
    reset_socket_monitor_handler_slot_unsafe (state);
}

void stream_tsfn_call_js (napi_env env, napi_value js_cb, void *context, void *data)
{
    std::unique_ptr<stream_js_payload_t> payload (static_cast<stream_js_payload_t *> (data));
    stream_js_state_t *state = static_cast<stream_js_state_t *> (context);
    if (!env || !js_cb || !state || !payload)
        return;

    napi_value argv[3];
    if (napi_create_buffer_copy (env, payload->routing_id.size,
                                 payload->routing_id.size == 0 ? NULL : payload->routing_id.data,
                                 NULL, &argv[0])
        != napi_ok)
        return;
    // STREAM packet handlers often relay the header together with the body.
    // Move both frames to Message storage so a handler that only inspects
    // their sizes does not first copy the header into a JS-owned Buffer.
    argv[1] = move_message_to_native_frame_value (env, &payload->packets[0]);
    if (!argv[1])
        return;
    argv[2] = payload->body_materialization == 0
      ? move_message_to_native_frame_value (env, &payload->packets[1])
      : create_received_message_buffer (env, &payload->packets[1]);
    if (!argv[2])
        return;

    napi_value recv;
    napi_value this_arg;
    napi_get_undefined (env, &this_arg);
    napi_status call_status = napi_call_function (env, this_arg, js_cb, 3, argv, &recv);
    if (call_status != napi_ok) {
        state->stop_requested.store (1, std::memory_order_release);
        return;
    }

    int32_t ret = 0;
    if (napi_get_value_int32 (env, recv, &ret) == napi_ok && ret != 0) {
        state->stop_requested.store (1, std::memory_order_release);
    }
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
    std::unique_ptr<stream_js_payload_t> payload (new stream_js_payload_t ());
    copy_routing_id (&payload->routing_id, rid_);
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

    napi_threadsafe_function tsfn = NULL;
    {
        std::lock_guard<std::mutex> lock (state->mutex);
        if (!state->used || !state->tsfn) {
            close_messages ();
            return;
        }
        if (state->stop_requested.load (std::memory_order_acquire) != 0) {
            close_messages ();
            return;
        }
        tsfn = state->tsfn;
        payload->body_materialization = state->body_materialization;
        if (napi_acquire_threadsafe_function (tsfn) != napi_ok) {
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
    (void) napi_release_threadsafe_function (tsfn, napi_tsfn_release);
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
        std::lock_guard<std::mutex> state_lock (state->mutex);
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

#define SOCKET_MONITOR_HANDLER_SLOT_CALLBACK(N) &socket_monitor_handler_slot_callback<N>
typedef void (*socket_monitor_handler_slot_callback_t) (const zlink_monitor_event_t *, void *);
template <size_t Slot>
void socket_monitor_handler_slot_callback (const zlink_monitor_event_t *event_, void *userdata_)
{
    (void) userdata_;
    std::unique_ptr<socket_monitor_handler_js_payload_t> payload (
      new socket_monitor_handler_js_payload_t ());
    if (event_)
        payload->event = *event_;
    else
        memset (&payload->event, 0, sizeof (payload->event));
    socket_monitor_handler_js_state_t *state = &g_socket_monitor_handler_slots[Slot];
    napi_threadsafe_function tsfn = NULL;
    {
        std::lock_guard<std::mutex> lock (state->mutex);
        if (!state->used || !state->tsfn)
            return;
        tsfn = state->tsfn;
        if (napi_acquire_threadsafe_function (tsfn) != napi_ok)
            return;
    }

    if (napi_call_threadsafe_function (tsfn, payload.get (), napi_tsfn_nonblocking) == napi_ok) {
        payload.release ();
    }
    (void) napi_release_threadsafe_function (tsfn, napi_tsfn_release);
}
static socket_monitor_handler_slot_callback_t
  g_socket_monitor_handler_slot_callbacks[k_socket_monitor_handler_slot_count] = {
    SOCKET_MONITOR_HANDLER_SLOT_CALLBACK (0), SOCKET_MONITOR_HANDLER_SLOT_CALLBACK (1),
    SOCKET_MONITOR_HANDLER_SLOT_CALLBACK (2), SOCKET_MONITOR_HANDLER_SLOT_CALLBACK (3),
    SOCKET_MONITOR_HANDLER_SLOT_CALLBACK (4), SOCKET_MONITOR_HANDLER_SLOT_CALLBACK (5),
    SOCKET_MONITOR_HANDLER_SLOT_CALLBACK (6), SOCKET_MONITOR_HANDLER_SLOT_CALLBACK (7),
};
#undef SOCKET_MONITOR_HANDLER_SLOT_CALLBACK

void send_completion_tsfn_finalize (napi_env env,
                                    void *finalize_data,
                                    void *finalize_hint)
{
    (void) env;
    (void) finalize_hint;
    send_completion_state_t *state =
      static_cast<send_completion_state_t *> (finalize_data);
    if (!state)
        return;
    std::unordered_map<void *, send_completion_state_t *>::iterator entry =
      g_send_completion_states.find (state->socket);
    if (entry != g_send_completion_states.end () && entry->second == state)
        g_send_completion_states.erase (entry);
    delete state;
}

void send_completion_tsfn_call_js (napi_env env,
                                   napi_value js_callback,
                                   void *context,
                                   void *data)
{
    std::unique_ptr<send_completion_js_payload_t> payload (
      static_cast<send_completion_js_payload_t *> (data));
    if (!env || !js_callback || !payload)
        return;

    napi_value event;
    if (napi_create_object (env, &event) != napi_ok)
        return;
    set_uint64_bigint_property (env, event, "token", payload->token);
    set_uint64_bigint_property (env, event, "opId", payload->event.op_id);
    set_int64_property (env, event, "result", payload->event.result);
    set_int64_property (env, event, "terminalErrno", payload->event.terminal_errno);

    napi_value peer_rid = create_routing_id_value (env, payload->event.peer_rid);
    napi_set_named_property (env, event, "peerRid", peer_rid);
    set_uint64_bigint_property (
      env, event, "transportPairId", payload->event.transport_pair_id);
    set_uint64_bigint_property (
      env, event, "transportPairGeneration",
      payload->event.transport_pair_generation);

    napi_value this_arg;
    napi_value ignored;
    napi_get_undefined (env, &this_arg);
    (void) napi_call_function (env, this_arg, js_callback, 1, &event, &ignored);

    //  This runs on the JavaScript thread, so the counter needs no lock. The
    //  completion is delivered, so the operation no longer has to hold the
    //  event loop open.
    send_completion_state_t *state =
      static_cast<send_completion_state_t *> (context);
    if (state && state->js_thread_outstanding > 0
        && --state->js_thread_outstanding == 0)
        (void) napi_unref_threadsafe_function (env, state->tsfn);
}

void send_completion_callback (void *subject,
                               const zlink_send_complete_event_t *event,
                               void *userdata)
{
    (void) subject;
    (void) userdata;
    if (!event)
        return;

    send_async_operation_t *operation =
      static_cast<send_async_operation_t *> (event->userdata);
    if (!operation)
        return;

    {
        std::lock_guard<std::mutex> lock (operation->mutex);
        if (!operation->submit_returned) {
            operation->event = *event;
            operation->completed = true;
            return;
        }
    }

    std::unique_ptr<send_completion_js_payload_t> payload (
      new (std::nothrow) send_completion_js_payload_t ());
    if (payload) {
        payload->token = operation->token;
        payload->event = *event;
    }
    send_completion_state_t *state = operation->state;
    napi_threadsafe_function tsfn = state ? state->tsfn : NULL;
    if (payload && tsfn
        && napi_call_threadsafe_function (
             tsfn, payload.get (), napi_tsfn_nonblocking) == napi_ok)
        payload.release ();
    delete operation;
}

bool attach_send_completion_handler (napi_env env, void *socket, napi_value handler)
{
    send_completion_state_t *state = new (std::nothrow) send_completion_state_t ();
    if (!state) {
        napi_throw_error (env, NULL, "send completion handler allocation failed");
        return false;
    }
    state->socket = socket;
    state->env = env;

    if (g_send_completion_states.find (socket) != g_send_completion_states.end ()) {
        delete state;
        napi_throw_error (env, NULL, "send completion handler already attached");
        return false;
    }
    g_send_completion_states[socket] = state;

    napi_value resource_name;
    napi_create_string_utf8 (env, "zlink-send-completion", NAPI_AUTO_LENGTH, &resource_name);
    if (napi_create_threadsafe_function (
          env, handler, NULL, resource_name, 0, 1, state,
          send_completion_tsfn_finalize, state, send_completion_tsfn_call_js,
          &state->tsfn) != napi_ok) {
        g_send_completion_states.erase (socket);
        delete state;
        napi_throw_error (env, NULL, "send completion handler callback queue setup failed");
        return false;
    }
    (void) napi_unref_threadsafe_function (env, state->tsfn);

    const zlink_handler_result_t result =
      zlink_send_complete_handler (socket, send_completion_callback, state);
    if (result != ZLINK_HANDLER_OK) {
        g_send_completion_states.erase (socket);
        (void) napi_release_threadsafe_function (state->tsfn, napi_tsfn_abort);
        throw_last_error (env, "send completion handler failed");
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
        std::lock_guard<std::mutex> state_lock (slot->mutex);
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

void release_socket_send_completion_handler (void *socket)
{
    napi_threadsafe_function tsfn = NULL;
    std::unordered_map<void *, send_completion_state_t *>::iterator entry =
      g_send_completion_states.find (socket);
    if (entry == g_send_completion_states.end ())
        return;
    send_completion_state_t *state = entry->second;
    tsfn = state->tsfn;
    //  Runs on the JavaScript thread while the socket closes. Drop the
    //  outstanding-operation reference here rather than leaving it to the
    //  queued callbacks: a completion that never reaches the queue would
    //  otherwise hold the event loop open for the life of the process.
    //  Zeroing the counter first keeps the callbacks that do arrive from
    //  releasing it a second time.
    if (state->js_thread_outstanding > 0) {
        state->js_thread_outstanding = 0;
        (void) napi_unref_threadsafe_function (state->env, tsfn);
    }
    g_send_completion_states.erase (entry);
    if (tsfn)
        (void) napi_release_threadsafe_function (tsfn, napi_tsfn_release);
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
        std::lock_guard<std::mutex> state_lock (state->mutex);
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

napi_value throw_submit_error (napi_env env, const char *prefix, int result)
{
    int err = zlink_errno ();
    const char *msg = zlink_strerror (err);
    char buf[256];
    snprintf (buf, sizeof (buf), "%s: %s", prefix, msg ? msg : "error");

    napi_value message;
    napi_value error;
    napi_create_string_utf8 (env, buf, NAPI_AUTO_LENGTH, &message);
    napi_create_error (env, NULL, message, &error);
    set_int64_property (env, error, "nativeResult", result);
    napi_throw (env, error);
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
    native_message_frame_t *native_frame;
};

class contiguous_message_input_t
{
  public:
    contiguous_message_input_t () : spans_ (inline_spans_), count_ (0) {}

    bool resize (size_t count)
    {
        count_ = count;
        if (count <= k_inline_span_count) {
            spans_ = inline_spans_;
            return true;
        }
        overflow_spans_.resize (count);
        spans_ = overflow_spans_.data ();
        return true;
    }

    message_value_span_t *data () { return spans_; }
    size_t size () const { return count_; }

    void consume_native_messages () const
    {
        for (size_t index = 0; index < count_; ++index) {
            native_message_frame_t *frame = spans_[index].native_frame;
            if (!frame)
                continue;
            zlink_msg_close (&frame->message);
            zlink_msg_init (&frame->message);
        }
    }

  private:
    static const size_t k_inline_span_count = 8;
    message_value_span_t inline_spans_[k_inline_span_count];
    std::vector<message_value_span_t> overflow_spans_;
    message_value_span_t *spans_;
    size_t count_;
};

bool get_message_value_span (napi_env env, napi_value value, message_value_span_t *span)
{
    span->native_frame = NULL;
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
        span->native_frame = frame;
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

bool init_contiguous_msg_from_array (napi_env env,
                                     napi_value value,
                                     zlink_msg_t *msg,
                                     contiguous_message_input_t *input)
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

    if (!input || !input->resize (length))
        return false;
    size_t total = 0;
    for (uint32_t index = 0; index < length; ++index) {
        napi_value part;
        if (napi_get_element (env, value, index, &part) != napi_ok
            || !get_message_value_span (env, part, &input->data ()[index]))
            return false;
        if (input->data ()[index].size > std::numeric_limits<size_t>::max () - total) {
            napi_throw_range_error (env, NULL, "stream parts are too large");
            return false;
        }
        total += input->data ()[index].size;
    }

    if (zlink_msg_init_size (msg, total) != 0)
        return false;
    unsigned char *write = static_cast<unsigned char *> (zlink_msg_data (msg));
    for (size_t index = 0; index < input->size (); ++index) {
        const message_value_span_t &span = input->data ()[index];
        if (span.size > 0) {
            memcpy (write, span.data, span.size);
            write += span.size;
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
    napi_value data;
    bool has_data = false;
    if (napi_has_named_property (env, value, "data", &has_data) == napi_ok && has_data
        && napi_get_named_property (env, value, "data", &data) == napi_ok) {
        napi_value array_buffer;
        if (napi_get_named_property (env, data, "buffer", &array_buffer) == napi_ok) {
            // A successful send consumes Message ownership. Detaching the
            // previously returned writable Buffer view enforces that boundary
            // before the shared native payload becomes visible to Core.
            napi_detach_arraybuffer (env, array_buffer);
        }
    }
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
    // Message.from() can be submitted without exposing data().  Defer the
    // external Buffer view until JavaScript actually asks for it.
    return create_native_message_value (env, frame, false);
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
    // Allocation owns native storage immediately; its JavaScript Buffer view
    // is created lazily by Message.data().
    return create_native_message_value (env, frame, false);
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

napi_value message_frame_copy_data (napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    if (argc < 1) {
        napi_throw_type_error (env, NULL, "messageFrameCopyData requires a native message frame");
        return NULL;
    }
    native_message_frame_t *frame = get_native_message_frame (
      env, argv[0], "messageFrameCopyData requires a native message frame");
    if (!frame)
        return NULL;
    return create_received_message_buffer (env, &frame->message);
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

napi_value ctx_get_auto_hwm_budget_snapshot (napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *ctx = NULL;
    napi_get_value_external (env, argv[0], &ctx);
    zlink_auto_hwm_budget_snapshot_t snapshot;
    memset (&snapshot, 0, sizeof (snapshot));
    snapshot.abi_version = ZLINK_AUTO_HWM_BUDGET_SNAPSHOT_ABI_V1;
    snapshot.struct_size = sizeof (snapshot);
    const zlink_config_result_t rc =
      zlink_ctx_get_auto_hwm_budget_snapshot (ctx, &snapshot);
    if (rc != ZLINK_CONFIG_OK)
        return throw_last_error (env, "ctx_get_auto_hwm_budget_snapshot failed");

    napi_value out;
    napi_create_object (env, &out);
    set_uint32_property (env, out, "abiVersion", snapshot.abi_version);
    set_uint32_property (env, out, "structSize", snapshot.struct_size);
    set_uint64_bigint_property (env, out, "budgetGeneration", snapshot.budget_generation);
    set_uint64_bigint_property (env, out, "measurementEpoch", snapshot.measurement_epoch);
    set_uint64_bigint_property (env, out, "configuredMemoryLimitBytes",
                                snapshot.configured_memory_limit_bytes);
    set_uint64_bigint_property (env, out, "runtimeMemoryLimitBytes",
                                snapshot.runtime_memory_limit_bytes);
    set_uint64_bigint_property (env, out, "resolvedMemoryLimitBytes",
                                snapshot.resolved_memory_limit_bytes);
    set_uint64_bigint_property (env, out, "configuredCoreBudgetBytes",
                                snapshot.configured_core_budget_bytes);
    set_uint64_bigint_property (env, out, "effectiveCoreBudgetBytes",
                                snapshot.effective_core_budget_bytes);
    set_uint64_bigint_property (env, out, "totalPlannedHwmBytes",
                                snapshot.total_planned_hwm_bytes);
    set_uint64_bigint_property (env, out, "totalAppliedHwmBytes",
                                snapshot.total_applied_hwm_bytes);
    set_uint64_bigint_property (env, out, "manualReservedHwmBytes",
                                snapshot.manual_reserved_hwm_bytes);
    set_uint64_bigint_property (env, out, "coreQueueAccountedBytes",
                                snapshot.core_queue_accounted_bytes);
    set_uint64_bigint_property (env, out, "applicationAccountedBytes",
                                snapshot.application_accounted_bytes);
    set_uint64_bigint_property (env, out, "currentAccountedBytes",
                                snapshot.current_accounted_bytes);
    set_uint64_bigint_property (env, out, "provisionalAccountedBytes",
                                snapshot.provisional_accounted_bytes);
    set_uint64_bigint_property (env, out, "peakAccountedBytes",
                                snapshot.peak_accounted_bytes);
    set_uint64_bigint_property (env, out, "completionCurrentAccountedBytes",
                                snapshot.completion_current_accounted_bytes);
    set_uint64_bigint_property (env, out, "completionPeakAccountedBytes",
                                snapshot.completion_peak_accounted_bytes);
    set_uint64_bigint_property (env, out, "completionPendingMessageCount",
                                snapshot.completion_pending_message_count);
    set_uint64_bigint_property (env, out, "totalMessagingAccountedBytes",
                                snapshot.total_messaging_accounted_bytes);
    set_uint64_bigint_property (env, out, "monitorQueueAppliedHwmBytes",
                                snapshot.monitor_queue_applied_hwm_bytes);
    set_uint64_bigint_property (env, out, "monitorQueueAccountedBytes",
                                snapshot.monitor_queue_accounted_bytes);
    set_uint64_bigint_property (env, out, "totalInstanceAppliedHwmBytes",
                                snapshot.total_instance_applied_hwm_bytes);
    set_uint64_bigint_property (env, out, "totalInstanceAccountedBytes",
                                snapshot.total_instance_accounted_bytes);
    set_uint64_bigint_property (env, out, "oversizeAdmissionCount",
                                snapshot.oversize_admission_count);
    set_uint64_bigint_property (env, out, "largestOversizeMessageBytes",
                                snapshot.largest_oversize_message_bytes);
    set_uint64_bigint_property (env, out, "activeDirectionalQueueCount",
                                snapshot.active_directional_queue_count);
    set_uint64_bigint_property (env, out, "activeCompletionDirectionalQueueCount",
                                snapshot.active_completion_directional_queue_count);
    set_uint64_bigint_property (env, out, "activeSendQueueCount",
                                snapshot.active_send_queue_count);
    set_uint64_bigint_property (env, out, "activeReceiveQueueCount",
                                snapshot.active_receive_queue_count);
    set_uint64_bigint_property (env, out, "outstandingApplicationLeaseCount",
                                snapshot.outstanding_application_lease_count);
    set_uint64_bigint_property (env, out, "retiredQueueCount",
                                snapshot.retired_queue_count);
    set_uint64_bigint_property (env, out, "deferredOriginCreditBytes",
                                snapshot.deferred_origin_credit_bytes);
    set_uint64_bigint_property (env, out, "unlimitedManualQueueCount",
                                snapshot.unlimited_manual_queue_count);
    set_uint32_property (env, out, "blockedRatioPpm", snapshot.blocked_ratio_ppm);
    set_uint32_property (env, out, "flags", snapshot.flags);
    napi_value reserved;
    napi_create_array_with_length (env, 8, &reserved);
    for (uint32_t index = 0; index < 8; ++index) {
        napi_value value;
        napi_create_bigint_uint64 (env, snapshot.reserved_u64[index], &value);
        napi_set_element (env, reserved, index, value);
    }
    napi_set_named_property (env, out, "reservedUInt64", reserved);
    return out;
}

napi_value ctx_reset_auto_hwm_budget_metrics (napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *ctx = NULL;
    napi_get_value_external (env, argv[0], &ctx);
    const zlink_config_result_t rc = zlink_ctx_reset_auto_hwm_budget_metrics (ctx);
    if (rc != ZLINK_CONFIG_OK)
        return throw_last_error (env, "ctx_reset_auto_hwm_budget_metrics failed");
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
    release_socket_send_completion_handler (sock);
    release_socket_request_dispatcher (sock);
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value test_begin_held_routed_multipart (napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    if (argc < 2) {
        napi_throw_type_error (env, NULL, "test hook requires socket and routing id");
        return NULL;
    }

    held_routed_multipart_test_t *state =
      new (std::nothrow) held_routed_multipart_test_t ();
    if (!state) {
        napi_throw_error (env, NULL, "test hook allocation failed");
        return NULL;
    }
    napi_get_value_external (env, argv[0], &state->socket);
    if (!state->socket || !parse_routing_id_value (env, argv[1], &state->routing_id)) {
        delete state;
        return NULL;
    }

    state->worker = std::thread (run_held_routed_multipart_test, state);
    {
        std::unique_lock<std::mutex> lock (state->mutex);
        state->condition.wait (lock, [state] { return state->opened; });
    }

    napi_value out;
    napi_value external;
    napi_create_object (env, &out);
    napi_create_external (env, state, NULL, NULL, &external);
    napi_set_named_property (env, out, "state", external);
    set_int64_property (env, out, "openResult", state->open_result);
    set_int64_property (env, out, "openErrno", state->open_errno);
    return out;
}

napi_value test_end_held_routed_multipart (napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    held_routed_multipart_test_t *state = NULL;
    if (argc < 1
        || napi_get_value_external (
             env, argv[0], reinterpret_cast<void **> (&state)) != napi_ok
        || !state) {
        napi_throw_type_error (env, NULL, "test hook state is invalid");
        return NULL;
    }

    {
        std::lock_guard<std::mutex> lock (state->mutex);
        state->finish = true;
    }
    state->condition.notify_one ();
    if (state->worker.joinable ())
        state->worker.join ();

    napi_value out;
    napi_create_object (env, &out);
    set_int64_property (env, out, "finalResult", state->final_result);
    set_int64_property (env, out, "finalErrno", state->final_errno);
    delete state;
    return out;
}

napi_value test_run_send_close_stress (napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    uint32_t thread_count = 4;
    uint32_t iterations = 10000;
    if (argc >= 1)
        napi_get_value_uint32 (env, argv[0], &thread_count);
    if (argc >= 2)
        napi_get_value_uint32 (env, argv[1], &iterations);
    if (thread_count == 0 || thread_count > 32 || iterations == 0) {
        napi_throw_range_error (env, NULL, "stress dimensions are invalid");
        return NULL;
    }

    void *context = zlink_ctx_new ();
    void *sender = context ? zlink_socket (context, ZLINK_SOCKET_PAIR) : NULL;
    void *receiver = context ? zlink_socket (context, ZLINK_SOCKET_PAIR) : NULL;
    if (!context || !sender || !receiver) {
        if (sender)
            zlink_close (sender);
        if (receiver)
            zlink_close (receiver);
        if (context)
            zlink_ctx_term (context);
        return throw_last_error (env, "stress setup failed");
    }

    static std::atomic<uint64_t> next_endpoint (1);
    char endpoint[96];
    snprintf (endpoint, sizeof (endpoint), "inproc://node-send-close-stress-%llu",
              static_cast<unsigned long long> (
                next_endpoint.fetch_add (1, std::memory_order_relaxed)));
    if (zlink_bind (receiver, endpoint) != ZLINK_BIND_OK
        || zlink_connect (sender, endpoint) != ZLINK_CONNECT_OK) {
        zlink_close (sender);
        zlink_close (receiver);
        zlink_ctx_term (context);
        return throw_last_error (env, "stress connect failed");
    }

    send_close_stress_counts_t counts;
    std::atomic<uint32_t> ready (0);
    std::atomic<bool> start (false);
    std::atomic<bool> senders_done (false);
    std::vector<std::thread> senders;
    senders.reserve (thread_count);
    for (uint32_t index = 0; index < thread_count; ++index) {
        senders.emplace_back (
          run_send_close_stress_sender, sender, index, iterations,
          &ready, &start, &counts);
    }
    std::thread receiver_thread (
      run_send_close_stress_receiver, receiver, &senders_done, &counts);
    const uint64_t total_attempts =
      static_cast<uint64_t> (thread_count) * iterations;
    std::thread closer_thread (
      run_send_close_stress_closer, sender, total_attempts, &counts);

    while (ready.load (std::memory_order_acquire) != thread_count)
        std::this_thread::yield ();
    start.store (true, std::memory_order_release);
    for (size_t index = 0; index < senders.size (); ++index)
        senders[index].join ();
    closer_thread.join ();
    senders_done.store (true, std::memory_order_release);
    receiver_thread.join ();

    zlink_close (receiver);
    zlink_ctx_term (context);

    napi_value out;
    napi_create_object (env, &out);
#define SET_STRESS_COUNT(name)                                                                    \
    set_uint64_bigint_property (                                                                  \
      env, out, #name, counts.name.load (std::memory_order_relaxed))
    SET_STRESS_COUNT (attempts);
    SET_STRESS_COUNT (single_attempts);
    SET_STRESS_COUNT (multipart_attempts);
    SET_STRESS_COUNT (submitted);
    SET_STRESS_COUNT (rejected_einval);
    SET_STRESS_COUNT (shutdown);
    SET_STRESS_COUNT (backpressured);
    SET_STRESS_COUNT (other_submit);
    SET_STRESS_COUNT (received_records);
    SET_STRESS_COUNT (bad_records);
    SET_STRESS_COUNT (bad_first_parts);
    SET_STRESS_COUNT (bad_mixed_parts);
    SET_STRESS_COUNT (bad_part_counts);
    SET_STRESS_COUNT (bad_next_part_results);
    SET_STRESS_COUNT (close_ok);
    SET_STRESS_COUNT (close_busy);
    SET_STRESS_COUNT (close_shutdown);
    SET_STRESS_COUNT (close_other);
#undef SET_STRESS_COUNT
    return out;
}

napi_value socket_send_completion_handler (napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    if (argc < 2) {
        napi_throw_type_error (
          env, NULL, "socketSendCompletionHandler requires socket and handler");
        return NULL;
    }
    void *socket = NULL;
    napi_get_value_external (env, argv[0], &socket);
    napi_valuetype handler_type = napi_undefined;
    napi_typeof (env, argv[1], &handler_type);
    if (!socket || handler_type != napi_function) {
        napi_throw_type_error (env, NULL, "send completion handler is invalid");
        return NULL;
    }
    if (!attach_send_completion_handler (env, socket, argv[1]))
        return NULL;
    napi_value out;
    napi_get_undefined (env, &out);
    return out;
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
        return throw_submit_error (env, "send failed", rc);
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
        return throw_submit_error (env, "sendParts failed", rc);
    }
    consume_native_message_value (env, argv[1]);

    napi_value out;
    napi_create_int32 (env, static_cast<int32_t> (total), &out);
    return out;
}

static napi_value create_send_submit_result (napi_env env,
                                             int result,
                                             int native_errno,
                                             uint64_t op_id)
{
    napi_value out;
    napi_create_object (env, &out);
    set_int64_property (env, out, "result", result);
    set_int64_property (env, out, "nativeErrno", native_errno);
    set_uint64_bigint_property (env, out, "opId", op_id);
    return out;
}

static napi_value create_inline_send_completion (napi_env env,
                                                 uint64_t token,
                                                 const zlink_send_complete_event_t &event)
{
    napi_value out;
    napi_create_object (env, &out);
    set_uint64_bigint_property (env, out, "token", token);
    set_uint64_bigint_property (env, out, "opId", event.op_id);
    set_int64_property (env, out, "result", event.result);
    set_int64_property (env, out, "terminalErrno", event.terminal_errno);
    napi_value peer_rid = create_routing_id_value (env, event.peer_rid);
    napi_set_named_property (env, out, "peerRid", peer_rid);
    set_uint64_bigint_property (env, out, "transportPairId", event.transport_pair_id);
    set_uint64_bigint_property (
      env, out, "transportPairGeneration", event.transport_pair_generation);
    return out;
}

napi_value socket_send_async (napi_env env, napi_callback_info info)
{
    napi_value argv[5];
    size_t argc = 5;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    if (argc < 5) {
        napi_throw_type_error (
          env, NULL,
          "socketSendAsync requires (socket, parts, timeoutMs, routingIdOrNull, token)");
        return NULL;
    }

    void *socket = NULL;
    napi_get_value_external (env, argv[0], &socket);
    send_completion_state_t *state = find_send_completion_state (socket);
    if (!state || !state->tsfn) {
        napi_throw_error (env, NULL, "send completion handler is not attached");
        return NULL;
    }

    int32_t timeout_ms = 0;
    if (napi_get_value_int32 (env, argv[2], &timeout_ms) != napi_ok
        || timeout_ms < -1) {
        napi_throw_range_error (env, NULL, "send timeout must be -1 or non-negative");
        return NULL;
    }
    uint64_t token = 0;
    if (!get_uint64_like (env, argv[4], &token)) {
        napi_throw_type_error (env, NULL, "send token must be uint64");
        return NULL;
    }

    zlink_routed_submit_target_t target;
    const zlink_routed_submit_target_t *target_ptr = NULL;
    napi_value null_value;
    bool is_null = false;
    napi_get_null (env, &null_value);
    napi_strict_equals (env, argv[3], null_value, &is_null);
    if (!is_null) {
        zlink_routing_id_t routing_id;
        if (!parse_routing_id_value (env, argv[3], &routing_id))
            return NULL;
        const int target_result =
          zlink_select_routed_submit_target (socket, &routing_id, &target);
        if (target_result != ZLINK_SUBMIT_OK) {
            return create_send_submit_result (
              env, target_result, zlink_errno (), 0);
        }
        target_ptr = &target;
    }

    std::vector<zlink_msg_t> parts;
    if (!build_msg_vector_or_single (env, argv[1], &parts))
        return NULL;

    send_async_operation_t *operation =
      new (std::nothrow) send_async_operation_t (state, token);
    if (!operation) {
        close_msg_vector (parts);
        napi_throw_error (env, NULL, "send operation allocation failed");
        return NULL;
    }

    zlink_send_async_options_t options;
    memset (&options, 0, sizeof (options));
    options.struct_size = sizeof (options);
    options.timeout_ms = timeout_ms > 0 ? static_cast<uint32_t> (timeout_ms) : 0u;
    options.userdata = operation;
    options.target = target_ptr;
    zlink_send_op_id_t op_id = 0;
    const int result = zlink_send_async (
      socket, parts.data (), parts.size (), &options, &op_id);
    if (result != ZLINK_SUBMIT_OK) {
        const int native_errno = zlink_errno ();
        close_msg_vector (parts);
        delete operation;
        return create_send_submit_result (env, result, native_errno, 0);
    }
    // Keep the Message ownership transition identical to the synchronous
    // send path: detach any previously exposed writable Buffer view while
    // leaving Core's copied/admitted record independent of the wrapper.
    consume_native_message_value (env, argv[1]);
    parts.clear ();

    bool inline_completed = false;
    zlink_send_complete_event_t inline_event;
    memset (&inline_event, 0, sizeof (inline_event));
    if (op_id == 0) {
        // Core admitted the record directly and intentionally emits no
        // completion callback. Return the completion in the submit result so
        // JavaScript resolves the Promise without a TSFN hop.
        operation->submit_returned = true;
        inline_completed = true;
        inline_event.userdata = operation;
        inline_event.result = ZLINK_SEND_ADMITTED;
    } else {
        std::lock_guard<std::mutex> lock (operation->mutex);
        operation->submit_returned = true;
        inline_completed = operation->completed;
        if (inline_completed)
            inline_event = operation->event;
    }

    if (!inline_completed && state->js_thread_outstanding++ == 0) {
        //  Taken before returning to JavaScript. The threadsafe callback that
        //  releases it can only run once this native call has returned, so the
        //  0 -> 1 transition cannot race its own release.
        (void) napi_ref_threadsafe_function (env, state->tsfn);
    }
    napi_value out = create_send_submit_result (env, result, 0, op_id);
    if (inline_completed) {
        napi_value completion =
          create_inline_send_completion (env, token, inline_event);
        napi_set_named_property (env, out, "inlineCompletion", completion);
        delete operation;
    }
    return out;
}

static napi_value create_request_submit_result (napi_env env,
                                                int result,
                                                int native_errno)
{
    napi_value out;
    napi_create_object (env, &out);
    set_int64_property (env, out, "result", result);
    set_int64_property (env, out, "nativeErrno", native_errno);
    return out;
}

static int dealer_request_parts (void *dealer,
                                 std::vector<zlink_msg_t> *parts,
                                 uint32_t timeout_ms,
                                 request_js_state_t *state)
{
    return submit_msg_parts (
      parts->data (), parts->size (),
      [dealer, timeout_ms, state] (zlink_msg_t *part,
                                   zlink_part_flag_t part_flag,
                                   bool is_final) {
          return zlink_dealer_request_part (
            dealer, part, ZLINK_SEND_FLAGS_DONTWAIT, part_flag,
            is_final ? timeout_ms : 0u,
            is_final ? request_reply_callback_trampoline : NULL,
            is_final ? state : NULL);
      });
}

static int router_request_parts (void *router,
                                 const zlink_routing_id_t *peer_rid,
                                 const zlink_routed_submit_target_t *target,
                                 std::vector<zlink_msg_t> *parts,
                                 uint32_t timeout_ms,
                                 request_js_state_t *state)
{
    return submit_msg_parts (
      parts->data (), parts->size (),
      [router, peer_rid, target, timeout_ms, state] (
        zlink_msg_t *part, zlink_part_flag_t part_flag, bool is_final) {
          if (target) {
              return zlink_router_request_transport_pair_part (
                router, &target->peer_rid, target->transport_pair_id,
                target->transport_pair_generation, part,
                ZLINK_SEND_FLAGS_DONTWAIT, part_flag,
                is_final ? timeout_ms : 0u,
                is_final ? request_reply_callback_trampoline : NULL,
                is_final ? state : NULL);
          }
          return zlink_router_request_part (
            router, peer_rid, part, ZLINK_SEND_FLAGS_DONTWAIT, part_flag,
            is_final ? timeout_ms : 0u,
            is_final ? request_reply_callback_trampoline : NULL,
            is_final ? state : NULL);
      });
}

napi_value dealer_request (napi_env env, napi_callback_info info)
{
    napi_value argv[4];
    size_t argc = 4;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    if (argc < 4) {
        napi_throw_type_error (
          env, NULL, "dealerRequest requires (socket, parts, token, timeoutMs)");
        return NULL;
    }

    void *dealer = NULL;
    napi_get_value_external (env, argv[0], &dealer);
    uint64_t token = 0;
    if (!get_uint64_like (env, argv[2], &token)) {
        napi_throw_type_error (env, NULL, "request token must be uint64");
        return NULL;
    }
    int32_t timeout_ms = 0;
    if (napi_get_value_int32 (env, argv[3], &timeout_ms) != napi_ok
        || timeout_ms <= 0) {
        napi_throw_range_error (env, NULL, "request timeout must be positive");
        return NULL;
    }

    std::vector<zlink_msg_t> parts;
    if (!build_msg_vector_or_single (env, argv[1], &parts))
        return NULL;
    request_js_state_t *state = create_core_request_js_state (
      env, dealer, token);
    if (!state) {
        close_msg_vector (parts);
        return NULL;
    }
    const int result = dealer_request_parts (
      dealer, &parts, static_cast<uint32_t> (timeout_ms), state);
    parts.clear ();
    const int native_errno = result == ZLINK_SUBMIT_OK ? 0 : zlink_errno ();
    if (result == ZLINK_SUBMIT_OK)
        consume_native_message_value (env, argv[1]);
    if (result != ZLINK_SUBMIT_OK)
        abort_request_js_state (state);
    return create_request_submit_result (env, result, native_errno);
}

napi_value router_request (napi_env env, napi_callback_info info)
{
    napi_value argv[7];
    size_t argc = 7;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    if (argc < 5) {
        napi_throw_type_error (
          env, NULL,
          "routerRequest requires (socket, routingId, parts, token, timeoutMs, pairId?, pairGeneration?)");
        return NULL;
    }

    void *router = NULL;
    napi_get_value_external (env, argv[0], &router);
    zlink_routing_id_t peer_rid;
    if (!parse_routing_id_value (env, argv[1], &peer_rid))
        return NULL;
    uint64_t token = 0;
    if (!get_uint64_like (env, argv[3], &token)) {
        napi_throw_type_error (env, NULL, "request token must be uint64");
        return NULL;
    }
    int32_t timeout_ms = 0;
    if (napi_get_value_int32 (env, argv[4], &timeout_ms) != napi_ok
        || timeout_ms <= 0) {
        napi_throw_range_error (env, NULL, "request timeout must be positive");
        return NULL;
    }

    uint64_t pair_id = 0;
    uint64_t pair_generation = 0;
    if (argc >= 7) {
        if (!get_uint64_like (env, argv[5], &pair_id)
            || !get_uint64_like (env, argv[6], &pair_generation)) {
            napi_throw_type_error (env, NULL, "transport pair identity must be uint64");
            return NULL;
        }
        if ((pair_id == 0) != (pair_generation == 0)) {
            napi_throw_range_error (
              env, NULL, "transport pair identity must be both zero or non-zero");
            return NULL;
        }
    }

    zlink_routed_submit_target_t target;
    const zlink_routed_submit_target_t *target_ptr = NULL;
    if (pair_id != 0) {
        target.peer_rid = peer_rid;
        target.transport_pair_id = pair_id;
        target.transport_pair_generation = pair_generation;
        target_ptr = &target;
    }

    std::vector<zlink_msg_t> parts;
    if (!build_msg_vector_or_single (env, argv[2], &parts))
        return NULL;
    request_js_state_t *state = create_core_request_js_state (
      env, router, token);
    if (!state) {
        close_msg_vector (parts);
        return NULL;
    }
    const int result = router_request_parts (
      router, &peer_rid, target_ptr, &parts,
      static_cast<uint32_t> (timeout_ms), state);
    parts.clear ();
    const int native_errno = result == ZLINK_SUBMIT_OK ? 0 : zlink_errno ();
    if (result == ZLINK_SUBMIT_OK)
        consume_native_message_value (env, argv[2]);
    if (result != ZLINK_SUBMIT_OK)
        abort_request_js_state (state);
    return create_request_submit_result (env, result, native_errno);
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
    bool is_buf = false;
    bool is_array = false;
    bool contains_native_frame = false;
    if (napi_is_buffer (env, argv[2], &is_buf) == napi_ok && is_buf) {
        if (!init_msg_from_value (env, argv[2], &single_part,
                                  &contains_native_frame))
            return throw_last_error (env, "publish failed");
        use_single_part = true;
    } else {
        napi_is_array (env, argv[2], &is_array);
        napi_valuetype payload_type = napi_undefined;
        napi_typeof (env, argv[2], &payload_type);
        if (is_array) {
            if (!build_msg_vector (env, argv[2], &parts))
                return NULL;
        } else if (payload_type == napi_object) {
            if (!init_msg_from_value (env, argv[2], &single_part,
                                      &contains_native_frame))
                return throw_last_error (env, "publish failed");
            use_single_part = true;
        } else if (!build_msg_vector (env, argv[2], &parts)) {
            return NULL;
        }
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
        return throw_submit_error (env, "publish failed", rc);
    }
    if (is_array || contains_native_frame)
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
    bool is_buf = false;
    bool is_array = false;
    bool contains_native_frame = false;
    if (napi_is_buffer (env, argv[2], &is_buf) == napi_ok && is_buf) {
        void *data = NULL;
        size_t len = 0;
        if (napi_get_buffer_info (env, argv[2], &data, &len) != napi_ok) {
            napi_throw_type_error (env, NULL, "publish buffer invalid");
            return NULL;
        }
        if (!init_msg_from_bytes (&single_part, data, len))
            return throw_last_error (env, "publishNoWaitResult failed");
        use_single_part = true;
    } else {
        napi_is_array (env, argv[2], &is_array);
        napi_valuetype payload_type = napi_undefined;
        napi_typeof (env, argv[2], &payload_type);
        if (is_array) {
            if (!build_msg_vector (env, argv[2], &parts))
                return NULL;
        } else if (payload_type == napi_object) {
            if (!init_msg_from_value (env, argv[2], &single_part,
                                      &contains_native_frame))
                return throw_last_error (env, "publishNoWaitResult failed");
            use_single_part = true;
        } else if (!build_msg_vector (env, argv[2], &parts)) {
            return NULL;
        }
    }

    int rc = publish_parts (sock, topic, use_single_part ? &single_part : parts.data (),
                            use_single_part ? 1 : parts.size (), ZLINK_SEND_FLAGS_DONTWAIT);
    if (rc != ZLINK_SUBMIT_OK)
        rc = preserve_try_send_result (rc);
    if (rc < 0) {
        return throw_last_error (env, "publishNoWaitResult failed");
    }
    if (rc == ZLINK_SUBMIT_OK && (is_array || contains_native_frame))
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
        rc = preserve_try_send_result (rc);
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
        rc = preserve_try_send_result (rc);
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
        rc = preserve_try_send_result (rc);
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
        rc = preserve_try_send_result (rc);
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
    contiguous_message_input_t input;
    if (!init_contiguous_msg_from_array (env, argv[2], &msg, &input))
        return NULL;

    int rc = send_parts_rid (sock, &routing_id, &msg, 1, ZLINK_SEND_FLAGS_DONTWAIT);
    if (rc != ZLINK_SUBMIT_OK)
        rc = preserve_try_send_result (rc);
    if (rc < 0)
        return throw_last_error (env, "tryStreamSendPartsTo failed");
    if (rc == ZLINK_SUBMIT_OK)
        input.consume_native_messages ();
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
        return throw_submit_error (env, "send failed", rc);
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
        return throw_submit_error (env, "sendPartsTo failed", rc);
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
    contiguous_message_input_t input;
    if (!init_contiguous_msg_from_array (env, argv[2], &msg, &input))
        return NULL;

    int32_t flags = 0;
    napi_get_value_int32 (env, argv[3], &flags);
    int rc = send_parts_rid (sock, &routing_id, &msg, 1,
                             static_cast<zlink_send_flags_t> (flags));
    if (rc != ZLINK_SUBMIT_OK)
        return throw_submit_error (env, "streamSendPartsTo failed", rc);
    input.consume_native_messages ();

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

int dealer_recv_message_value (napi_env env,
                               void *dealer,
                               int32_t flags,
                               napi_value *out)
{
    if (!out)
        return ZLINK_RECV_INTERNAL_ERROR;

    *out = NULL;
    zlink_msg_t first_part;
    if (zlink_msg_init (&first_part) != 0)
        return ZLINK_RECV_INTERNAL_ERROR;

    uint8_t message_type = ZLINK_DEALER_MESSAGE_RAW;
    uint64_t request_seq = 0;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    const int rc = zlink_dealer_recv_part (
      dealer, &message_type, &request_seq, &first_part, &has_more,
      static_cast<zlink_recv_flags_t> (flags));
    if (rc != ZLINK_RECV_OK) {
        zlink_msg_close (&first_part);
        return rc;
    }

    zlink_routing_id_t empty_routing_id;
    memset (&empty_routing_id, 0, sizeof (empty_routing_id));
    if (has_more == ZLINK_PART_FINAL) {
        *out = create_recv_message_value (env, empty_routing_id, &first_part, 1);
        zlink_msg_close (&first_part);
    } else {
        std::vector<zlink_msg_t> parts;
        if (!append_msg_move (&parts, &first_part)) {
            zlink_msg_close (&first_part);
            errno = ENOMEM;
            return ZLINK_RECV_INTERNAL_ERROR;
        }

        while (has_more != ZLINK_PART_FINAL) {
            zlink_msg_t next_part;
            if (zlink_msg_init (&next_part) != 0) {
                close_msg_vector (parts);
                return ZLINK_RECV_INTERNAL_ERROR;
            }
            uint8_t next_message_type = ZLINK_DEALER_MESSAGE_RAW;
            uint64_t next_request_seq = 0;
            zlink_part_flag_t next_has_more = ZLINK_PART_FINAL;
            const int next_rc = zlink_dealer_recv_part (
              dealer, &next_message_type, &next_request_seq, &next_part, &next_has_more,
              ZLINK_RECV_FLAGS_DONTWAIT);
            if (next_rc != ZLINK_RECV_OK) {
                zlink_msg_close (&next_part);
                close_msg_vector (parts);
                return next_rc;
            }
            if (next_message_type != message_type || next_request_seq != request_seq) {
                zlink_msg_close (&next_part);
                close_msg_vector (parts);
                errno = EPROTO;
                return ZLINK_RECV_INTERNAL_ERROR;
            }
            if (!append_msg_move (&parts, &next_part)) {
                zlink_msg_close (&next_part);
                close_msg_vector (parts);
                errno = ENOMEM;
                return ZLINK_RECV_INTERNAL_ERROR;
            }
            has_more = next_has_more;
        }

        *out = create_recv_message_value (
          env, empty_routing_id, parts.data (), parts.size ());
        close_msg_vector (parts);
    }

    if (!*out)
        return ZLINK_RECV_INTERNAL_ERROR;
    if (message_type == ZLINK_DEALER_MESSAGE_REQUEST && request_seq != 0) {
        napi_value request_seq_value;
        napi_create_bigint_uint64 (env, request_seq, &request_seq_value);
        napi_set_named_property (env, *out, "requestSeq", request_seq_value);
    }
    return ZLINK_RECV_OK;
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

napi_value dealer_recv_message (napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *dealer = NULL;
    napi_get_value_external (env, argv[0], &dealer);
    int32_t flags = 0;
    if (argc >= 2)
        napi_get_value_int32 (env, argv[1], &flags);

    napi_value out;
    const int rc = dealer_recv_message_value (env, dealer, flags, &out);
    if (rc != ZLINK_RECV_OK)
        return throw_last_error (env, "dealerRecvMessage failed");
    return out;
}

napi_value dealer_try_recv_message (napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *dealer = NULL;
    napi_get_value_external (env, argv[0], &dealer);

    napi_value out;
    const int rc = dealer_recv_message_value (
      env, dealer, static_cast<int32_t> (ZLINK_RECV_FLAGS_DONTWAIT), &out);
    if (rc != ZLINK_RECV_OK) {
        if (zlink_errno () == EAGAIN) {
            napi_value none;
            napi_get_null (env, &none);
            return none;
        }
        return throw_last_error (env, "dealerRecvMessageNoWait failed");
    }
    return out;
}

napi_value dealer_reply (napi_env env, napi_callback_info info)
{
    napi_value argv[3];
    size_t argc = 3;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    if (argc < 3) {
        napi_throw_type_error (env, NULL,
                               "dealerReply requires (socket, requestSeq, parts)");
        return NULL;
    }
    void *dealer = NULL;
    napi_get_value_external (env, argv[0], &dealer);
    uint64_t request_seq = 0;
    if (!get_uint64_like (env, argv[1], &request_seq)) {
        napi_throw_type_error (env, NULL, "requestSeq must be uint64");
        return NULL;
    }

    std::vector<zlink_msg_t> parts;
    zlink_msg_t single_part;
    bool use_single_part = false;
    bool is_array = false;
    if (napi_is_array (env, argv[2], &is_array) == napi_ok && is_array) {
        if (!build_msg_vector (env, argv[2], &parts))
            return NULL;
    } else {
        if (!init_msg_from_value (env, argv[2], &single_part))
            return NULL;
        use_single_part = true;
    }
    const int rc = dealer_reply_parts (
      dealer, request_seq, use_single_part ? &single_part : parts.data (),
      use_single_part ? 1 : parts.size ());
    if (rc != ZLINK_SUBMIT_OK)
        return throw_submit_error (env, "dealerReply failed", rc);
    consume_native_message_value (env, argv[2]);
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
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
        std::vector<zlink_msg_t> parts;
        const int rc = subscribe_parts (sock, &routing_id, topic.data (), topic.size (), &topic_len,
                                        &parts, ZLINK_RECV_FLAGS_DONTWAIT);
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
        const int err = zlink_errno ();
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
        std::lock_guard<std::mutex> state_lock (slot->mutex);
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

napi_value socket_set_receive_flow_state (napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *sock = NULL;
    napi_get_value_external (env, argv[0], &sock);
    int32_t state = 0;
    napi_get_value_int32 (env, argv[1], &state);
    const zlink_config_result_t rc = zlink_socket_set_receive_flow_state (
      sock, static_cast<zlink_receive_flow_state_t> (state));
    if (rc != ZLINK_CONFIG_OK)
        return throw_last_error (env, "socket_set_receive_flow_state failed");
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
        return throw_submit_error (env, "routerSendTransportPair failed", rc);
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
        return throw_submit_error (env, "routerReply failed", rc);
    }
    consume_native_message_value (env, argv[3]);
    napi_value ok;
    napi_get_undefined (env, &ok);
    return ok;
}

napi_value router_recv_message (napi_env env, napi_callback_info info)
{
    napi_value argv[4];
    size_t argc = 4;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *router = NULL;
    napi_get_value_external (env, argv[0], &router);
    int32_t flags = 0;
    if (argc >= 2)
        napi_get_value_int32 (env, argv[1], &flags);
    bool prefer_managed_single_part = false;
    if (argc >= 3)
        napi_get_value_bool (env, argv[2], &prefer_managed_single_part);
    napi_value routing_id_storage = argc >= 4 ? argv[3] : NULL;

    napi_value out = NULL;
    const int rc = router_recv_message_value (
      env, router, flags, prefer_managed_single_part, routing_id_storage, &out);
    if (rc != ZLINK_RECV_OK)
        return throw_last_error (env, "routerRecvMessage failed");
    return out;
}

napi_value router_try_recv_message (napi_env env, napi_callback_info info)
{
    napi_value argv[3];
    size_t argc = 3;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *router = NULL;
    napi_get_value_external (env, argv[0], &router);
    bool prefer_managed_single_part = false;
    if (argc >= 2)
        napi_get_value_bool (env, argv[1], &prefer_managed_single_part);
    napi_value routing_id_storage = argc >= 3 ? argv[2] : NULL;

    napi_value out = NULL;
    const int rc = router_recv_message_value (
      env, router, ZLINK_RECV_FLAGS_DONTWAIT, prefer_managed_single_part,
      routing_id_storage, &out);
    if (rc != ZLINK_RECV_OK) {
        if (zlink_errno () == EAGAIN) {
            napi_value none;
            napi_get_null (env, &none);
            return none;
        }
        return throw_last_error (env, "routerRecvMessageNoWait failed");
    }

    return out;
}

napi_value monitor_open (napi_env env, napi_callback_info info)
{
    napi_value argv[3];
    size_t argc = 3;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *sock = NULL;
    napi_get_value_external (env, argv[0], &sock);
    int32_t events = 0;
    napi_get_value_int32 (env, argv[1], &events);
    uint64_t monitor_hwm_bytes = 0;
    bool monitor_hwm_lossless = false;
    if (argc < 3
        || napi_get_value_bigint_uint64 (
             env, argv[2], &monitor_hwm_bytes, &monitor_hwm_lossless)
             != napi_ok
        || !monitor_hwm_lossless) {
        napi_throw_type_error (
          env, NULL, "monitorHwmBytes must be a lossless uint64 bigint");
        return NULL;
    }
    zlink_socket_monitor_open_options_t options{};
    options.events = static_cast<zlink_socket_monitor_event_mask_t> (events);
    options.monitor_hwm_bytes = monitor_hwm_bytes;
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
    int rc = zlink_socket_monitor_recv (mon, &evt, ZLINK_RECV_FLAGS_NONE);
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
    int rc = zlink_socket_monitor_recv (mon, &evt, ZLINK_RECV_FLAGS_DONTWAIT);
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
    std::mutex mutex;
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
    std::lock_guard<std::mutex> state_lock (state->mutex);
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
        std::lock_guard<std::mutex> state_lock (state->mutex);
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

    std::unique_ptr<uint64_t> payload (new uint64_t (fire_count_));
    napi_threadsafe_function tsfn = NULL;
    {
        std::lock_guard<std::mutex> lock (state->mutex);
        if (!state->used || !state->tsfn)
            return;
        tsfn = state->tsfn;
        if (napi_acquire_threadsafe_function (tsfn) != napi_ok)
            return;
    }

    if (napi_call_threadsafe_function (tsfn, payload.get (), napi_tsfn_nonblocking) != napi_ok) {
        (void) napi_release_threadsafe_function (tsfn, napi_tsfn_release);
        return;
    }
    (void) payload.release ();
    (void) napi_release_threadsafe_function (tsfn, napi_tsfn_release);
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
        std::lock_guard<std::mutex> state_lock (slot->mutex);
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
