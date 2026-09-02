/* SPDX-License-Identifier: MPL-2.0 */

#include "addon_core_api.h"
#include "addon_core_options.h"
#include "addon_monitor_status_values.h"
#include "addon_message_values.h"
#include "addon_message_parts.h"
#include "addon_submit_results.h"
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

static std::atomic<uint64_t> g_completion_close_count (0);
static const size_t k_inline_message_part_count = 8;

// Most public records contain one or two parts. Keep those native staging
// messages in the N-API call frame and allocate only for an uncommon larger
// multipart record. This storage never closes messages implicitly: callers
// must either close() the still-owned staging records or release() them after
// Core has consumed every slot.
class small_msg_storage_t
{
  public:
    small_msg_storage_t () : heap_size_ (0), heap_capacity_ (0), stack_size_ (0) {}

    bool prepare (napi_env env, size_t count)
    {
        release ();
        if (count <= k_inline_message_part_count) {
            stack_size_ = count;
            return true;
        }
        if (count > std::numeric_limits<size_t>::max () / sizeof (zlink_msg_t)) {
            napi_throw_error (env, NULL, "message parts allocation failed");
            return false;
        }
        heap_.reset (new (std::nothrow) zlink_msg_t[count]);
        if (!heap_) {
            napi_throw_error (env, NULL, "message parts allocation failed");
            return false;
        }
        heap_size_ = count;
        heap_capacity_ = count;
        return true;
    }

    bool append_move (zlink_msg_t *part)
    {
        if (!part)
            return false;
        if (!heap_ && stack_size_ < k_inline_message_part_count) {
            zlink_msg_t *slot = &stack_[stack_size_];
            if (zlink_msg_init (slot) != 0)
                return false;
            if (zlink_msg_move (slot, part) != 0) {
                zlink_msg_close (slot);
                return false;
            }
            ++stack_size_;
            return true;
        }

        if (!heap_) {
            const size_t capacity = k_inline_message_part_count * 2;
            std::unique_ptr<zlink_msg_t[]> candidate (
              new (std::nothrow) zlink_msg_t[capacity]);
            if (!candidate)
                return false;
            for (size_t index = 0; index < stack_size_; ++index) {
                if (zlink_msg_init (&candidate[index]) != 0) {
                    for (size_t built = 0; built < index; ++built)
                        zlink_msg_close (&candidate[built]);
                    for (size_t remaining = index; remaining < stack_size_; ++remaining)
                        zlink_msg_close (&stack_[remaining]);
                    stack_size_ = 0;
                    return false;
                }
                if (zlink_msg_move (&candidate[index], &stack_[index]) != 0) {
                    for (size_t built = 0; built <= index; ++built)
                        zlink_msg_close (&candidate[built]);
                    for (size_t remaining = index; remaining < stack_size_; ++remaining)
                        zlink_msg_close (&stack_[remaining]);
                    stack_size_ = 0;
                    return false;
                }
            }
            heap_size_ = stack_size_;
            heap_capacity_ = capacity;
            stack_size_ = 0;
            heap_ = std::move (candidate);
        }

        if (heap_size_ == heap_capacity_) {
            if (heap_capacity_ > std::numeric_limits<size_t>::max () / 2
                || heap_capacity_ * 2
                     > std::numeric_limits<size_t>::max () / sizeof (zlink_msg_t))
                return false;
            const size_t capacity = heap_capacity_ * 2;
            std::unique_ptr<zlink_msg_t[]> candidate (
              new (std::nothrow) zlink_msg_t[capacity]);
            if (!candidate)
                return false;
            for (size_t index = 0; index < heap_size_; ++index) {
                if (zlink_msg_init (&candidate[index]) != 0) {
                    for (size_t built = 0; built < index; ++built)
                        zlink_msg_close (&candidate[built]);
                    for (size_t remaining = index; remaining < heap_size_; ++remaining)
                        zlink_msg_close (&heap_[remaining]);
                    heap_.reset ();
                    heap_size_ = 0;
                    heap_capacity_ = 0;
                    return false;
                }
                if (zlink_msg_move (&candidate[index], &heap_[index]) != 0) {
                    for (size_t built = 0; built <= index; ++built)
                        zlink_msg_close (&candidate[built]);
                    for (size_t remaining = index; remaining < heap_size_; ++remaining)
                        zlink_msg_close (&heap_[remaining]);
                    heap_.reset ();
                    heap_size_ = 0;
                    heap_capacity_ = 0;
                    return false;
                }
            }
            heap_ = std::move (candidate);
            heap_capacity_ = capacity;
        }

        if (zlink_msg_init (&heap_[heap_size_]) != 0)
            return false;
        if (zlink_msg_move (&heap_[heap_size_], part) != 0) {
            zlink_msg_close (&heap_[heap_size_]);
            return false;
        }
        ++heap_size_;
        return true;
    }

    zlink_msg_t *data () { return heap_ ? heap_.get () : stack_; }
    const zlink_msg_t *data () const { return heap_ ? heap_.get () : stack_; }
    size_t size () const { return heap_ ? heap_size_ : stack_size_; }
    zlink_msg_t &operator[] (size_t index) { return data ()[index]; }

    void close ()
    {
        if (size () > 0)
            zlink_multipart_close (data (), size ());
        release ();
    }

    void release ()
    {
        heap_.reset ();
        heap_size_ = 0;
        heap_capacity_ = 0;
        stack_size_ = 0;
    }

  private:
    zlink_msg_t stack_[k_inline_message_part_count];
    std::unique_ptr<zlink_msg_t[]> heap_;
    size_t heap_size_;
    size_t heap_capacity_;
    size_t stack_size_;
};

inline int collect_recv_parts (void *socket,
                               zlink_msg_t *first_part,
                               zlink_part_flag_t has_more,
                               small_msg_storage_t *parts)
{
    if (!parts) {
        if (first_part)
            zlink_msg_close (first_part);
        errno = EFAULT;
        return ZLINK_RECV_INTERNAL_ERROR;
    }
    if (!parts->append_move (first_part)) {
        if (first_part)
            zlink_msg_close (first_part);
        errno = ENOMEM;
        return ZLINK_RECV_INTERNAL_ERROR;
    }
    while (has_more) {
        const zlink_routing_id_t *source_rid = NULL;
        zlink_msg_t next_part;
        if (zlink_msg_init (&next_part) != 0) {
            parts->close ();
            return ZLINK_RECV_INTERNAL_ERROR;
        }
        zlink_part_flag_t more = ZLINK_PART_FINAL;
        const int rc = zlink_recv_part (
          socket, &source_rid, &next_part, &more, ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc != ZLINK_RECV_OK) {
            zlink_msg_close (&next_part);
            parts->close ();
            return rc;
        }
        if (!parts->append_move (&next_part)) {
            zlink_msg_close (&next_part);
            parts->close ();
            errno = ENOMEM;
            return ZLINK_RECV_INTERNAL_ERROR;
        }
        has_more = more;
    }
    return ZLINK_RECV_OK;
}

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
        return zlink_send_part (sock, part, flags, part_flag, NULL, NULL);
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
          ZLINK_SEND_FLAGS_NONE, ZLINK_PART_MORE, NULL, NULL);
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
          ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL, NULL, NULL);
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

int reply_parts_016 (void *router,
                        const zlink_routing_id_t *peer_rid,
                        uint64_t reply_token,
                        zlink_msg_t *parts,
                        size_t part_count)
{
    return submit_msg_parts (parts, part_count, [router, peer_rid, reply_token] (
                                                  zlink_msg_t *part,
                                                  zlink_part_flag_t part_flag, bool) {
        return zlink_reply_part (
          router, peer_rid, reply_token, part, part_flag);
    });
}

napi_value create_recv_message_value (napi_env env,
                                      const zlink_routing_id_t &routing_id,
                                      zlink_msg_t *parts,
                                      size_t part_count,
                                      napi_value routing_id_storage = NULL)
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
            napi_value rid = create_routing_id_value_reusing (
              env, routing_id, routing_id_storage);
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
        napi_value rid = create_routing_id_value_reusing (
          env, routing_id, routing_id_storage);
        napi_set_named_property (env, obj, "routingId", rid);
    }
    return obj;
}

napi_value create_router_recv_message_value (napi_env env,
                                             const zlink_routing_id_t &routing_id,
                                             uint64_t reply_token,
                                             zlink_msg_t *parts,
                                             size_t part_count,
                                             bool prefer_managed_single_part,
                                             napi_value routing_id_storage)
{
    napi_value obj;
    if (part_count == 1 && !prefer_managed_single_part) {
        napi_create_object (env, &obj);
        napi_value native_message = move_message_to_native_frame_value (env, &parts[0]);
        if (!native_message)
            return NULL;
        napi_set_named_property (env, obj, "nativeMessage", native_message);
        napi_value rid = create_routing_id_value_reusing (
          env, routing_id, routing_id_storage);
        napi_set_named_property (env, obj, "routingId", rid);
    } else {
        obj = create_recv_message_value (
          env, routing_id, parts, part_count, routing_id_storage);
    }
    if (!obj)
        return NULL;
    if (reply_token != 0) {
        napi_value value;
        napi_create_bigint_uint64 (env, reply_token, &value);
        napi_set_named_property (env, obj, "replyToken", value);
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
    uint64_t reply_token = 0;
    zlink_msg_t first_part;
    if (zlink_msg_init (&first_part) != 0)
        return ZLINK_RECV_INTERNAL_ERROR;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    const int rc = zlink_router_recv_part (
      router, &peer_rid_ptr, &reply_token, &first_part, &has_more,
      static_cast<zlink_recv_flags_t> (flags));
    if (rc != ZLINK_RECV_OK) {
        zlink_msg_close (&first_part);
        return rc;
    }
    copy_routing_id (&peer_rid, peer_rid_ptr);
    if (!has_more) {
        *out = create_router_recv_message_value (
          env, peer_rid, reply_token, &first_part, 1,
          prefer_managed_single_part, routing_id_storage);
        zlink_msg_close (&first_part);
        return *out ? ZLINK_RECV_OK : ZLINK_RECV_INTERNAL_ERROR;
    }

    small_msg_storage_t parts;
    if (!parts.append_move (&first_part)) {
        zlink_msg_close (&first_part);
        errno = ENOMEM;
        return ZLINK_RECV_INTERNAL_ERROR;
    }
    while (has_more) {
        const zlink_routing_id_t *next_peer_rid = NULL;
        uint64_t next_reply_token = 0;
        zlink_msg_t next_part;
        if (zlink_msg_init (&next_part) != 0) {
            parts.close ();
            return ZLINK_RECV_INTERNAL_ERROR;
        }
        zlink_part_flag_t more = ZLINK_PART_FINAL;
        const int next_rc = zlink_router_recv_part (
          router, &next_peer_rid, &next_reply_token, &next_part, &more,
          ZLINK_RECV_FLAGS_DONTWAIT);
        if (next_rc != ZLINK_RECV_OK) {
            zlink_msg_close (&next_part);
            parts.close ();
            return next_rc;
        }
        if (!parts.append_move (&next_part)) {
            zlink_msg_close (&next_part);
            parts.close ();
            errno = ENOMEM;
            return ZLINK_RECV_INTERNAL_ERROR;
        }
        has_more = more;
    }
    *out = create_router_recv_message_value (
      env, peer_rid, reply_token, parts.data (), parts.size (),
      prefer_managed_single_part, routing_id_storage);
    parts.close ();
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

} // namespace

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

static void close_built_msg_storage (small_msg_storage_t *parts, size_t built)
{
    if (!parts)
        return;
    for (size_t index = 0; index < built; ++index)
        zlink_msg_close (&(*parts)[index]);
    parts->release ();
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

bool build_msg_vector (napi_env env, napi_value arr, small_msg_storage_t *out)
{
    uint32_t len = 0;
    if (napi_get_array_length (env, arr, &len) != napi_ok) {
        napi_throw_type_error (env, NULL, "parts must be an array");
        return false;
    }
    if (!out->prepare (env, len))
        return false;
    size_t built = 0;
    for (uint32_t index = 0; index < len; ++index) {
        napi_value value;
        if (napi_get_element (env, arr, index, &value) != napi_ok) {
            close_built_msg_storage (out, built);
            napi_throw_type_error (env, NULL, "parts element read failed");
            return false;
        }
        if (!init_msg_from_value (env, value, &(*out)[index])) {
            close_built_msg_storage (out, built);
            return false;
        }
        ++built;
    }
    return true;
}

bool build_msg_vector_or_single (
  napi_env env, napi_value value, small_msg_storage_t *out)
{
    bool is_array = false;
    if (napi_is_array (env, value, &is_array) == napi_ok && is_array)
        return build_msg_vector (env, value, out);

    if (!out->prepare (env, 1))
        return false;
    if (!init_msg_from_value (env, value, &(*out)[0])) {
        out->release ();
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


static napi_value create_send_submit_result (napi_env env,
                                             int result,
                                             int native_errno)
{
    napi_value out;
    napi_create_object (env, &out);
    set_int64_property (env, out, "result", result);
    set_int64_property (env, out, "nativeErrno", native_errno);
    return out;
}

static napi_value create_completion_submit_result (
  napi_env env, int result, int native_errno, uint64_t completion_id)
{
    napi_value out = create_send_submit_result (env, result, native_errno);
    set_uint64_bigint_property (env, out, "completionId", completion_id);
    return out;
}

static bool parse_optional_routing_id (
  napi_env env, napi_value value, zlink_routing_id_t *storage,
  const zlink_routing_id_t **out)
{
    napi_value null_value;
    bool is_null = false;
    napi_get_null (env, &null_value);
    napi_strict_equals (env, value, null_value, &is_null);
    if (is_null) {
        *out = NULL;
        return true;
    }
    if (!parse_routing_id_value (env, value, storage))
        return false;
    *out = storage;
    return true;
}

static int submit_send_parts_016 (
  void *socket, const zlink_routing_id_t *target, zlink_msg_t *parts,
  size_t part_count, zlink_send_flags_t flags, void *user_context,
  zlink_completion_id_t *completion_id)
{
    return submit_msg_parts (
      parts, part_count,
      [socket, target, flags, user_context, completion_id] (
        zlink_msg_t *part, zlink_part_flag_t part_flag, bool final) {
          return target
            ? zlink_send_part_rid (
                socket, target, part, flags, part_flag,
                final ? user_context : NULL, final ? completion_id : NULL)
            : zlink_send_part (
                socket, part, flags, part_flag,
                final ? user_context : NULL, final ? completion_id : NULL);
      });
}

static int submit_request_parts_016 (
  void *socket, const zlink_routing_id_t *target, zlink_msg_t *parts,
  size_t part_count, zlink_send_flags_t flags, uint32_t timeout_ms,
  void *user_context, zlink_completion_id_t *completion_id)
{
    return submit_msg_parts (
      parts, part_count,
      [socket, target, flags, timeout_ms, user_context, completion_id] (
        zlink_msg_t *part, zlink_part_flag_t part_flag, bool final) {
          return zlink_request_part (
            socket, target, part, flags, part_flag,
            final ? timeout_ms : 0u,
            final ? user_context : NULL, final ? completion_id : NULL);
      });
}

napi_value socket_submit_send (napi_env env, napi_callback_info info)
{
    napi_value argv[5];
    size_t argc = 5;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    if (argc < 5) {
        napi_throw_type_error (
          env, NULL,
          "socketSubmitSend requires (socket, parts, routingIdOrNull, flags, token)");
        return NULL;
    }
    void *socket = NULL;
    napi_get_value_external (env, argv[0], &socket);
    zlink_routing_id_t target_storage;
    const zlink_routing_id_t *target = NULL;
    if (!parse_optional_routing_id (env, argv[2], &target_storage, &target))
        return NULL;
    int32_t flags = 0;
    if (napi_get_value_int32 (env, argv[3], &flags) != napi_ok) {
        napi_throw_type_error (env, NULL, "send flags must be int32");
        return NULL;
    }
    uint64_t token = 0;
    if (!get_uint64_like (env, argv[4], &token)) {
        napi_throw_type_error (env, NULL, "completion token must be uint64");
        return NULL;
    }
    small_msg_storage_t parts;
    if (!build_msg_vector_or_single (env, argv[1], &parts))
        return NULL;
    zlink_completion_id_t completion_id = 0;
    void *user_context = token == 0
      ? NULL : reinterpret_cast<void *> (static_cast<uintptr_t> (token));
    const int result = submit_send_parts_016 (
      socket, target, parts.data (), parts.size (),
      static_cast<zlink_send_flags_t> (flags), user_context, &completion_id);
    const int native_errno = result == ZLINK_SUBMIT_OK ? 0 : zlink_errno ();
    parts.release ();
    if (result == ZLINK_SUBMIT_OK)
        consume_native_message_value (env, argv[1]);
    return create_completion_submit_result (
      env, result, native_errno, completion_id);
}

napi_value socket_submit_request (napi_env env, napi_callback_info info)
{
    napi_value argv[6];
    size_t argc = 6;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    if (argc < 6) {
        napi_throw_type_error (
          env, NULL,
          "socketSubmitRequest requires (socket, targetOrNull, parts, timeoutMs, flags, token)");
        return NULL;
    }
    void *socket = NULL;
    napi_get_value_external (env, argv[0], &socket);
    zlink_routing_id_t target_storage;
    const zlink_routing_id_t *target = NULL;
    if (!parse_optional_routing_id (env, argv[1], &target_storage, &target))
        return NULL;
    int32_t timeout_ms = 0;
    int32_t flags = 0;
    if (napi_get_value_int32 (env, argv[3], &timeout_ms) != napi_ok
        || timeout_ms <= 0) {
        napi_throw_range_error (env, NULL, "request timeout must be positive");
        return NULL;
    }
    if (napi_get_value_int32 (env, argv[4], &flags) != napi_ok) {
        napi_throw_type_error (env, NULL, "request flags must be int32");
        return NULL;
    }
    uint64_t token = 0;
    if (!get_uint64_like (env, argv[5], &token)) {
        napi_throw_type_error (env, NULL, "completion token must be uint64");
        return NULL;
    }
    small_msg_storage_t parts;
    if (!build_msg_vector_or_single (env, argv[2], &parts))
        return NULL;
    zlink_completion_id_t completion_id = 0;
    void *user_context = token == 0
      ? NULL : reinterpret_cast<void *> (static_cast<uintptr_t> (token));
    const int result = submit_request_parts_016 (
      socket, target, parts.data (), parts.size (),
      static_cast<zlink_send_flags_t> (flags),
      static_cast<uint32_t> (timeout_ms), user_context, &completion_id);
    const int native_errno = result == ZLINK_SUBMIT_OK ? 0 : zlink_errno ();
    parts.release ();
    if (result == ZLINK_SUBMIT_OK)
        consume_native_message_value (env, argv[2]);
    return create_completion_submit_result (
      env, result, native_errno, completion_id);
}

class completion_close_guard_t
{
  public:
    explicit completion_close_guard_t (zlink_completion_t *value) : value_ (value) {}
    ~completion_close_guard_t ()
    {
        zlink_completion_close (value_);
        g_completion_close_count.fetch_add (1, std::memory_order_relaxed);
    }
  private:
    zlink_completion_t *value_;
};

static napi_value create_completion_value (
  napi_env env, zlink_completion_t *completion)
{
    completion_close_guard_t guard (completion);
    napi_value out;
    napi_create_object (env, &out);
    set_int64_property (env, out, "kind", completion->kind);
    set_uint64_bigint_property (
      env, out, "completionId", completion->completion_id);
    set_uint64_bigint_property (
      env, out, "userContext",
      static_cast<uint64_t> (
        reinterpret_cast<uintptr_t> (completion->user_context)));
    set_int64_property (env, out, "sendResult", completion->send_result);
    set_int64_property (
      env, out, "terminalErrno", completion->send_terminal_errno);
    set_int64_property (
      env, out, "requestResult", completion->request_result);
    if (completion->kind == ZLINK_COMPLETION_REQUEST
        && completion->request_result == ZLINK_REQUEST_OK) {
        napi_value parts;
        napi_create_array_with_length (
          env, completion->reply_part_count, &parts);
        for (size_t index = 0; index < completion->reply_part_count; ++index) {
            napi_value part = create_received_message_buffer (
              env, &completion->reply_parts[index]);
            if (!part)
                return NULL;
            napi_set_element (
              env, parts, static_cast<uint32_t> (index), part);
        }
        napi_set_named_property (env, out, "parts", parts);
    }
    return out;
}

napi_value socket_completion_recv (napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    if (argc < 2) {
        napi_throw_type_error (
          env, NULL, "socketCompletionRecv requires (socket, flags)");
        return NULL;
    }
    void *socket = NULL;
    napi_get_value_external (env, argv[0], &socket);
    int32_t flags = 0;
    napi_get_value_int32 (env, argv[1], &flags);
    zlink_completion_t completion;
    memset (&completion, 0, sizeof (completion));
    completion.struct_size = sizeof (completion);
    const int result = zlink_completion_recv (
      socket, &completion, static_cast<zlink_recv_flags_t> (flags));
    if (result == ZLINK_RECV_NO_DATA) {
        napi_value null_value;
        napi_get_null (env, &null_value);
        return null_value;
    }
    if (result != ZLINK_RECV_OK)
        return throw_last_error (env, "completion recv failed");
    return create_completion_value (env, &completion);
}

napi_value test_completion_close_count (napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    bool reset = false;
    if (argc > 0)
        napi_get_value_bool (env, argv[0], &reset);
    const uint64_t value = reset
      ? g_completion_close_count.exchange (0, std::memory_order_relaxed)
      : g_completion_close_count.load (std::memory_order_relaxed);
    napi_value out;
    napi_create_bigint_uint64 (env, value, &out);
    return out;
}

napi_value socket_request_sync (napi_env env, napi_callback_info info)
{
    napi_value argv[4];
    size_t argc = 4;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    if (argc < 4) {
        napi_throw_type_error (
          env, NULL,
          "socketRequestSync requires (socket, targetOrNull, parts, timeoutMs)");
        return NULL;
    }
    void *socket = NULL;
    napi_get_value_external (env, argv[0], &socket);
    zlink_routing_id_t target_storage;
    const zlink_routing_id_t *target = NULL;
    if (!parse_optional_routing_id (env, argv[1], &target_storage, &target))
        return NULL;
    int32_t timeout_ms = 0;
    if (napi_get_value_int32 (env, argv[3], &timeout_ms) != napi_ok
        || timeout_ms <= 0) {
        napi_throw_range_error (env, NULL, "request timeout must be positive");
        return NULL;
    }
    small_msg_storage_t parts;
    if (!build_msg_vector_or_single (env, argv[2], &parts))
        return NULL;
    zlink_completion_id_t completion_id = 0;
    const int result = submit_request_parts_016 (
      socket, target, parts.data (), parts.size (),
      ZLINK_SEND_FLAGS_NONE, static_cast<uint32_t> (timeout_ms),
      NULL, &completion_id);
    const int native_errno = result == ZLINK_SUBMIT_OK ? 0 : zlink_errno ();
    parts.release ();
    if (result == ZLINK_SUBMIT_OK)
        consume_native_message_value (env, argv[2]);

    napi_value out = create_completion_submit_result (
      env, result, native_errno, completion_id);
    napi_value completions;
    napi_create_array (env, &completions);
    napi_set_named_property (env, out, "completions", completions);
    if (result != ZLINK_SUBMIT_OK)
        return out;

    uint32_t index = 0;
    bool found = false;
    while (!found) {
        zlink_completion_t completion;
        memset (&completion, 0, sizeof (completion));
        completion.struct_size = sizeof (completion);
        const int recv_result = zlink_completion_recv (
          socket, &completion, ZLINK_RECV_FLAGS_NONE);
        if (recv_result != ZLINK_RECV_OK)
            return throw_last_error (env, "request completion recv failed");
        found = completion.completion_id == completion_id;
        napi_value value = create_completion_value (env, &completion);
        if (!value)
            return NULL;
        napi_set_element (env, completions, index++, value);
    }
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

napi_value socket_stream_recv_packet (napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    if (argc < 2) {
        napi_throw_type_error (
          env, NULL, "socketStreamRecvPacket requires (socket, flags)");
        return NULL;
    }
    void *socket = NULL;
    napi_get_value_external (env, argv[0], &socket);
    int32_t flags = 0;
    napi_get_value_int32 (env, argv[1], &flags);
    const zlink_routing_id_t *source_rid = NULL;
    zlink_msg_t header;
    zlink_msg_t body;
    if (zlink_msg_init (&header) != 0 || zlink_msg_init (&body) != 0) {
        zlink_msg_close (&header);
        return throw_last_error (env, "stream packet init failed");
    }
    const int result = zlink_stream_recv_packet (
      socket, &source_rid, &header, &body,
      static_cast<zlink_recv_flags_t> (flags));
    if (result == ZLINK_RECV_NO_DATA) {
        zlink_msg_close (&header);
        zlink_msg_close (&body);
        napi_value null_value;
        napi_get_null (env, &null_value);
        return null_value;
    }
    if (result != ZLINK_RECV_OK) {
        zlink_msg_close (&header);
        zlink_msg_close (&body);
        return throw_last_error (env, "stream packet recv failed");
    }
    napi_value out;
    napi_create_object (env, &out);
    napi_value rid = create_routing_id_value (env, *source_rid);
    napi_value header_value = create_received_message_buffer (env, &header);
    napi_value body_value = create_received_message_buffer (env, &body);
    zlink_msg_close (&header);
    zlink_msg_close (&body);
    if (!rid || !header_value || !body_value)
        return NULL;
    napi_set_named_property (env, out, "routingId", rid);
    napi_set_named_property (env, out, "header", header_value);
    napi_set_named_property (env, out, "body", body_value);
    return out;
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

napi_value socket_reply (napi_env env, napi_callback_info info)
{
    napi_value argv[4];
    size_t argc = 4;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    if (argc < 4) {
        napi_throw_type_error (
          env, NULL, "socketReply requires (socket, sourceRid, token, parts)");
        return NULL;
    }
    void *router = NULL;
    napi_get_value_external (env, argv[0], &router);
    zlink_routing_id_t source_rid;
    if (!parse_routing_id_value (env, argv[1], &source_rid))
        return NULL;
    uint64_t reply_token = 0;
    if (!get_uint64_like (env, argv[2], &reply_token)
        || reply_token == 0) {
        napi_throw_type_error (env, NULL, "reply token must be nonzero uint64");
        return NULL;
    }
    small_msg_storage_t parts;
    if (!build_msg_vector_or_single (env, argv[3], &parts))
        return NULL;
    const int result = reply_parts_016 (
      router, &source_rid, reply_token, parts.data (), parts.size ());
    parts.release ();
    if (result != ZLINK_SUBMIT_OK)
        return throw_submit_error (env, "reply failed", result);
    consume_native_message_value (env, argv[3]);
    napi_value out;
    napi_get_undefined (env, &out);
    return out;
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
