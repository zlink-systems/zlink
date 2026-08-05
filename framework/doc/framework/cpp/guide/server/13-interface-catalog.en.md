---
title: "13. Key Type Usage Index · C++"
---

<!-- framework-adapter-nav:start -->
[Guide Home](../../../index.ko.md) | [Previous: Operations — metrics · drain · readiness](12-operations.en.md) | [Next: Picking A Sample](14-samples.en.md)
<!-- framework-adapter-nav:end -->

# 13. Key Type Usage Index

> **The document that owns this chapter's contract** —
> the [C++ exact interface table of contents](../../../common/spec/server/languages/cpp/interfaces/README.ko.md)
> owns the exact signatures. This chapter is a guide to finding the public types an
> application uses often, organized by feature.

C++ framework type names use a `_t` suffix. It's fastest to read them by splitting
**types the application creates directly** from **types injected via DI** — the former you
inherit or declare, the latter you list in `dependency_types` and receive through the
constructor.

## 1. Channel Messaging

The calling side receives a client injected via DI.

```cpp
class place_order_handler_t
{
  public:
    using dependency_types = dependency_list_t<route_client_t>;

    task_t<order_placed_t> handle (const place_order_t &request)
    {
        co_return co_await _client
          .request_to_channel ("orders", request)
          .timeout (std::chrono::seconds (3))
          .submit<order_placed_t> ();
    }

  private:
    route_client_t &_client;
};
```

| Type | What the application does with it |
| --- | --- |
| `route_client_t` | send / request by ChannelName or a managed Node RID |
| `publisher_t` · `spot_publisher_client_t` | Publish an event to a classic fanout channel |
| `route_send_call_t` | A one-way submission |
| `channel_request_call_t` | Specifying a timeout and receiving a typed reply |
| `message_context_t` · `route_message_context_t` | This dispatch's metadata and origin |
| `handler_filter_context_t` | The dispatch information a filter sees |

**A handler is a class, and its contract is declared as members.** `request_type`,
`reply_type`, and `topic_name` are that contract. A return type of `task_t<TReply>` makes it
a request, `task_t<void>` makes it a send.

Use node-direct (`request_to_node`) only when you're managing a specific MeshNode itself.
Address business objects by ActorId, SpotId, or ChannelName.

## 2. Topology Registration

Builders used only during the startup phase. None of these exist after `app.run ()`.

| Type | What it registers |
| --- | --- |
| `zlink_framework_options_t` | The root — codec, handler group, location store, dispatch |
| `mesh_node_builder_t` | A single MeshNode (`add_route_mesh`) |
| `mesh_channel_builder_t` | That node's channel role (`channel_name`) |
| `mesh_channel_server_builder_t` · `mesh_channel_client_builder_t` | Handler registration vs. call-only declaration |
| `fanout_channel_builder_t` | A classic fanout channel (`add_fanout_channel`) |
| `client_server_channel_builder_t` | A client/server channel pair (`add_client_server_channel`) |
| `stream_node_options_builder_t` | A STREAM node (`add_stream_node`) |
| `mesh_peer_connections_t` · `endpoint_connections_t` | Manual peer connections |
| `mesh_node_socket_config_t` | Socket caps ([16. Options](16-options.en.md) §3.1) |

`mesh_channel_builder_t` calls `client ()` or `server ()` **exactly once**.

## 3. Spot

Types the application inherits to build are split from context the framework hands in.

| Type | Nature |
| --- | --- |
| `spot_t<TActor>` | User Spot — built by inheriting |
| `entry_spot_t<TActor>` | Entry Spot — built by inheriting |
| `instance_spot_t` | Instance Spot — built by inheriting |
| `spot_context_t` · `entry_spot_context_t` · `instance_spot_context_t` | Received via constructor |
| `spot_common_context_t` | The part shared by the three above |
| `spot_manager_t` | Received via DI; creates and finds Spots |
| `spot_ref_t` | A reference carrying SpotId and generation |
| `spot_create_call_t` · `spot_create_result_t` | The create call and its result |
| `spot_create_response_t` | A creation callback's accept / reject |
| `spot_actor_join_result_t` | A join admission's accept / reject |
| `spot_closing_context_t` | Deadline info handed in while closing |
| `spot_handler_registry_t` · `instance_spot_handler_registry_t` | Registers handlers inside `configure ()` |
| `spot_relocation_adapter_t<TSpot>` | The adapter that packs and unpacks state |
| `spot_relocation_ready_call_t` · `spot_relocation_ready_completion_t` | The relocation-ready signal and its result |
| `user_spot_factory_builder_t` · `instance_spot_factory_builder_t` | Specifying policy at registration |

**A Spot handler is a member function of the Spot.** Register it in `configure ()` as
`add_handler<&TSpot::method> ()`. There's one exception — **only the timer has a separate
handler type**, registered with `add_timer<THandler> ()`, and that type's `handle` takes
both the target Spot and the tick ([6. Spot](06-spot.ko.md) §6.1).

| Timer-related type | What it does |
| --- | --- |
| `timer_t` | The handle registration returns. Used with `cancel ()` |
| `timer_options_t` | Overrun policy and catch-up cap |
| `timer_tick_t` | Per-tick delay, skipped count, etc. |
| `timer_failure_event_t` | Fires when the tick handler fails |

## 4. Actor

| Type | Nature |
| --- | --- |
| `actor_t` | Built by inheriting |
| `actor_context_t` | Received via constructor. Join / bound-session access |
| `actor_manager_t` | Received via DI; creates and finds Actors |
| `actor_client_t` | send / request by ActorId |
| `actor_ref_t` · `actor_id_t` | Reference and identifier |
| `actor_factory_t` · `actor_factory_builder_t` | Creation method and registration policy |
| `actor_create_call_t` | The create call |
| `actor_create_created_t` · `actor_create_existing_t` · `actor_create_rejected_t` | The three-way creation result |
| `actor_create_response_t` | The Entry Spot's admission response |
| `actor_join_call_t` | Reserving a join |
| `actor_join_accepted_t` · `actor_join_rejected_t` · `actor_join_failed_t` | The three-way join completion |
| `actor_relocation_adapter_t<TActor>` | The adapter that packs and unpacks state |
| `session_actor_t` · `session_actor_manager_t` | An Actor bound to a session |

Both the creation result and the join completion are **three-way types**. Split them with
`std::get_if<...>` or `std::visit`.

## 5. STREAM Session

| Type | Nature |
| --- | --- |
| `packet_stream_session_t` | Built by inheriting. Override `on_packet` · `on_connected`, etc. |
| `stream_t` | The connection itself. reply / send / close |
| `session_message_context_t` | This packet's dispatch info |
| `stream_error_t` | An error notification |
| `stream_send_call_t` · `stream_write_call_t` | Sending |
| `bound_session_t` · `bound_session_send_call_t` | Pushing to a session bound to an Actor |
| `bind_actor_call_t` | Ties a session to an Actor |
| `stream_compression_options_builder_t` · `stream_compression_codec_t` | Compression configuration |
| `stream_snapshot_t` | Status query |

**A C++ session branches from a single `on_packet`, not a handler registry.** This is where
its shape differs from the other four languages ([9. STREAM](09-stream.en.md)).

## 6. Location And Relocation

| Type | Nature |
| --- | --- |
| `location_store_t` · `relocation_store_t` | Implement directly or use a provided implementation |
| `redis_location_store_t` · `redis_location_options_t` | The Redis implementation and its settings |
| `redis_relocation_store_t` · `redis_relocation_options_t` | Same, for relocation |
| `location_options_t` | Behavior values ([16. Options](16-options.en.md) §5) |
| `location_readiness_t` | Whether the required peers are Ready |
| `location_runtime_query_t` | Status and topology queries |
| `location_runtime_status_t` · `location_topology_entry_t` | Query results |
| `location_page_t` · `location_page_request_t` | Paged queries |

Implementing a store yourself is rare. You'll only look at the `store_*` / `blob_*` family
then.

## 7. Host And Observation

| Type | Nature |
| --- | --- |
| `app_t` | The entry point. `create ()` · `run ()` |
| `framework_runtime_t` | Host status, plus relocate / shutdown |
| `route_mesh_runtime_t` | A MeshNode's status snapshot and observation |
| `route_mesh_runtime_options_t` | Adjusting weight while running |
| `client_server_runtime_t` · `fanout_runtime_t` | That channel's status |
| `message_flow_observer_t` | Receiving message flow records |
| `framework_exception_t` | A failure. `kind ()` · `is_retriable ()` |
| `logger_t<TOwner>` | The logger received via DI |

Usage of the observation surfaces is covered in [11. Monitoring](11-monitoring.en.md).

## 8. Where They Come From

| How you get it | Types |
| --- | --- |
| You inherit it | `spot_t` · `entry_spot_t` · `instance_spot_t` · `actor_t` · `packet_stream_session_t` |
| Received via constructor (context) | The `spot_context_t` family · `actor_context_t` · `message_context_t` |
| Injected via `dependency_types` | `route_client_t` · `publisher_t` · `actor_client_t` · `channel_client_t` · `spot_manager_t` · `actor_manager_t` · `logger_t<T>` |
| A startup-phase builder returns it | The `mesh_node_builder_t` family · `stream_node_options_builder_t` |
| A call returns it | `*_call_t` · `*_result_t` · `*_ref_t` |

DI injection rules are covered in [18. DI Container](18-di-container.ko.md).

## 9. Related Documents

- Exact signatures: [C++ exact interface table of contents](../../../common/spec/server/languages/cpp/interfaces/README.ko.md)
- The execution model and `task_t` / `result_t`: [21. Execution & Configuration Model](21-execution-model.ko.md)
- Options and defaults: [16. Options](16-options.en.md)
- Observation surfaces: [11. Monitoring](11-monitoring.en.md)
