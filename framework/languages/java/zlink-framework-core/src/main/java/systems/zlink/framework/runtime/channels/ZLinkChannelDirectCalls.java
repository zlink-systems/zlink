package systems.zlink.framework.runtime.channels;
import java.util.concurrent.atomic.AtomicBoolean;

import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;

import systems.zlink.framework.runtime.internal.backend.*;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.Executor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.locks.LockSupport;
import java.util.function.Supplier;
import java.util.logging.Logger;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkCloseException;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.ZLinkHandlerFilter;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.channels.ZLinkClient;
import systems.zlink.framework.channels.ZLinkFanoutClient;
import systems.zlink.framework.channels.ZLinkFanoutPublishCall;
import systems.zlink.framework.channels.ZLinkChannelRuntimeOptions;
import systems.zlink.framework.channels.ZLinkClientServerChannelRuntimeOptions;
import systems.zlink.framework.channels.ZLinkPublishCall;
import systems.zlink.framework.channels.ZLinkPublishMessageContext;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.channels.ZLinkRequestCall;
import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.channels.ZLinkSendCall;
import systems.zlink.framework.channels.ZLinkSocketRuntimeOptions;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorAction;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorReason;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorSurface;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchMessageKind;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowEvent;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowOutcome;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchFailure;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectType;
import systems.zlink.framework.locations.ZLinkLocationRole;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkRuntimeEventDispatcher;
import systems.zlink.framework.runtime.internal.configuration.ZLinkCodecRegistration;
import systems.zlink.framework.runtime.configuration.ZLinkFrameworkRegistration;
import systems.zlink.framework.runtime.diagnostics.ZLinkDispatchErrorReporter;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerScanner;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerStages;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandler;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerCatalog;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerKind;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerSurface;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationAdapter;
import systems.zlink.framework.runtime.messaging.ZLinkPayloadEncoding;
import systems.zlink.framework.runtime.messaging.ZLinkMessagePayloads;


final class PublishCall implements ZLinkFanoutPublishCall {
    private final AtomicBoolean submitGate;
    private final ZLinkChannelCallRuntime runtime;
    private final ZLinkBackendPublisherSocket publisher;
    private final String topic;
    private final Message payload;
    private final Optional<String> packetName;
    private final String contentType;

    PublishCall(
        ZLinkChannelCallRuntime runtime,
        ZLinkBackendPublisherSocket publisher,
        String topic,
        Message payload,
        Optional<String> packetName) {
        this(runtime, publisher, topic, payload, packetName,
            ZLinkChannelContentTypeFrame.DEFAULT_CONTENT_TYPE);
    }

    PublishCall(
        ZLinkChannelCallRuntime runtime,
        ZLinkBackendPublisherSocket publisher,
        String topic,
        Message payload,
        Optional<String> packetName,
        String contentType) {
        this(runtime, publisher, topic, payload, packetName, contentType,
            new AtomicBoolean());
    }

    private PublishCall(
        ZLinkChannelCallRuntime runtime,
        ZLinkBackendPublisherSocket publisher,
        String topic,
        Message payload,
        Optional<String> packetName,
        String contentType,
        AtomicBoolean submitGate) {
        this.submitGate = submitGate;
        this.runtime = runtime;
        this.publisher = publisher;
        this.topic = topic;
        this.payload = payload;
        this.packetName = packetName;
        this.contentType = contentType;
    }

    PublishCall(
        ZLinkChannelCallRuntime runtime,
        ZLinkBackendPublisherSocket publisher,
        String topic,
        Message payload) {
        this(runtime, publisher, topic, payload, Optional.empty());
    }

    public ZLinkFanoutPublishCall packetName(String packetName) {
        return new PublishCall(
            runtime, publisher, topic, payload, Optional.of(packetName), contentType,
            submitGate);
    }

    @Override
    public CompletionStage<Void> submit() {
        CompletionStage<Void> duplicate =
            ZLinkOneWayCalls.beginOneWay(submitGate);
        if (duplicate != null) {
            return duplicate;
        }
        try (var flowScope = runtime.enterApplicationFlow()) {
            List<Message> publishParts = ZLinkChannelCallRuntime.parts(
                packetName, payload, contentType);
            try {
                return ZLinkOneWayCalls.oneWayStatus(
                    publisher.publish(topic, publishParts, SendFlags.DONT_WAIT)
                        ? ZLinkOneWayCalls.SUBMITTED
                        : ZLinkOneWayCalls.BACKPRESSURED);
            } finally {
                publishParts.forEach(Message::close);
            }
        }
    }
}

final class SendCall implements ZLinkSendCall {
    private final AtomicBoolean submitGate;
    private final ZLinkChannelCallRuntime runtime;
    private final ZLinkBackendDealerSocket client;
    private final Message payload;
    private final Optional<String> packetName;
    private final String contentType;

    SendCall(
        ZLinkChannelCallRuntime runtime,
        ZLinkBackendDealerSocket client,
        Message payload,
        Optional<String> packetName) {
        this(runtime, client, payload, packetName,
            ZLinkChannelContentTypeFrame.DEFAULT_CONTENT_TYPE);
    }

    SendCall(
        ZLinkChannelCallRuntime runtime,
        ZLinkBackendDealerSocket client,
        Message payload,
        Optional<String> packetName,
        String contentType) {
        this(runtime, client, payload, packetName, contentType,
            new AtomicBoolean());
    }

    private SendCall(
        ZLinkChannelCallRuntime runtime,
        ZLinkBackendDealerSocket client,
        Message payload,
        Optional<String> packetName,
        String contentType,
        AtomicBoolean submitGate) {
        this.submitGate = submitGate;
        this.runtime = runtime;
        this.client = client;
        this.payload = payload;
        this.packetName = packetName;
        this.contentType = contentType;
    }

    SendCall(ZLinkChannelCallRuntime runtime, ZLinkBackendDealerSocket client, Message payload) {
        this(runtime, client, payload, Optional.empty());
    }

    public ZLinkSendCall packetName(String packetName) {
        return new SendCall(
            runtime, client, payload, Optional.of(packetName), contentType, submitGate);
    }

    @Override
    public CompletionStage<Void> submit() {
        CompletionStage<Void> duplicate =
            ZLinkOneWayCalls.beginOneWay(submitGate);
        if (duplicate != null) {
            return duplicate;
        }
        try (var flowScope = runtime.enterApplicationFlow()) {
        ZLinkMessageFlowTracer.TracePoint sent =
            runtime.flow().begin(ZLinkMessageFlowOutcome.SENT);
        if (sent != null) {
            sent.trace(new ZLinkMessageFlowEvent(
                ZLinkMessageFlowOutcome.SENT,
                ZLinkDispatchErrorSurface.CHANNEL,
                ZLinkDispatchMessageKind.SEND,
                packetName.orElse(null), null, null, null, null, null, null, null));
        }
        List<Message> parts = ZLinkChannelCallRuntime.parts(
            packetName, payload, contentType);
        return ZLinkOneWayCalls.adaptOneWay(client.send(parts))
            .whenComplete((ignored, failure) ->
                parts.forEach(Message::close));
        }
    }
}

final class RequestCall implements ZLinkRequestCall {
    private final AtomicBoolean submitGate;
    private final ZLinkChannelCallRuntime runtime;
    private final ZLinkBackendDealerSocket client;
    private final Message payload;
    private final Optional<String> packetName;
    private final Duration timeout;
    private final String contentType;
    //  spec 25 requires mesh_name and surface on every mesh_node request
    //  instrument, and outcome on the duration histogram. The label sets are
    //  fixed per mesh and surface, so they are built once and shared instead of
    //  being rebuilt on each request.
    private final ZLinkRequestMetricTags metricTags;

    RequestCall(
        ZLinkChannelCallRuntime runtime,
        ZLinkBackendDealerSocket client,
        Message payload,
        Optional<String> packetName,
        Duration timeout,
        ZLinkRequestMetricTags metricTags) {
        this(runtime, client, payload, packetName, timeout, metricTags,
            ZLinkChannelContentTypeFrame.DEFAULT_CONTENT_TYPE);
    }

    RequestCall(
        ZLinkChannelCallRuntime runtime,
        ZLinkBackendDealerSocket client,
        Message payload,
        Optional<String> packetName,
        Duration timeout,
        ZLinkRequestMetricTags metricTags,
        String contentType) {
        this(runtime, client, payload, packetName, timeout, metricTags,
            contentType, new AtomicBoolean());
    }

    private RequestCall(
        ZLinkChannelCallRuntime runtime,
        ZLinkBackendDealerSocket client,
        Message payload,
        Optional<String> packetName,
        Duration timeout,
        ZLinkRequestMetricTags metricTags,
        String contentType,
        AtomicBoolean submitGate) {
        this.submitGate = submitGate;
        this.metricTags = metricTags;
        this.runtime = runtime;
        this.client = client;
        this.payload = payload;
        this.packetName = packetName;
        this.timeout = timeout;
        this.contentType = contentType;
    }

    public ZLinkRequestCall packetName(String packetName) {
        return new RequestCall(
            runtime, client, payload, Optional.of(packetName), timeout, metricTags, contentType,
            submitGate);
    }

    @Override
    public ZLinkRequestCall timeout(Duration timeout) {
        return new RequestCall(
            runtime, client, payload, packetName, timeout, metricTags, contentType, submitGate);
    }

    @Override
    public <TReply> CompletionStage<TReply> submit(Class<TReply> replyType) {
        CompletionStage<TReply> duplicate =
            ZLinkOneWayCalls.beginOneWay(submitGate);
        if (duplicate != null) {
            return duplicate;
        }
        try (var flowScope = runtime.enterApplicationFlow()) {
        CompletableFuture<TReply> result = new CompletableFuture<>();
        String reqPacket = packetName.orElse(null);
        result.whenComplete((ignored, error) -> {
            ZLinkMessageFlowTracer.TerminalTracePoint terminal =
                runtime.flow().beginRequestTerminal(error, result);
            if (terminal != null) {
                terminal.trace(new ZLinkMessageFlowEvent(
                    ZLinkMessageFlowOutcome.REPLY_RECEIVED,
                    ZLinkDispatchErrorSurface.CHANNEL,
                    ZLinkDispatchMessageKind.REQUEST,
                    reqPacket, null, null, null, null, null, null, null));
            }
        });
        //  Nothing on this path allocates while metrics are off: no label map is
        //  built and no completion callback is registered.
        if (ZLinkRuntimeMetrics.enabled()) {
            final long started = System.nanoTime();
            ZLinkRuntimeMetrics.add(
                "zlink.mesh_node.requests.inflight", 1, metricTags.request);
            result.whenComplete((ignored, error) -> {
                ZLinkRuntimeMetrics.add(
                    "zlink.mesh_node.requests.inflight", -1, metricTags.request);
                boolean timedOut = error instanceof TimeoutException
                    || (error != null && error.getCause() instanceof TimeoutException);
                ZLinkRuntimeMetrics.record("zlink.mesh_node.request.duration",
                    Duration.ofNanos(System.nanoTime() - started),
                    metricTags.duration(timedOut, error != null));
                if (timedOut) {
                    ZLinkRuntimeMetrics.increment(
                        "zlink.mesh_node.request.timeouts", metricTags.request);
                }
            });
        }
        runtime.track(result, timeout);
        List<Message> requestParts = ZLinkChannelCallRuntime.parts(
            packetName, payload, contentType);
        result.whenComplete((ignored, error) -> requestParts.forEach(Message::close));
        ZLinkMessageFlowTracer.TracePoint sent =
            runtime.flow().begin(ZLinkMessageFlowOutcome.SENT);
        if (sent != null) {
            sent.trace(new ZLinkMessageFlowEvent(
                ZLinkMessageFlowOutcome.SENT,
                ZLinkDispatchErrorSurface.CHANNEL,
                ZLinkDispatchMessageKind.REQUEST,
                reqPacket, null, null, null, null, null, null, null));
        }
        runtime.requestClient(client, requestParts, timeout)
            .whenComplete((reply, failure) -> {
                if (failure != null) {
                    result.completeExceptionally(
                        ZLinkChannelCallRuntime.unwrap(failure));
                    return;
                }
                if (result.isDone()) {
                    reply.close();
                    return;
                }
                try {
                    runtime.completeReply(reply, replyType, result);
                } catch (RuntimeException ex) {
                    result.completeExceptionally(ex);
                } finally {
                    reply.close();
                }
            });
        return ZLinkSerialExecutionQueue.manageCurrent(result);
        }
    }

    @Override
    public <TReply> CompletionStage<TReply> yield(Class<TReply> replyType) {
        systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.requireYieldAllowed("Channel request");
        return ZLinkSerialExecutionQueue
            .yieldCurrent(submit(replyType));
    }

}
