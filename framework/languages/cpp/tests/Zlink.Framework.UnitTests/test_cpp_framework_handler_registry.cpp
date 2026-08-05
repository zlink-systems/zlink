/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework.hpp>

#include <chrono>
#include <coroutine>
#include <stdexcept>
#include <string>
#include <thread>

namespace
{

struct request_t
{
    int value{};
};

struct reply_t
{
    int value{};
};

struct command_t
{
    static constexpr const char *packet_name = "Command";
    int value{};
};

struct event_t
{
    int value{};
};

struct async_request_t
{
    int value{};
};

struct delayed_request_t
{
    int value{};
};

class handler_t
{
  public:
    reply_t get_reply (const request_t &request)
    {
        last_thread = std::this_thread::get_id ();
        last_request = request.value;
        return {request.value + 1};
    }

    reply_t get_context_reply (const request_t &request,
                               const zlink::framework::message_context_t &context)
    {
        last_thread = std::this_thread::get_id ();
        last_request = request.value;
        capture_context (context);
        return {request.value + 2};
    }

    void on_command (const command_t &command)
    {
        last_thread = std::this_thread::get_id ();
        last_command = command.value;
    }

    void on_context_command (const command_t &command,
                             const zlink::framework::message_context_t &context)
    {
        last_thread = std::this_thread::get_id ();
        last_command = command.value;
        capture_context (context);
    }

    zlink::framework::task_t<void> on_event (const event_t &event)
    {
        last_thread = std::this_thread::get_id ();
        last_event = event.value;
        co_return;
    }

    zlink::framework::task_t<void>
    on_context_event (const event_t &event,
                      const zlink::framework::publish_message_context_t &context)
    {
        last_thread = std::this_thread::get_id ();
        last_event = event.value;
        capture_context (context);
        last_context_topic = context.topic;
        last_context_source = context.source.value_or ("<none>");
        co_return;
    }

    zlink::framework::task_t<reply_t> get_async_reply (const async_request_t &request)
    {
        last_thread = std::this_thread::get_id ();
        last_request = request.value;
        co_return reply_t{request.value + 10};
    }

    zlink::framework::task_t<reply_t> get_delayed_reply (const delayed_request_t &request)
    {
        last_thread = std::this_thread::get_id ();
        last_request = request.value;
        struct delay_awaiter_t
        {
            int value;
            bool await_ready () const noexcept { return false; }
            void await_suspend (std::coroutine_handle<> continuation) const
            {
                std::thread ([continuation] {
                    std::this_thread::sleep_for (std::chrono::milliseconds (5));
                    continuation.resume ();
                }).detach ();
            }
            reply_t await_resume () const noexcept { return {value + 20}; }
        };
        co_return co_await delay_awaiter_t{request.value};
    }

    reply_t throw_reply (const request_t &)
    {
        last_thread = std::this_thread::get_id ();
        throw std::runtime_error ("boom");
    }

    void capture_context (const zlink::framework::message_context_t &context)
    {
        last_context_mesh = context.mesh_name.value_or ("<none>");
        last_context_channel = context.channel_name.value_or ("<none>");
        last_context_packet = context.packet_name;
        last_context_content_type = context.content_type.value_or ("<none>");
        last_context_correlation = context.correlation_id.value_or ("<none>");
        last_context_trace = std::string (context.metadata.find ("trace-id").value_or ("<none>"));
        last_context_metadata_size = context.metadata.values ().size ();
    }

    int last_request = 0;
    int last_command = 0;
    int last_event = 0;
    std::string last_context_mesh;
    std::string last_context_channel;
    std::string last_context_packet;
    std::string last_context_content_type;
    std::string last_context_correlation;
    std::string last_context_trace;
    std::size_t last_context_metadata_size = 0;
    std::string last_context_topic;
    std::string last_context_source;
    std::thread::id last_thread;
};


struct spot_actor_t
{
    int id = 0;
};

/// Exercises the Spot dispatch side of the unified MessageContext: the Spot packet member and the
/// Spot Actor member both take the universal context, and the Spot subscription member takes the
/// publish context.
struct dispatch_spot_t : public zlink::framework::spot_t<spot_actor_t>
{
    zlink::framework::spot_context_t &context () noexcept override
    {
        std::terminate ();
    }

    const zlink::framework::spot_context_t &context () const noexcept override
    {
        std::terminate ();
    }

    void configure () override {}

    zlink::framework::task_t<zlink::framework::spot_actor_join_result_t>
    on_actor_join (std::string_view,
                   const zlink::framework::message_t &) override
    {
        co_return zlink::framework::spot_actor_join_result_t::accept ();
    }

    zlink::framework::task_t<void>
    on_actor_joined (spot_actor_t &) override
    {
        co_return;
    }

    zlink::framework::task_t<void>
    on_leave_actor (spot_actor_t &) override
    {
        co_return;
    }

    void on_packet (const zlink::framework::message_context_t &context, const request_t &request)
    {
        last_payload = request.value;
        capture (context);
    }

    void on_actor_packet (spot_actor_t &actor,
                          const zlink::framework::message_context_t &context,
                          const request_t &request)
    {
        last_actor = actor.id;
        last_payload = request.value;
        capture (context);
    }

    void on_topic_event (const zlink::framework::publish_message_context_t &context,
                         const event_t &event)
    {
        last_payload = event.value;
        capture (context);
        last_topic = context.topic;
        last_source = context.source.value_or ("<none>");
    }

    void capture (const zlink::framework::message_context_t &context)
    {
        last_mesh = context.mesh_name.value_or ("<none>");
        last_channel = context.channel_name.value_or ("<none>");
        last_packet = context.packet_name;
        last_content_type = context.content_type.value_or ("<none>");
        last_correlation = context.correlation_id.value_or ("<none>");
        last_trace = std::string (context.metadata.find ("trace-id").value_or ("<none>"));
    }

    int last_actor = 0;
    int last_payload = 0;
    std::string last_mesh;
    std::string last_channel;
    std::string last_packet;
    std::string last_content_type;
    std::string last_correlation;
    std::string last_trace;
    std::string last_topic;
    std::string last_source;
};

class auditing_filter_t
{
  public:
    zlink::framework::task_t<void>
    invoke (const zlink::framework::handler_filter_context_t &context,
            zlink::framework::handler_next_t next)
    {
        ++before_count;
        last_packet_name = context.packet_name;
        last_context_channel = context.channel_name.value_or ("<none>");
        last_context_packet = context.packet_name;
        last_dispatch_kind = context.dispatch_kind;
        co_await next ();
        ++after_count;
        co_return;
    }

    int before_count = 0;
    int after_count = 0;
    std::string last_packet_name;
    std::string last_context_channel;
    std::string last_context_packet;
    zlink::framework::handler_dispatch_kind_t last_dispatch_kind =
      zlink::framework::handler_dispatch_kind_t::channel_send;
};

class short_circuit_filter_t
{
  public:
    zlink::framework::task_t<void>
    invoke (const zlink::framework::handler_filter_context_t &context,
            zlink::framework::handler_next_t next)
    {
        if (context.packet_name == "blocked"
            || context.packet_name == "blocked-send"
            || context.packet_name == "blocked-event") {
            ++short_circuit_count;
            co_return;
        }
        co_await next ();
        co_return;
    }

    int short_circuit_count = 0;
};

class duplicate_next_filter_t
{
  public:
    zlink::framework::task_t<void>
    invoke (const zlink::framework::handler_filter_context_t &context,
            zlink::framework::handler_next_t next)
    {
        if (context.packet_name != "duplicate") {
            co_await next ();
            co_return;
        }
        co_await next ();
        try {
            co_await next ();
        }
        catch (const zlink::framework::framework_exception_t &) {
            ++duplicate_rejections;
        }
        co_return;
    }

    int duplicate_rejections = 0;
};

template <typename T> void add_int_serializer (zlink::framework::serializer_registry_t &serializers)
{
    serializers.add<T> (
      [] (const T &value) {
          return zlink::framework::encoded_payload_t::from_string (std::to_string (value.value));
      },
      [] (const zlink::framework::encoded_payload_t &payload) {
          return T{std::stoi (payload.to_string ())};
      });
}

zlink::framework::task_t<reply_t> delayed_reply_task (int value)
{
    struct delay_awaiter_t
    {
        int value;
        bool await_ready () const noexcept { return false; }
        void await_suspend (std::coroutine_handle<> continuation) const
        {
            std::thread ([continuation] {
                std::this_thread::sleep_for (std::chrono::milliseconds (5));
                continuation.resume ();
            }).detach ();
        }
        reply_t await_resume () const noexcept { return {value}; }
    };
    co_return co_await delay_awaiter_t{value};
}

zlink::framework::task_t<int> await_shared_reply (zlink::framework::task_t<reply_t> &task,
                                                  int offset)
{
    auto reply = co_await task;
    co_return reply.value + offset;
}

zlink::framework::task_t<int> timeout_task ()
{
    co_return zlink::framework::detail::boundary_failure<int> (zlink::framework::detail::boundary_error_t::timed_out, "timeout preserved");
}

zlink::framework::task_t<int> await_timeout_task ()
{
    auto task = timeout_task ();
    co_return co_await task;
}

} // namespace

int main ()
{
    zlink::framework::service_collection_t services;
    services.add_singleton<handler_t> ();
    services.add_singleton<auditing_filter_t> ();
    services.add_singleton<short_circuit_filter_t> ();
    services.add_singleton<duplicate_next_filter_t> ();
    auto provider = services.build_provider ();

    zlink::framework::serializer_registry_t serializers;
    add_int_serializer<request_t> (serializers);
    add_int_serializer<reply_t> (serializers);
    add_int_serializer<command_t> (serializers);
    add_int_serializer<event_t> (serializers);
    add_int_serializer<async_request_t> (serializers);
    add_int_serializer<delayed_request_t> (serializers);

    zlink::framework::handler_registry_t handlers;
    int failure_events = 0;
    zlink::framework::framework_error_kind_t last_failure_kind =
      zlink::framework::framework_error_kind_t::internal_failure;
    handlers.observe_failures ([&failure_events, &last_failure_kind] (
                                 const zlink::framework::handler_failure_event_t &event) {
        ++failure_events;
        last_failure_kind = event.error_kind;
    });
    handlers.on_request<handler_t, request_t, reply_t> ("game", "move", &handler_t::get_reply,
                                                        {.packet_name = "request"});
    handlers.on_request<handler_t, request_t, reply_t> (
      "game", "context-move", &handler_t::get_context_reply, {.packet_name = "context-request"});
    handlers.on_send<handler_t, command_t> ("game", "command", &handler_t::on_command,
                                            {.packet_name = "command"});
    handlers.on_send<handler_t, command_t> ("game", "context-command",
                                            &handler_t::on_context_command,
                                            {.packet_name = "context-command"});
    handlers.on_event<handler_t, event_t> (
      "game", "event", &handler_t::on_event,
      {.packet_name = "event", .execution = zlink::framework::handler_execution_t::offload});
    handlers.on_event<handler_t, event_t> ("game", "context-event", &handler_t::on_context_event,
                                           {.packet_name = "context-event"});
    handlers.on_request<handler_t, async_request_t, reply_t> (
      "game", "async", &handler_t::get_async_reply, {.packet_name = "async"});
    handlers.on_request<handler_t, delayed_request_t, reply_t> (
      "game", "delayed", &handler_t::get_delayed_reply, {.packet_name = "delayed"});
    handlers.on_request<handler_t, request_t, reply_t> ("game", "throw", &handler_t::throw_reply,
                                                        {.packet_name = "throw"});
    handlers.on_request<handler_t, request_t, reply_t> ("game", "blocked", &handler_t::get_reply,
                                                        {.packet_name = "blocked"});
    handlers.on_request<handler_t, request_t, reply_t> (
      "game", "duplicate", &handler_t::get_reply, {.packet_name = "duplicate"});
    handlers.on_send<handler_t, command_t> (
      "game", "blocked-send", &handler_t::on_command,
      {.packet_name = "blocked-send"});
    handlers.on_event<handler_t, event_t> (
      "game", "blocked-event", &handler_t::on_event,
      {.packet_name = "blocked-event"});
    handlers.use_filter<auditing_filter_t> ()
      .use_filter<short_circuit_filter_t> ()
      .use_filter<duplicate_next_filter_t> ();

    const auto *descriptor = handlers.find ("game", "move", "request");
    if (descriptor == nullptr || descriptor->topic != "move") {
        return 1;
    }
    if (handlers.find ("game", "other-topic", "request") != nullptr) {
        return 2;
    }

    auto request_result = handlers.invoke ("game", "move", "request", provider, serializers,
                                           zlink::message_t::from (std::string ("7")));
    if (!request_result
        || serializers.get<reply_t> ()
               .deserialize (
                 zlink::framework::detail::encoded_payload_from_raw (request_result.value ()))
               .value
             != 8) {
        return 3;
    }
    if (provider.get_required<handler_t> ().last_thread == std::this_thread::get_id ()) {
        return 30;
    }
    auto &audit_filter = provider.get_required<auditing_filter_t> ();
    if (audit_filter.before_count != 1 || audit_filter.after_count != 1
        || audit_filter.last_packet_name != "request" || audit_filter.last_context_channel != "game"
        || audit_filter.last_context_packet != "request"
        || audit_filter.last_dispatch_kind
             != zlink::framework::handler_dispatch_kind_t::channel_request) {
        return 35;
    }

    auto blocked_result = handlers.invoke ("game", "blocked", "blocked", provider, serializers,
                                           zlink::message_t::from (std::string ("123")));
    if (blocked_result
        || blocked_result.error_kind ()
             != zlink::framework::framework_error_kind_t::rejected) {
        return 36;
    }
    if (provider.get_required<handler_t> ().last_request == 123
        || provider.get_required<short_circuit_filter_t> ().short_circuit_count != 1) {
        return 37;
    }
    if (audit_filter.before_count != 2 || audit_filter.after_count != 1
        || audit_filter.last_packet_name != "blocked") {
        return 38;
    }

    auto duplicate_result =
      handlers.invoke ("game", "duplicate", "duplicate", provider, serializers,
                       zlink::message_t::from (std::string ("321")));
    if (duplicate_result
        || duplicate_result.error_kind ()
             != zlink::framework::framework_error_kind_t::invalid_operation
        || provider.get_required<duplicate_next_filter_t> ().duplicate_rejections != 1
        || provider.get_required<handler_t> ().last_request != 321) {
        return 39;
    }

    provider.get_required<handler_t> ().last_command = 0;
    auto blocked_send =
      handlers.invoke ("game", "blocked-send", "blocked-send", provider,
                       serializers, zlink::message_t::from (std::string ("41")));
    if (!blocked_send || provider.get_required<handler_t> ().last_command != 0) {
        return 40;
    }

    provider.get_required<handler_t> ().last_event = 0;
    auto blocked_event =
      handlers.invoke ("game", "blocked-event", "blocked-event", provider,
                       serializers, zlink::message_t::from (std::string ("51")));
    if (!blocked_event || provider.get_required<handler_t> ().last_event != 0) {
        return 41;
    }
    auto isolated_event =
      handlers.invoke ("game", "event", "event", provider, serializers,
                       zlink::message_t::from (std::string ("52")));
    if (!isolated_event || provider.get_required<handler_t> ().last_event != 52
        || audit_filter.last_dispatch_kind
             != zlink::framework::handler_dispatch_kind_t::classic_fanout) {
        return 42;
    }

    zlink::framework::detail::inbound_message_context_t inbound;
    inbound.message.mesh_name = "rooms";
    inbound.message.content_type = "application/json";
    inbound.message.correlation_id = "corr-77";
    inbound.message.metadata =
      zlink::framework::message_metadata_t ({{"trace-id", "trace-abc"}});
    auto context_request_result =
      handlers.invoke ("game", "context-move", "context-request", provider, serializers,
                       zlink::message_t::from (std::string ("8")), inbound);
    auto &handler = provider.get_required<handler_t> ();
    if (!context_request_result
        || serializers.get<reply_t> ()
               .deserialize (zlink::framework::detail::encoded_payload_from_raw (
                 context_request_result.value ()))
               .value
             != 10
        || handler.last_context_mesh != "rooms" || handler.last_context_channel != "game"
        || handler.last_context_packet != "context-request"
        || handler.last_context_content_type != "application/json"
        || handler.last_context_correlation != "corr-77"
        || handler.last_context_trace != "trace-abc"
        || handler.last_context_metadata_size != 1) {
        return 39;
    }

    auto send_result = handlers.invoke ("game", "command", "command", provider, serializers,
                                        zlink::message_t::from (std::string ("9")));
    if (!send_result || provider.get_required<handler_t> ().last_command != 9) {
        return 4;
    }

    auto context_send_result =
      handlers.invoke ("game", "context-command", "context-command", provider, serializers,
                       zlink::message_t::from (std::string ("10")), inbound);
    if (!context_send_result || handler.last_command != 10 || handler.last_context_mesh != "rooms"
        || handler.last_context_channel != "game"
        || handler.last_context_packet != "context-command"
        || handler.last_context_correlation != "corr-77"
        || handler.last_context_trace != "trace-abc") {
        return 40;
    }

    auto event_result = handlers.invoke ("game", "event", "event", provider, serializers,
                                         zlink::message_t::from (std::string ("11")));
    if (!event_result || provider.get_required<handler_t> ().last_event != 11) {
        return 5;
    }
    descriptor = handlers.find ("game", "event", "event");
    if (descriptor == nullptr
        || descriptor->execution != zlink::framework::handler_execution_t::offload) {
        return 6;
    }

    auto publish_inbound = inbound;
    publish_inbound.source = "node-a";
    auto context_event_result =
      handlers.invoke ("game", "context-event", "context-event", provider, serializers,
                       zlink::message_t::from (std::string ("12")), publish_inbound);
    if (!context_event_result || handler.last_event != 12 || handler.last_context_mesh != "rooms"
        || handler.last_context_channel != "game"
        || handler.last_context_packet != "context-event"
        || handler.last_context_correlation != "corr-77"
        || handler.last_context_trace != "trace-abc"
        || handler.last_context_topic != "context-event"
        || handler.last_context_source != "node-a") {
        return 41;
    }

    zlink::framework::handler_registry_t topic_handlers;
    topic_handlers.on_event<handler_t, event_t> ("game", "topic-a", &handler_t::on_event,
                                                 {.packet_name = "topic-event"});
    topic_handlers.on_event<handler_t, event_t> ("game", "topic-b", &handler_t::on_event,
                                                 {.packet_name = "topic-event"});
    if (topic_handlers.find ("game", "topic-a", "topic-event") == nullptr
        || topic_handlers.find ("game", "topic-b", "topic-event") == nullptr
        || topic_handlers.find ("game", "topic-c", "topic-event") != nullptr) {
        return 42;
    }

    auto async_result = handlers.invoke ("game", "async", "async", provider, serializers,
                                         zlink::message_t::from (std::string ("5")));
    if (!async_result
        || serializers.get<reply_t> ()
               .deserialize (
                 zlink::framework::detail::encoded_payload_from_raw (async_result.value ()))
               .value
             != 15) {
        return 7;
    }

    auto delayed_result = handlers.invoke ("game", "delayed", "delayed", provider, serializers,
                                           zlink::message_t::from (std::string ("6")));
    if (!delayed_result
        || serializers.get<reply_t> ()
               .deserialize (
                 zlink::framework::detail::encoded_payload_from_raw (delayed_result.value ()))
               .value
             != 26) {
        return 31;
    }

    auto shared = delayed_reply_task (40);
    auto first_waiter = await_shared_reply (shared, 1);
    auto second_waiter = await_shared_reply (shared, 2);
    if (first_waiter.result ().value () != 41 || second_waiter.result ().value () != 42) {
        return 32;
    }

    zlink::framework::detail::task_completion_source_t<int> completion;
    auto first_complete_wins = completion.task ();
    int callback_count = 0;
    int callback_value = 0;
    zlink::framework::detail::observe_task_completion (
      first_complete_wins,
      [&callback_count, &callback_value] (const zlink::framework::result_t<int> &result) {
          ++callback_count;
          callback_value = result.value ();
      });
    completion.complete (zlink::framework::result_t<int>::success (100));
    completion.complete (zlink::framework::result_t<int>::success (200));
    if (first_complete_wins.result ().value () != 100 || callback_count != 1
        || callback_value != 100) {
        return 33;
    }

    auto preserved_failure = await_timeout_task ().result ();
    if (preserved_failure
        || (preserved_failure.error () != nullptr
         && zlink::framework::detail::boundary_state (*preserved_failure.error ()) != zlink::framework::detail::boundary_error_t::timed_out)) {
        return 34;
    }

    auto missing_result = handlers.invoke ("game", "missing", "request", provider, serializers,
                                           zlink::message_t::from (std::string ("1")));
    if (missing_result
        || missing_result.error_kind ()
             != zlink::framework::framework_error_kind_t::not_found) {
        return 8;
    }

    auto decode_result = handlers.invoke ("game", "move", "request", provider, serializers,
                                          zlink::message_t::from (std::string ("bad")));
    if (decode_result
        || decode_result.error_kind ()
             != zlink::framework::framework_error_kind_t::protocol_error) {
        return 9;
    }
    if (failure_events != 3
        || last_failure_kind != zlink::framework::framework_error_kind_t::protocol_error) {
        return 10;
    }

    auto thrown_result = handlers.invoke ("game", "throw", "throw", provider, serializers,
                                          zlink::message_t::from (std::string ("1")));
    if (thrown_result
        || thrown_result.error_kind ()
             != zlink::framework::framework_error_kind_t::internal_failure) {
        return 11;
    }
    if (failure_events != 4
        || last_failure_kind != zlink::framework::framework_error_kind_t::internal_failure) {
        return 12;
    }

    zlink::framework::service_collection_t empty_services;
    auto empty_provider = empty_services.build_provider ();
    auto owner_result = handlers.invoke ("game", "move", "request", empty_provider, serializers,
                                         zlink::message_t::from (std::string ("1")));
    if (owner_result
        || owner_result.error_kind ()
             != zlink::framework::framework_error_kind_t::not_found) {
        return 13;
    }
    if (failure_events != 5
        || last_failure_kind
             != zlink::framework::framework_error_kind_t::not_found) {
        return 14;
    }

    bool raw_called = false;
    handlers.send_raw ("game", "raw-topic", "raw",
                       [&raw_called] (const zlink::framework::payload_view_t &payload) {
                           raw_called = payload.to_string () == "raw-body";
                           return zlink::framework::result_t<void>::success ();
                       });
    auto raw_result = handlers.invoke ("game", "raw-topic", "raw", provider, serializers,
                                       zlink::message_t::from (std::string ("raw-body")));
    if (!raw_result || !raw_called) {
        return 15;
    }

    zlink::framework::handler_registry_t default_handlers;
    default_handlers.on_send<handler_t, command_t> ("game", "default", &handler_t::on_command);
    if (default_handlers.find ("game", "default", command_t::packet_name) == nullptr) {
        return 16;
    }

    bool duplicate_failed = false;
    try {
        handlers.on_send<handler_t, command_t> ("game", "command", &handler_t::on_command,
                                                {.packet_name = "command"});
    }
    catch (const zlink::framework::framework_exception_t &error) {
        duplicate_failed =
          error.kind () == zlink::framework::framework_error_kind_t::protocol_error;
    }
    if (!duplicate_failed) {
        return 17;
    }

    zlink::framework::spot_handler_registry_t spot_handlers;
    spot_handlers.add_handler<&dispatch_spot_t::on_packet> ("spot-packet");
    spot_handlers.add_subscribe<&dispatch_spot_t::on_topic_event> ("spot-topic");
    spot_handlers.add_actor_send<&dispatch_spot_t::on_actor_packet> ("spot-actor-packet");

    zlink::framework::spot_inbound_message_t spot_inbound;
    spot_inbound.content_type = "application/json";
    spot_inbound.values = {{"trace-id", "trace-spot"}};
    spot_inbound.mesh_name = "rooms";
    spot_inbound.correlation_id = "corr-spot";
    spot_inbound.source = "node-b";

    dispatch_spot_t spot;
    spot_actor_t spot_actor{42};
    auto spot_actor_result = spot_handlers.invoke_actor_packet (
      "spot-actor-packet", spot, spot_actor, provider, serializers,
      zlink::message_t::from (std::string ("21")), spot_inbound);
    if (!spot_actor_result || spot.last_actor != 42 || spot.last_payload != 21
        || spot.last_mesh != "rooms" || spot.last_channel != "<none>"
        || spot.last_packet != "spot-actor-packet" || spot.last_content_type != "application/json"
        || spot.last_correlation != "corr-spot" || spot.last_trace != "trace-spot") {
        return 43;
    }

    const auto projected_packet_context = spot_inbound.to_message_context ("spot-packet");
    if (projected_packet_context.mesh_name.value_or ("") != "rooms"
        || projected_packet_context.channel_name.has_value ()
        || projected_packet_context.packet_name != "spot-packet"
        || projected_packet_context.content_type.value_or ("") != "application/json"
        || projected_packet_context.correlation_id.value_or ("") != "corr-spot"
        || projected_packet_context.metadata.find ("trace-id").value_or ("") != "trace-spot") {
        return 44;
    }

    const auto projected_publish_context =
      spot_inbound.to_publish_context ("spot-event", "spot-topic");
    if (projected_publish_context.mesh_name.value_or ("") != "rooms"
        || projected_publish_context.packet_name != "spot-event"
        || projected_publish_context.correlation_id.value_or ("") != "corr-spot"
        || projected_publish_context.metadata.find ("trace-id").value_or ("") != "trace-spot"
        || projected_publish_context.topic != "spot-topic"
        || projected_publish_context.source.value_or ("") != "node-b") {
        return 45;
    }

    return 0;
}
