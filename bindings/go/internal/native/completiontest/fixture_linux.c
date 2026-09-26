// SPDX-License-Identifier: MPL-2.0

#include "zlink.h"
#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <string.h>

static _Atomic(void *) fixture_socket;
static _Atomic(void *) watched_context;
static _Atomic int override_empty_poller_wait;
static _Atomic int override_missing_poller_modify;
static _Atomic int override_poll_failure;
static zlink_completion_t records[16];
static size_t read_index, write_index, trace_size;
static char trace[64];

static int owns_socket(void *socket)
{
    return socket != NULL && socket == atomic_load(&fixture_socket);
}

static void record_call(char call)
{
    assert(trace_size + 1 < sizeof(trace));
    trace[trace_size++] = call;
    trace[trace_size] = '\0';
}

void fixture_start(void *socket)
{
    assert(atomic_load(&fixture_socket) == NULL);
    read_index = write_index = trace_size = 0;
    trace[0] = '\0';
    atomic_store(&fixture_socket, socket);
    atomic_store(&override_empty_poller_wait, 0);
    atomic_store(&override_missing_poller_modify, 0);
    atomic_store(&override_poll_failure, 0);
}

void fixture_stop(void) {
    atomic_store(&fixture_socket, NULL);
    atomic_store(&watched_context, NULL);
    atomic_store(&override_empty_poller_wait, 0);
    atomic_store(&override_missing_poller_modify, 0);
    atomic_store(&override_poll_failure, 0);
}
void fixture_watch_context(void *context) { atomic_store(&watched_context, context); }
void fixture_override_empty_poller_wait_once(void) { atomic_store(&override_empty_poller_wait, 1); }
void fixture_override_missing_poller_modify_once(void) { atomic_store(&override_missing_poller_modify, 1); }
void fixture_override_poll_failure_once(void) { atomic_store(&override_poll_failure, 1); }
const char *fixture_trace(void) { return trace; }

int __real_zlink_poll(zlink_pollitem_t *, int, long, zlink_config_result_t *);
int __wrap_zlink_poll(zlink_pollitem_t *items, int count, long timeout, zlink_config_result_t *error_out)
{
    if (atomic_exchange(&override_poll_failure, 0)) {
        if (error_out != NULL)
            *error_out = ZLINK_CONFIG_OK;
        errno = EBUSY;
        return -1;
    }
    return __real_zlink_poll(items, count, timeout, error_out);
}

int __real_zlink_poller_wait(void *, zlink_poller_event_t *, int, long, zlink_config_result_t *);
int __wrap_zlink_poller_wait(void *poller, zlink_poller_event_t *events, int capacity, long timeout,
                             zlink_config_result_t *error_out)
{
    if (capacity == 0 && atomic_exchange(&override_empty_poller_wait, 0)) {
        if (error_out != NULL)
            *error_out = ZLINK_CONFIG_BUSY;
        errno = EBUSY;
        return -1;
    }
    return __real_zlink_poller_wait(poller, events, capacity, timeout, error_out);
}

zlink_config_result_t __real_zlink_poller_modify(void *, void *, short);
zlink_config_result_t __wrap_zlink_poller_modify(void *poller, void *source, short events)
{
    if (atomic_exchange(&override_missing_poller_modify, 0)) {
        errno = EBUSY;
        return ZLINK_CONFIG_BUSY;
    }
    return __real_zlink_poller_modify(poller, source, events);
}

int __real_zlink_ctx_shutdown(void *);
int __wrap_zlink_ctx_shutdown(void *context)
{
    const int rc = __real_zlink_ctx_shutdown(context);
    if (context != NULL && context == atomic_load(&watched_context) && rc == ZLINK_CLOSE_OK)
        record_call('S');
    return rc;
}

int __real_zlink_ctx_term(void *);
int __wrap_zlink_ctx_term(void *context)
{
    const int rc = __real_zlink_ctx_term(context);
    if (context != NULL && context == atomic_load(&watched_context) && rc == ZLINK_CLOSE_OK)
        record_call('T');
    return rc;
}

static zlink_completion_t *append_record(uint64_t id, uintptr_t context)
{
    assert(write_index < sizeof(records) / sizeof(records[0]));
    zlink_completion_t *record = &records[write_index++];
    memset(record, 0, sizeof(*record));
    record->struct_size = sizeof(*record);
    record->completion_id = id;
    record->user_context = (void *) context;
    return record;
}

void fixture_writable(uint64_t id, uintptr_t context, const char *rid)
{
    zlink_completion_t *record = append_record(id, context);
    record->kind = ZLINK_COMPLETION_WRITABLE;
    record->send_result = ZLINK_SEND_ADMITTED;
    size_t size = strlen(rid);
    assert(size <= sizeof(record->peer_rid.data));
    record->peer_rid.size = (uint8_t) size;
    memcpy(record->peer_rid.data, rid, size);
}

void fixture_request(uint64_t id, uintptr_t context)
{
    zlink_completion_t *record = append_record(id, context);
    record->kind = ZLINK_COMPLETION_REQUEST;
    record->request_result = ZLINK_REQUEST_OK;
}

zlink_recv_result_t __real_zlink_completion_recv(void *, zlink_completion_t *, zlink_recv_flags_t);
zlink_recv_result_t __wrap_zlink_completion_recv(void *socket, zlink_completion_t *out, zlink_recv_flags_t flags)
{
    if (!owns_socket(socket))
        return __real_zlink_completion_recv(socket, out, flags);
    assert(flags == ZLINK_RECV_FLAGS_DONTWAIT);
    if (read_index == write_index) {
        record_call('N');
        errno = EAGAIN;
        return ZLINK_RECV_NO_DATA;
    }
    *out = records[read_index++];
    record_call(out->kind == ZLINK_COMPLETION_WRITABLE ? 'W' : 'Q');
    return ZLINK_RECV_OK;
}

static zlink_submit_result_t admit(zlink_msg_t *parts, size_t part_count,
                                  zlink_send_flags_t flags, void *context,
                                  zlink_completion_id_t *id, int request)
{
    assert(flags == ZLINK_SEND_FLAGS_DONTWAIT);
    assert(part_count > 0);
    zlink_multipart_close(parts, part_count);
    if (id)
        *id = 0;
    record_call(request ? 'R' : 'S');
    if (request) {
        assert(id != NULL);
        *id = 900;
        fixture_request(*id, (uintptr_t) context);
    }
    return ZLINK_SUBMIT_OK;
}

zlink_submit_result_t __real_zlink_send(void *, zlink_msg_t *, size_t, zlink_send_flags_t, void *, zlink_completion_id_t *);
zlink_submit_result_t __wrap_zlink_send(void *socket, zlink_msg_t *parts, size_t part_count, zlink_send_flags_t flags, void *context, zlink_completion_id_t *id)
{
    if (!owns_socket(socket))
        return __real_zlink_send(socket, parts, part_count, flags, context, id);
    return admit(parts, part_count, flags, context, id, 0);
}

zlink_submit_result_t __real_zlink_send_rid(void *, const zlink_routing_id_t *, zlink_msg_t *, size_t, zlink_send_flags_t, void *, zlink_completion_id_t *);
zlink_submit_result_t __wrap_zlink_send_rid(void *socket, const zlink_routing_id_t *rid, zlink_msg_t *parts, size_t part_count, zlink_send_flags_t flags, void *context, zlink_completion_id_t *id)
{
    if (!owns_socket(socket))
        return __real_zlink_send_rid(socket, rid, parts, part_count, flags, context, id);
    return admit(parts, part_count, flags, context, id, 0);
}

zlink_submit_result_t __real_zlink_request(void *, const zlink_routing_id_t *, zlink_msg_t *, size_t, zlink_send_flags_t, uint32_t, void *, zlink_completion_id_t *);
zlink_submit_result_t __wrap_zlink_request(void *socket, const zlink_routing_id_t *rid, zlink_msg_t *parts, size_t part_count, zlink_send_flags_t flags, uint32_t timeout, void *context, zlink_completion_id_t *id)
{
    if (!owns_socket(socket))
        return __real_zlink_request(socket, rid, parts, part_count, flags, timeout, context, id);
    return admit(parts, part_count, flags, context, id, 1);
}
