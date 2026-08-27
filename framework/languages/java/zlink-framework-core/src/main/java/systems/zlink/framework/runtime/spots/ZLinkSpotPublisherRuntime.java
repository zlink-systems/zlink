package systems.zlink.framework.runtime.spots;
import java.util.Objects;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicBoolean;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject;

import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;

import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.SynchronousQueue;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.logging.Level;
import java.util.logging.Logger;
import java.time.Duration;
import java.util.function.Function;
import systems.zlink.contracts.errors.ZlinkCloseException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.channels.ZLinkPublishCall;

import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.messaging.ZLinkPayloadEncoding;
import systems.zlink.framework.runtime.messaging.ZLinkApplicationMetadata;
import systems.zlink.framework.spots.ZLinkSpotPublisherClient;

final class ZLinkSpotPublisherRuntime implements AutoCloseable {
    private static final Logger LOGGER =
        Logger.getLogger(ZLinkSpotPublisherRuntime.class.getName());
    private final ZLinkMessageSerializer serializer;
    private final ZLinkSpotRouteMessages messages;
    private final ThreadPoolExecutor multicastExecutor;
    private final ThreadPoolExecutor multicastHandoffExecutor;
    private final Function<ZLinkBackendObject,
        Duration> admissionTimeout;
    private final Function<Class<?>, String> contentTypeResolver;
    private final ZLinkMessageFlowTracer flow;
    private final ZLinkStateLane stateLane = new ZLinkStateLane();
    private final Map<String, ZLinkInternalSpotNode> nodesByChannel = new HashMap<>();
    private final Map<String, ZLinkBackendSpot> spotsByChannel = new HashMap<>();
    private boolean closed;

    ZLinkSpotPublisherRuntime(
        ZLinkMessageSerializer serializer,
        ZLinkSpotRouteMessages messages) {
        this(
            serializer,
            messages,
            Math.max(2, Runtime.getRuntime().availableProcessors()),
            ignored -> Duration.ofSeconds(1));
    }

    ZLinkSpotPublisherRuntime(
        ZLinkMessageSerializer serializer,
        ZLinkSpotRouteMessages messages,
        int parallelism) {
        this(serializer, messages, parallelism,
            ignored -> Duration.ofSeconds(1));
    }

    ZLinkSpotPublisherRuntime(
        ZLinkMessageSerializer serializer,
        ZLinkSpotRouteMessages messages,
        int parallelism,
        Function<ZLinkBackendObject,
            Duration> admissionTimeout) {
        this(
            serializer,
            messages,
            parallelism,
            admissionTimeout,
            ignored -> systems.zlink.framework.runtime.channels
                .ZLinkChannelContentTypeFrame.DEFAULT_CONTENT_TYPE);
    }

    ZLinkSpotPublisherRuntime(
        ZLinkMessageSerializer serializer,
        ZLinkSpotRouteMessages messages,
        int parallelism,
        Function<ZLinkBackendObject,
            Duration> admissionTimeout,
        Function<Class<?>, String> contentTypeResolver) {
        this(
            serializer,
            messages,
            parallelism,
            admissionTimeout,
            contentTypeResolver,
            null);
    }

    ZLinkSpotPublisherRuntime(
        ZLinkMessageSerializer serializer,
        ZLinkSpotRouteMessages messages,
        int parallelism,
        Function<ZLinkBackendObject,
            Duration> admissionTimeout,
        Function<Class<?>, String> contentTypeResolver,
        ZLinkMessageFlowTracer flow) {
        this.serializer = serializer;
        this.messages = messages;
        this.flow = flow;
        this.admissionTimeout = Objects.requireNonNull(
            admissionTimeout, "admissionTimeout");
        this.contentTypeResolver = Objects.requireNonNull(
            contentTypeResolver, "contentTypeResolver");
        this.multicastExecutor = new ThreadPoolExecutor(
            parallelism,
            parallelism,
            30L,
            TimeUnit.SECONDS,
            new SynchronousQueue<>(),
            runnable -> {
                Thread thread = new Thread(runnable, "zlink-logical-multicast");
                thread.setDaemon(true);
                return thread;
            },
            new ThreadPoolExecutor.AbortPolicy());
        this.multicastExecutor.allowCoreThreadTimeOut(true);
        this.multicastHandoffExecutor = new ThreadPoolExecutor(
            1,
            1,
            30L,
            TimeUnit.SECONDS,
            new SynchronousQueue<>(),
            runnable -> {
                Thread thread = new Thread(
                    runnable,
                    "zlink-logical-multicast-admission");
                thread.setDaemon(true);
                return thread;
            },
            new ThreadPoolExecutor.AbortPolicy());
        this.multicastHandoffExecutor.allowCoreThreadTimeOut(true);
    }

    void register(String channelName, ZLinkInternalSpotNode node) {
        inStateLane(() -> {
            nodesByChannel.put(channelName, node);
            return null;
        });
    }

    boolean contains(String channelName) {
        return inStateLane(() -> nodesByChannel.containsKey(channelName));
    }

    ZLinkSpotPublisherClient client() {
        return new ZLinkDefaultSpotPublisherClient(this);
    }

    ZLinkPublishCall publish(
        String channelName,
        String topic,
        Object message) {
        return publish(channelName, channelName, topic, message);
    }

    ZLinkPublishCall publish(
        String meshName,
        String channelName,
        String topic,
        Object message) {
        if (channelName == null || channelName.isBlank()) {
            throw new ZLinkConfigurationException("SPOT publisher channel name is required");
        }
        if (topic == null || topic.isBlank()) {
            throw new ZLinkConfigurationException("SPOT publish topic is required");
        }
        requireChannel(meshName);
        ZLinkPayloadEncoding.EncodedPayload encoded =
            ZLinkPayloadEncoding.encode(
                serializer,
                message,
                contentTypeResolver.apply(ZLinkPayloadEncoding.declaredType(message)));
        return call(
            meshName,
            channelName,
            topic,
            encoded.payload(),
            Optional.of(encoded.packetName()),
            encoded.contentType());
    }

    ZLinkPublishCall call(
        String meshName,
        String channelName,
        String topic,
        Message payload,
        Optional<String> packetName) {
        return call(meshName, channelName, topic, payload, packetName, null);
    }

    ZLinkPublishCall call(
        String meshName,
        String channelName,
        String topic,
        Message payload,
        Optional<String> packetName,
        String contentType) {
        requireChannel(meshName);
        return new ZLinkExternalSpotPublishCall(
            this, meshName, channelName, topic, payload, packetName, contentType);
    }

    /**
     * R1 value-passing (spec 27 §4): the publish flow — the ambient callback
     * flow or a new APPLICATION flow for a first outbound started outside
     * framework callbacks — is captured as a value on the submitting thread
     * and handed to the encoder explicitly. The multicast executor hop
     * receives the value; no scope is installed and the admission future the
     * publish turn returns stays bare, so the spot dispatch lane's
     * release/drain chain gains no completion hop that can be lost during
     * teardown. At Off nothing is captured or allocated.
     */
    private ZLinkFlowContext.State captureOutboundFlow() {
        if (flow == null || !flow.captureEnabled()) {
            return null;
        }
        ZLinkFlowContext.State current = ZLinkFlowContext.current();
        return current != null
            ? current
            : ZLinkFlowContext.create(ZLinkFlowOrigin.APPLICATION);
    }

    void submitNow(
        String meshName,
        String channelName,
        String topic,
        Message payload,
        Optional<String> packetName,
        String contentType,
        ZLinkApplicationMetadata metadata) {
        submitNow(
            meshName,
            channelName,
            topic,
            payload,
            packetName,
            contentType,
            metadata,
            captureOutboundFlow());
    }

    private void submitNow(
        String meshName,
        String channelName,
        String topic,
        Message payload,
        Optional<String> packetName,
        String contentType,
        ZLinkApplicationMetadata metadata,
        ZLinkFlowContext.State flowState) {
        List<Message> parts = messages.encodePublish(
            channelName, topic, packetName, payload, contentType,
            metadata.values(), flowState);
        try {
            requireChannel(meshName).publish(
                channelName,
                topic,
                metadata.encode(),
                parts,
                SendFlags.DONT_WAIT);
        } catch (ZlinkSubmitException error) {
            // The source-local executor admitted the operation before this call.
            // Per-target transport failures do not change the publish terminal.
        } finally {
            parts.forEach(Message::close);
        }
    }

    void submitNow(
        String meshName,
        String channelName,
        String topic,
        Message payload,
        Optional<String> packetName,
        ZLinkApplicationMetadata metadata) {
        submitNow(meshName, channelName, topic, payload, packetName, null, metadata);
    }

    CompletionStage<ZLinkOneWayPublishAdmission> submitAsync(
        String meshName,
        String channelName,
        String topic,
        Message payload,
        Optional<String> packetName,
        String contentType,
        ZLinkApplicationMetadata metadata) {
        MulticastFuture result = new MulticastFuture(payload);
        if (isClosed()) {
            result.closePayloadOnce();
            result.completeRejected(new ZLinkOneWayPublishAdmission(
                ZLinkOneWayCalls.SHUTDOWN));
            return result;
        }
        //  Captured on the submitting thread; the executor hop below would
        //  otherwise lose the ambient flow (R1 value-passing).
        ZLinkFlowContext.State flowState = captureOutboundFlow();
        Runnable operation = () -> executeMulticast(
            meshName, channelName, topic, payload, packetName, contentType,
            metadata, flowState, result, true);
        Runnable handoffOperation = () -> executeMulticast(
            meshName, channelName, topic, payload, packetName, contentType,
            metadata, flowState, result, false);
        try {
            multicastExecutor.execute(operation);
        } catch (RejectedExecutionException rejected) {
            if (isClosed() || multicastExecutor.isShutdown()) {
                result.closePayloadOnce();
                result.completeRejected(emptyAdmission(ZLinkOneWayCalls.SHUTDOWN));
                return result;
            }
            int timeoutMillis = normalizedTimeoutMillis(
                admissionTimeout.apply(requireChannel(meshName)));
            try {
                multicastHandoffExecutor.execute(() -> awaitExecutorAdmission(
                    handoffOperation, result, timeoutMillis));
            } catch (RejectedExecutionException capacityExhausted) {
                result.closePayloadOnce();
                result.completeRejected(emptyAdmission(
                    isClosed() || multicastExecutor.isShutdown()
                        ? ZLinkOneWayCalls.SHUTDOWN
                        : ZLinkOneWayCalls.TIMED_OUT));
            }
        }
        return result;
    }

    private void executeMulticast(
        String meshName,
        String channelName,
        String topic,
        Message payload,
        Optional<String> packetName,
        String contentType,
        ZLinkApplicationMetadata metadata,
        ZLinkFlowContext.State flowState,
        MulticastFuture result,
        boolean completeBeforeSubmit) {
        if (!result.beginCommit()) {
            result.closePayloadOnce();
            return;
        }
        if (completeBeforeSubmit) {
            result.completeCommitted(new ZLinkOneWayPublishAdmission(
                ZLinkOneWayCalls.SUBMITTED));
        }
        try {
            submitNow(
                meshName, channelName, topic, payload, packetName, contentType,
                metadata, flowState);
        } catch (RuntimeException error) {
            LOGGER.log(
                Level.WARNING,
                "Logical Multicast target processing failed after source-local admission.",
                error);
        } finally {
            if (!completeBeforeSubmit) {
                result.completeCommitted(new ZLinkOneWayPublishAdmission(
                    ZLinkOneWayCalls.SUBMITTED));
            }
            result.closePayloadOnce();
        }
    }

    CompletionStage<ZLinkOneWayPublishAdmission> submitAsync(
        String meshName,
        String channelName,
        String topic,
        Message payload,
        Optional<String> packetName,
        ZLinkApplicationMetadata metadata) {
        return submitAsync(
            meshName,
            channelName,
            topic,
            payload,
            packetName,
            null,
            metadata);
    }

    private void awaitExecutorAdmission(
        Runnable operation,
        MulticastFuture result,
        int timeoutMillis) {
        try {
            if (multicastExecutor.getQueue().offer(
                    operation, timeoutMillis, TimeUnit.MILLISECONDS)) {
                return;
            }
            result.closePayloadOnce();
            result.completeRejected(emptyAdmission(
                isClosed() || multicastExecutor.isShutdown()
                    ? ZLinkOneWayCalls.SHUTDOWN
                    : ZLinkOneWayCalls.TIMED_OUT));
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            result.closePayloadOnce();
            result.completeRejected(emptyAdmission(
                isClosed() || multicastExecutor.isShutdown()
                    ? ZLinkOneWayCalls.SHUTDOWN
                    : ZLinkOneWayCalls.TIMED_OUT));
        }
    }

    private static ZLinkOneWayPublishAdmission emptyAdmission(
        int status) {
        return new ZLinkOneWayPublishAdmission(status);
    }

    private static int normalizedTimeoutMillis(Duration timeout) {
        if (timeout == null || timeout.isZero() || timeout.isNegative()) {
            throw new IllegalArgumentException("send timeout must be positive");
        }
        long millis = timeout.toMillis();
        if (timeout.compareTo(Duration.ofMillis(millis)) > 0) {
            millis++;
        }
        if (millis < 1L || millis > Integer.MAX_VALUE) {
            throw new IllegalArgumentException(
                "send timeout must normalize to 1..Integer.MAX_VALUE ms");
        }
        return (int) millis;
    }

    @Override
    public void close() {
        List<ZLinkBackendSpot> spots;
        spots = inStateLane(() -> {
            if (closed) {
                return null;
            }
            closed = true;
            List<ZLinkBackendSpot> current = List.copyOf(spotsByChannel.values());
            spotsByChannel.clear();
            return current;
        });
        if (spots == null) {
            return;
        }
        RuntimeException firstFailure = null;
        for (ZLinkBackendSpot spot : spots) {
            try {
                spot.close();
            } catch (ZlinkCloseException ignored) {
            } catch (RuntimeException error) {
                if (firstFailure == null) {
                    firstFailure = error;
                } else {
                    firstFailure.addSuppressed(error);
                }
            }
        }
        multicastExecutor.shutdownNow();
        multicastHandoffExecutor.shutdownNow();
        if (firstFailure != null) {
            throw firstFailure;
        }
    }

    private static final class MulticastFuture
        extends CompletableFuture<ZLinkOneWayPublishAdmission> {
        private final Message payload;
        private final ZLinkStateLane stateLane = new ZLinkStateLane(Runnable::run);
        private boolean committed;
        private boolean cancelled;
        private boolean payloadReleased;

        MulticastFuture(Message payload) {
            this.payload = payload;
        }

        boolean beginCommit() {
            return inStateLane(() -> {
                if (cancelled) {
                    return false;
                }
                committed = true;
                return true;
            });
        }

        @Override
        public boolean cancel(boolean mayInterruptIfRunning) {
            boolean accepted = inStateLane(() -> {
                if (committed || cancelled) {
                    return false;
                }
                cancelled = true;
                return true;
            });
            if (!accepted) {
                return false;
            }
            closePayloadOnce();
            return super.cancel(false);
        }

        void closePayloadOnce() {
            if (inStateLane(() -> {
                if (payloadReleased) {
                    return false;
                }
                payloadReleased = true;
                return true;
            })) {
                payload.close();
            }
        }

        void completeCommitted(ZLinkOneWayPublishAdmission value) {
            super.complete(value);
        }

        void completeRejected(ZLinkOneWayPublishAdmission value) {
            super.complete(value);
        }

        private <T> T inStateLane(java.util.function.Supplier<T> work) {
            try {
                return stateLane.runAsync(work).toCompletableFuture().join();
            } catch (CompletionException failure) {
                Throwable cause = failure.getCause();
                if (cause instanceof RuntimeException runtimeFailure) {
                    throw runtimeFailure;
                }
                if (cause instanceof Error error) {
                    throw error;
                }
                throw failure;
            }
        }
    }

    private ZLinkBackendSpot publisherSpot(String channelName) {
        return inStateLane(() -> {
            if (closed) {
                throw new ZLinkConfigurationException("SPOT publisher runtime is closed");
            }
            ZLinkInternalSpotNode node = requireChannelCore(channelName);
            return spotsByChannel.computeIfAbsent(channelName, ignored -> node.createSpot());
        });
    }

    private ZLinkInternalSpotNode requireChannel(String channelName) {
        return inStateLane(() -> requireChannelCore(channelName));
    }

    private ZLinkInternalSpotNode requireChannelCore(String channelName) {
        ZLinkInternalSpotNode node = nodesByChannel.get(channelName);
        if (node == null) {
            throw new ZLinkConfigurationException(
                "SPOT publisher client is not configured: " + channelName);
        }
        return node;
    }

    private boolean isClosed() {
        return inStateLane(() -> closed);
    }

    private <T> T inStateLane(java.util.function.Supplier<T> work) {
        try {
            return stateLane.runAsync(work).toCompletableFuture().join();
        } catch (CompletionException failure) {
            Throwable cause = failure.getCause();
            if (cause instanceof RuntimeException runtimeFailure) {
                throw runtimeFailure;
            }
            if (cause instanceof Error error) {
                throw error;
            }
            throw failure;
        }
    }

}

final class ZLinkDefaultSpotPublisherClient implements ZLinkSpotPublisherClient {
    private final ZLinkSpotPublisherRuntime publishers;

    ZLinkDefaultSpotPublisherClient(ZLinkSpotPublisherRuntime publishers) {
        this.publishers = publishers;
    }

    @Override
    public ZLinkPublishCall publish(
        String meshName,
        String channelName,
        String topic,
        Object message) {
        return publishers.publish(meshName, channelName, topic, message);
    }

    @Override
    public ZLinkPublishCall publish(
        String channelName,
        String topic,
        Object message) {
        return publishers.publish(channelName, topic, message);
    }
}

final class ZLinkExternalSpotPublishCall implements ZLinkPublishCall {
    private final AtomicBoolean submitGate;
    private final ZLinkSpotPublisherRuntime publishers;
    private final String meshName;
    private final String channelName;
    private final String topic;
    private final Message payload;
    private final Optional<String> packetName;
    private final String contentType;
    private final ZLinkApplicationMetadata metadata;

    ZLinkExternalSpotPublishCall(
        ZLinkSpotPublisherRuntime publishers,
        String meshName,
        String channelName,
        String topic,
        Message payload,
        Optional<String> packetName) {
        this(
            publishers,
            meshName,
            channelName,
            topic,
            payload,
            packetName,
            null,
            ZLinkApplicationMetadata.empty());
    }

    ZLinkExternalSpotPublishCall(
        ZLinkSpotPublisherRuntime publishers,
        String meshName,
        String channelName,
        String topic,
        Message payload,
        Optional<String> packetName,
        String contentType) {
        this(
            publishers,
            meshName,
            channelName,
            topic,
            payload,
            packetName,
            contentType,
            ZLinkApplicationMetadata.empty());
    }

    private ZLinkExternalSpotPublishCall(
        ZLinkSpotPublisherRuntime publishers,
        String meshName,
        String channelName,
        String topic,
        Message payload,
        Optional<String> packetName,
        String contentType,
        ZLinkApplicationMetadata metadata) {
        this(publishers, meshName, channelName, topic, payload, packetName, contentType,
            metadata, new AtomicBoolean());
    }

    private ZLinkExternalSpotPublishCall(
        ZLinkSpotPublisherRuntime publishers,
        String meshName,
        String channelName,
        String topic,
        Message payload,
        Optional<String> packetName,
        String contentType,
        ZLinkApplicationMetadata metadata,
        AtomicBoolean submitGate) {
        this.submitGate = submitGate;
        this.publishers = publishers;
        this.meshName = meshName;
        this.channelName = channelName;
        this.topic = topic;
        this.payload = payload;
        this.packetName = packetName;
        this.contentType = contentType;
        this.metadata = metadata;
    }

    public ZLinkPublishCall packetName(String packetName) {
        return new ZLinkExternalSpotPublishCall(
            publishers,
            meshName,
            channelName,
            topic,
            payload,
            Optional.of(packetName),
            contentType,
            metadata,
            submitGate);
    }

    @Override
    public ZLinkPublishCall metadata(String key, String value) {
        return new ZLinkExternalSpotPublishCall(
            publishers,
            meshName,
            channelName,
            topic,
            payload,
            packetName,
            contentType,
            metadata.with(key, value),
            submitGate);
    }

    @Override
    public ZLinkPublishCall metadata(Map<String, String> values) {
        return new ZLinkExternalSpotPublishCall(
            publishers,
            meshName,
            channelName,
            topic,
            payload,
            packetName,
            contentType,
            metadata.withAll(values),
            submitGate);
    }

    @Override
    public CompletionStage<Void> submit() {
        CompletionStage<Void> duplicate =
            ZLinkOneWayCalls.beginOneWay(submitGate);
        if (duplicate != null) {
            return duplicate;
        }
        return publishers.submitAsync(
                meshName, channelName, topic, payload, packetName, contentType, metadata)
            .thenCompose(result -> ZLinkOneWayCalls.oneWayStatus(result.status()));
    }
}

record ZLinkOneWayPublishAdmission(int status) {
}
