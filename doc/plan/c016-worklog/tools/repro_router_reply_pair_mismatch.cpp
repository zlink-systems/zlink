#include <zlink.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace {

zlink_routing_id_t rid(const char *text)
{
    zlink_routing_id_t value{};
    value.size = static_cast<uint8_t>(std::strlen(text));
    std::memcpy(value.data, text, value.size);
    return value;
}

void configure(void *socket, const char *routing_id, const char *endpoint)
{
    const int one = 1;
    const int zero = 0;
    const int handover = ZLINK_RID_DUPLICATE_HANDOVER;
    const int64_t unlimited = -1;
    const uint64_t hwm = 4096000;
    zlink_set_router_option(
      socket, ZLINK_ROUTER_OPT_MANDATORY, &one, sizeof one);
    zlink_set_option(socket, ZLINK_OPT_RID_DUPLICATE_POLICY, &handover,
                     sizeof handover);
    zlink_set_option(socket, ZLINK_OPT_LINGER, &zero, sizeof zero);
    zlink_set_option(socket, ZLINK_OPT_MAXMSGSIZE, &unlimited,
                     sizeof unlimited);
    zlink_set_option(socket, ZLINK_OPT_SNDHWM, &hwm, sizeof hwm);
    zlink_set_option(socket, ZLINK_OPT_RCVHWM, &hwm, sizeof hwm);
    if (zlink_set_routing_id(socket, routing_id,
                             std::strlen(routing_id)) != ZLINK_CONFIG_OK
        || zlink_bind(socket, endpoint) != ZLINK_BIND_OK)
        std::abort();
}

void connect(void *socket, const char *peer_rid, const char *endpoint)
{
    if (zlink_set_router_option(socket, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
                                peer_rid, std::strlen(peer_rid))
          != ZLINK_CONFIG_OK
        || zlink_connect(socket, endpoint) != ZLINK_CONNECT_OK)
        std::abort();
}

void *make_poller(void *socket)
{
    void *poller = zlink_poller_new();
    const short events = ZLINK_POLLIN | ZLINK_POLLERR | ZLINK_POLLOUT
                         | ZLINK_POLLCOMPLETION;
    if (!poller
        || zlink_poller_add(poller, socket, reinterpret_cast<void *>(1),
                            events) != ZLINK_CONFIG_OK)
        std::abort();
    return poller;
}

bool send_one(void *socket, const char *peer, const char *text)
{
    auto target = rid(peer);
    zlink_msg_t message;
    zlink_msg_init_size(&message, std::strlen(text));
    std::memcpy(zlink_msg_data(&message), text, std::strlen(text));
    int context = 7;
    zlink_completion_id_t completion = 0;
    const auto result = zlink_send_part_rid(
      socket, &target, &message, ZLINK_SEND_FLAGS_DONTWAIT,
      ZLINK_PART_FINAL, &context, &completion);
    zlink_msg_close(&message);
    return result == ZLINK_SUBMIT_OK && completion == 0;
}

bool receive_one(void *poller, void *socket, std::string *payload = nullptr)
{
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        zlink_poller_event_t event{};
        zlink_config_result_t error = ZLINK_CONFIG_OK;
        const int ready = zlink_poller_wait(poller, &event, 1, 50, &error);
        if (ready <= 0 || !(event.events & ZLINK_POLLIN))
            continue;
        zlink_msg_t part;
        zlink_msg_init(&part);
        const zlink_routing_id_t *source = nullptr;
        zlink_reply_token_t token = 0;
        zlink_part_flag_t more = ZLINK_PART_MORE;
        const auto result = zlink_router_recv_part(
          socket, &source, &token, &part, &more, ZLINK_RECV_FLAGS_DONTWAIT);
        if (result == ZLINK_RECV_OK && more == ZLINK_PART_FINAL) {
            if (payload)
                payload->assign(static_cast<const char *>(zlink_msg_data(&part)),
                                zlink_msg_size(&part));
            zlink_msg_close(&part);
            return true;
        }
        zlink_msg_close(&part);
    }
    return false;
}

} // namespace

int main()
{
    void *context = zlink_ctx_new();
    void *caller = zlink_socket(context, ZLINK_SOCKET_ROUTER);
    configure(caller, "stale-authority-caller", "inproc://fw-caller");
    void *caller_poller = make_poller(caller);
    // Framework starts the caller first: its connect is pending before the
    // owner's native socket has even been created and bound.
    connect(caller, "stale-authority-owner", "inproc://fw-owner");

    void *owner = zlink_socket(context, ZLINK_SOCKET_ROUTER);
    configure(owner, "stale-authority-owner", "inproc://fw-owner");
    void *owner_poller = make_poller(owner);
    connect(owner, "stale-authority-caller", "inproc://fw-caller");

    // The observed Framework startup boundary: simultaneous Hello in both
    // directions, then the owner's Admit response back to the caller.
    bool caller_hello = false;
    bool owner_hello = false;
    for (int attempt = 0; attempt < 300
         && (!caller_hello || !owner_hello); ++attempt) {
        if (!caller_hello)
            caller_hello = send_one(
              caller, "stale-authority-owner", "caller-hello");
        if (!owner_hello)
            owner_hello = send_one(
              owner, "stale-authority-caller", "owner-hello");
        if (!caller_hello || !owner_hello)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!caller_hello || !owner_hello || !receive_one(owner_poller, owner)
        || !receive_one(caller_poller, caller)
        || !send_one(owner, "stale-authority-caller", "owner-admit")
        || !receive_one(caller_poller, caller)) {
        std::printf("SETUP_FAIL errno=%d\n", zlink_errno());
        return 2;
    }

    auto target = rid("stale-authority-owner");
    zlink_msg_t head;
    zlink_msg_init_size(&head, 4);
    std::memcpy(zlink_msg_data(&head), "head", 4);
    auto submit = zlink_request_part(
      caller, &target, &head, ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_MORE,
      0, nullptr, nullptr);
    zlink_msg_close(&head);
    if (submit != ZLINK_SUBMIT_OK) {
        std::printf("REQUEST_MORE_FAIL result=%d errno=%d\n", submit,
                    zlink_errno());
        return 3;
    }

    zlink_msg_t body;
    zlink_msg_init_size(&body, 4);
    std::memcpy(zlink_msg_data(&body), "body", 4);
    int request_context = 0x5a17;
    zlink_completion_id_t completion_id = 0;
    submit = zlink_request_part(
      caller, &target, &body, ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL,
      3000, &request_context, &completion_id);
    zlink_msg_close(&body);
    std::printf("request result=%d id=%llu context=%p\n", submit,
                static_cast<unsigned long long>(completion_id),
                &request_context);
    if (submit != ZLINK_SUBMIT_OK)
        return 4;

    zlink_routing_id_t source_copy{};
    zlink_reply_token_t reply_token = 0;
    int part_count = 0;
    const auto receive_deadline = std::chrono::steady_clock::now()
                                  + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < receive_deadline
           && part_count < 2) {
        zlink_poller_event_t event{};
        zlink_config_result_t error = ZLINK_CONFIG_OK;
        if (zlink_poller_wait(owner_poller, &event, 1, 50, &error) <= 0
            || !(event.events & ZLINK_POLLIN))
            continue;
        zlink_msg_t part;
        zlink_msg_init(&part);
        const zlink_routing_id_t *source = nullptr;
        zlink_part_flag_t more = ZLINK_PART_MORE;
        const auto result = zlink_router_recv_part(
          owner, &source, &reply_token, &part, &more,
          ZLINK_RECV_FLAGS_DONTWAIT);
        if (result == ZLINK_RECV_OK) {
            source_copy = *source;
            ++part_count;
        }
        zlink_msg_close(&part);
    }
    if (part_count != 2 || reply_token == 0) {
        std::printf("REQUEST_RECV_FAIL parts=%d token=%llu\n", part_count,
                    static_cast<unsigned long long>(reply_token));
        return 5;
    }

    std::thread reply_thread([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        zlink_msg_t reply;
        zlink_msg_init_size(&reply, 5);
        std::memcpy(zlink_msg_data(&reply), "reply", 5);
        const auto result = zlink_reply_part(
          owner, &source_copy, reply_token, &reply, ZLINK_PART_FINAL);
        zlink_msg_close(&reply);
        std::printf("reply result=%d errno=%d\n", result, zlink_errno());
    });

    zlink_completion_t completion{};
    completion.struct_size = sizeof completion;
    const auto completion_deadline = std::chrono::steady_clock::now()
                                     + std::chrono::seconds(4);
    bool got_completion = false;
    while (std::chrono::steady_clock::now() < completion_deadline) {
        zlink_poller_event_t event{};
        zlink_config_result_t error = ZLINK_CONFIG_OK;
        if (zlink_poller_wait(caller_poller, &event, 1, 50, &error) <= 0)
            continue;
        if (!(event.events & (ZLINK_POLLCOMPLETION | ZLINK_POLLOUT
                             | ZLINK_POLLERR)))
            continue;
        const auto result = zlink_completion_recv(
          caller, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
        if (result == ZLINK_RECV_OK) {
            got_completion = true;
            break;
        }
    }
    reply_thread.join();
    if (!got_completion) {
        std::printf("NO_COMPLETION\n");
        return 6;
    }
    std::printf("completion kind=%d id=%llu context_match=%d result=%d parts=%zu\n",
                completion.kind,
                static_cast<unsigned long long>(completion.completion_id),
                completion.user_context == &request_context,
                completion.request_result, completion.reply_part_count);
    const bool passed = completion.kind == ZLINK_COMPLETION_REQUEST
                        && completion.request_result == ZLINK_REQUEST_OK
                        && completion.reply_part_count == 1;
    zlink_completion_close(&completion);

    zlink_poller_destroy(&caller_poller);
    zlink_poller_destroy(&owner_poller);
    zlink_close(caller);
    zlink_close(owner);
    zlink_ctx_term(context);
    return passed ? 0 : 7;
}
