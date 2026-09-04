/* SPDX-License-Identifier: MPL-2.0 */

#include "addon_exports.h"

#include "addon_core_api.h"

#define ZLINK_METHOD(js_name, native_fn)                                                           \
    {                                                                                              \
        js_name, 0, native_fn, 0, 0, 0, napi_default, 0                                            \
    }

static void
define_exports (napi_env env, napi_value exports, napi_property_descriptor *descs, size_t count)
{
    napi_define_properties (env, exports, count, descs);
}

void define_core_exports (napi_env env, napi_value exports)
{
    napi_property_descriptor descs[] = {
      ZLINK_METHOD ("version", version),
      ZLINK_METHOD ("errno", errno_value),
      ZLINK_METHOD ("strerror", strerror_value),
      ZLINK_METHOD ("has", has),
      ZLINK_METHOD ("proxy", proxy),
      ZLINK_METHOD ("sleep", sleep),
      ZLINK_METHOD ("messageFromBuffer", message_from_buffer),
      ZLINK_METHOD ("messageAllocate", message_allocate),
      ZLINK_METHOD ("messageFrameData", message_frame_data),
      ZLINK_METHOD ("messageFrameCopyData", message_frame_copy_data),
      ZLINK_METHOD ("messageFrameSize", message_frame_size),
      ZLINK_METHOD ("messageFrameClose", message_frame_close),
      ZLINK_METHOD ("ctxNew", ctx_new),
      ZLINK_METHOD ("ctxShutdown", ctx_shutdown),
      ZLINK_METHOD ("ctxTerm", ctx_term),
      ZLINK_METHOD ("ctxSetOpt", ctx_setopt),
      ZLINK_METHOD ("ctxGetOpt", ctx_getopt),
      ZLINK_METHOD ("ctxGetOptData", ctx_getopt_data),
      ZLINK_METHOD ("ctxRecalculateAutoHwm", ctx_recalculate_auto_hwm),
      ZLINK_METHOD ("ctxGetAutoHwmBudgetSnapshot", ctx_get_auto_hwm_budget_snapshot),
      ZLINK_METHOD ("ctxResetAutoHwmBudgetMetrics", ctx_reset_auto_hwm_budget_metrics),
      ZLINK_METHOD ("socketNew", socket_new),
      ZLINK_METHOD ("socketClose", socket_close),
      ZLINK_METHOD ("socketBind", socket_bind),
      ZLINK_METHOD ("socketUnbind", socket_unbind),
      ZLINK_METHOD ("socketConnect", socket_connect),
      ZLINK_METHOD ("socketDisconnect", socket_disconnect),
      ZLINK_METHOD ("socketDisconnectRid", socket_disconnect_rid),
      ZLINK_METHOD ("socketSetTlsServer", socket_set_tls_server),
      ZLINK_METHOD ("socketSetTlsClient", socket_set_tls_client),
      ZLINK_METHOD ("socketPublish", socket_publish),
      ZLINK_METHOD ("socketTryPublish", socket_try_publish),
      ZLINK_METHOD ("socketRecvMessage", socket_recv_message),
      ZLINK_METHOD ("socketRecvMessageNoWait", socket_try_recv_message),
      ZLINK_METHOD ("socketSubscribeMessage", socket_subscribe_message),
      ZLINK_METHOD ("socketTrySubscribeMessage", socket_try_subscribe_message),
      ZLINK_METHOD ("socketSubscriptionEvent", socket_subscription_event),
      ZLINK_METHOD ("socketTrySubscriptionEvent", socket_try_subscription_event),
      ZLINK_METHOD ("subscriptionAt", subscription_at),
      ZLINK_METHOD ("socketStreamRecvPacket", socket_stream_recv_packet),
      ZLINK_METHOD ("socketSetOpt", socket_setopt),
      ZLINK_METHOD ("socketGetOpt", socket_getopt),
      ZLINK_METHOD ("socketSetReceiveFlowState", socket_set_receive_flow_state),
      ZLINK_METHOD ("socketSetSubscription", socket_set_subscription),
      ZLINK_METHOD ("socketUnsetSubscription", socket_unset_subscription),
      ZLINK_METHOD ("handleSetRoutingId", handle_set_routing_id),
      ZLINK_METHOD ("handleGetRoutingId", handle_get_routing_id),
      ZLINK_METHOD ("socketSubmitSend", socket_submit_send),
      ZLINK_METHOD ("socketSubmitRequest", socket_submit_request),
      ZLINK_METHOD ("socketRequestSync", socket_request_sync),
      ZLINK_METHOD ("socketCompletionRecv", socket_completion_recv),
      ZLINK_METHOD ("socketReadableWatchStart", socket_readable_watch_start),
      ZLINK_METHOD ("socketReadableWatchStop", socket_readable_watch_stop),
      ZLINK_METHOD ("testCompletionCloseCount", test_completion_close_count),
      ZLINK_METHOD ("socketReply", socket_reply),
      ZLINK_METHOD ("routerRecvMessage", router_recv_message),
      ZLINK_METHOD ("routerRecvMessageNoWait", router_try_recv_message),
      ZLINK_METHOD ("monitorOpen", monitor_open),
      ZLINK_METHOD ("monitorRecv", monitor_recv),
      ZLINK_METHOD ("monitorRecvNoWait", monitor_try_recv),
      ZLINK_METHOD ("monitorStatus", monitor_status),
      ZLINK_METHOD ("monitorClose", monitor_close),
      ZLINK_METHOD ("pollerNew", poller_new),
      ZLINK_METHOD ("pollerDestroy", poller_destroy),
      ZLINK_METHOD ("pollerSize", poller_size),
      ZLINK_METHOD ("pollerAdd", poller_add),
      ZLINK_METHOD ("pollerModify", poller_modify),
      ZLINK_METHOD ("pollerRemove", poller_remove),
      ZLINK_METHOD ("pollerAddFd", poller_add_fd),
      ZLINK_METHOD ("pollerModifyFd", poller_modify_fd),
      ZLINK_METHOD ("pollerRemoveFd", poller_remove_fd),
      ZLINK_METHOD ("pollerAddTimer", poller_add_timer),
      ZLINK_METHOD ("pollerRemoveTimer", poller_remove_timer),
      ZLINK_METHOD ("pollerWait", poller_wait),
      ZLINK_METHOD ("pollEventsNew", poll_events_new),
      ZLINK_METHOD ("pollEventsDestroy", poll_events_destroy),
      ZLINK_METHOD ("pollEventsSourceKind", poll_events_source_kind),
      ZLINK_METHOD ("pollEventsSlot", poll_events_slot),
      ZLINK_METHOD ("pollEventsRevents", poll_events_revents),
      ZLINK_METHOD ("pollEventsFd", poll_events_fd),
      ZLINK_METHOD ("pollerWaitInto", poller_wait_into),
      ZLINK_METHOD ("timerNew", timer_new),
      ZLINK_METHOD ("timerDestroy", timer_destroy),
      ZLINK_METHOD ("timerStart", timer_start),
      ZLINK_METHOD ("timerStop", timer_stop),
      ZLINK_METHOD ("timerRecv", timer_recv),
      ZLINK_METHOD ("stopwatchStart", stopwatch_start),
      ZLINK_METHOD ("stopwatchIntermediate", stopwatch_intermediate),
      ZLINK_METHOD ("stopwatchStop", stopwatch_stop),
      ZLINK_METHOD ("atomicCounterNew", atomic_counter_new),
      ZLINK_METHOD ("atomicCounterSet", atomic_counter_set),
      ZLINK_METHOD ("atomicCounterInc", atomic_counter_inc),
      ZLINK_METHOD ("atomicCounterDec", atomic_counter_dec),
      ZLINK_METHOD ("atomicCounterValue", atomic_counter_value),
      ZLINK_METHOD ("atomicCounterDestroy", atomic_counter_destroy),
    };
    define_exports (env, exports, descs, sizeof (descs) / sizeof (*descs));

    if (getenv ("ZLINK_NODE_TEST_HOOKS")) {
        napi_property_descriptor test_descs[] = {
          ZLINK_METHOD ("testBeginHeldRoutedMultipart", test_begin_held_routed_multipart),
          ZLINK_METHOD ("testEndHeldRoutedMultipart", test_end_held_routed_multipart),
          ZLINK_METHOD ("testRunSendCloseStress", test_run_send_close_stress),
        };
        define_exports (
          env, exports, test_descs, sizeof (test_descs) / sizeof (*test_descs));
    }
}
