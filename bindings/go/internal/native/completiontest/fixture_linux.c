// SPDX-License-Identifier: MPL-2.0

#include "zlink.h"
#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <string.h>

static _Atomic(void *) fixture_socket;
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
}

void fixture_stop(void) { atomic_store(&fixture_socket, NULL); }
const char *fixture_trace(void) { return trace; }

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

static zlink_submit_result_t admit(zlink_msg_t *part, zlink_send_flags_t flags,
                                  zlink_part_flag_t part_flag, void *context,
                                  zlink_completion_id_t *id, int request)
{
    assert(flags == ZLINK_SEND_FLAGS_DONTWAIT);
    zlink_msg_close(part);
    if (id)
        *id = 0;
    if (part_flag == ZLINK_PART_FINAL) {
        record_call(request ? 'R' : 'S');
        if (request) {
            assert(id != NULL);
            *id = 900;
            fixture_request(*id, (uintptr_t) context);
        }
    }
    return ZLINK_SUBMIT_OK;
}

zlink_submit_result_t __real_zlink_send_part(void *, zlink_msg_t *, zlink_send_flags_t, zlink_part_flag_t, void *, zlink_completion_id_t *);
zlink_submit_result_t __wrap_zlink_send_part(void *socket, zlink_msg_t *part, zlink_send_flags_t flags, zlink_part_flag_t part_flag, void *context, zlink_completion_id_t *id)
{
    if (!owns_socket(socket))
        return __real_zlink_send_part(socket, part, flags, part_flag, context, id);
    return admit(part, flags, part_flag, context, id, 0);
}

zlink_submit_result_t __real_zlink_send_part_rid(void *, const zlink_routing_id_t *, zlink_msg_t *, zlink_send_flags_t, zlink_part_flag_t, void *, zlink_completion_id_t *);
zlink_submit_result_t __wrap_zlink_send_part_rid(void *socket, const zlink_routing_id_t *rid, zlink_msg_t *part, zlink_send_flags_t flags, zlink_part_flag_t part_flag, void *context, zlink_completion_id_t *id)
{
    if (!owns_socket(socket))
        return __real_zlink_send_part_rid(socket, rid, part, flags, part_flag, context, id);
    return admit(part, flags, part_flag, context, id, 0);
}

zlink_submit_result_t __real_zlink_request_part(void *, const zlink_routing_id_t *, zlink_msg_t *, zlink_send_flags_t, zlink_part_flag_t, uint32_t, void *, zlink_completion_id_t *);
zlink_submit_result_t __wrap_zlink_request_part(void *socket, const zlink_routing_id_t *rid, zlink_msg_t *part, zlink_send_flags_t flags, zlink_part_flag_t part_flag, uint32_t timeout, void *context, zlink_completion_id_t *id)
{
    if (!owns_socket(socket))
        return __real_zlink_request_part(socket, rid, part, flags, part_flag, timeout, context, id);
    return admit(part, flags, part_flag, context, id, 1);
}
