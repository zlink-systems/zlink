package systems.zlink.framework.runtime.spots;

import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.channels.ZLinkPublishCall;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.ZLinkCompletionBridge;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;
import systems.zlink.framework.runtime.internal.channels.ZLinkChannelAdmissionTimeout;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.messaging.ZLinkApplicationMetadata;
import systems.zlink.framework.runtime.messaging.ZLinkPayloadEncoding;
import systems.zlink.framework.spots.ZLinkSpotPublisherClient;

import java.time.Duration;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.SynchronousQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.Function;
import java.util.logging.Level;
import java.util.logging.Logger;

final class ZLinkSpotPublisherRuntime implements AutoCloseable {
    private static final int MIN_MULTICAST_PARALLELISM = 2;
    private static final long WORKER_IDLE_SECONDS = 30;
    private static final Duration DEFAULT_ADMISSION_TIMEOUT = Duration.ofSeconds(1);
    private static final Logger LOGGER =
            Logger.getLogger(ZLinkSpotPublisherRuntime.class.getName());
    private final ZLinkMessageSerializer serializer;
    private final ZLinkSpotRouteMessages messages;
    private final ThreadPoolExecutor multicastExecutor;
    private final ThreadPoolExecutor multicastHandoffExecutor;
    private final Function<ZLinkBackendObject, Duration> admissionTimeout;
    private final Function<Class<?>, String> contentTypeResolver;
    private final ZLinkMessageFlowTracer flow;
    private final ZLinkStateLane stateLane = new ZLinkStateLane();
    private volatile Map<String, ZLinkInternalSpotNode> nodesByChannel = Map.of();
    private volatile boolean closed;

    ZLinkSpotPublisherRuntime(ZLinkMessageSerializer serializer, ZLinkSpotRouteMessages messages) {
        this(
                serializer,
                messages,
                Math.max(MIN_MULTICAST_PARALLELISM, Runtime.getRuntime().availableProcessors()),
                ignored -> DEFAULT_ADMISSION_TIMEOUT);
    }

    ZLinkSpotPublisherRuntime(
            ZLinkMessageSerializer serializer, ZLinkSpotRouteMessages messages, int parallelism) {
        this(serializer, messages, parallelism, ignored -> DEFAULT_ADMISSION_TIMEOUT);
    }

    ZLinkSpotPublisherRuntime(
            ZLinkMessageSerializer serializer,
            ZLinkSpotRouteMessages messages,
            int parallelism,
            Function<ZLinkBackendObject, Duration> admissionTimeout) {
        this(
                serializer,
                messages,
                parallelism,
                admissionTimeout,
                ignored ->
                        systems.zlink.framework.runtime.channels.ZLinkChannelContentTypeFrame
                                .DEFAULT_CONTENT_TYPE);
    }

    ZLinkSpotPublisherRuntime(
            ZLinkMessageSerializer serializer,
            ZLinkSpotRouteMessages messages,
            int parallelism,
            Function<ZLinkBackendObject, Duration> admissionTimeout,
            Function<Class<?>, String> contentTypeResolver) {
        this(serializer, messages, parallelism, admissionTimeout, contentTypeResolver, null);
    }

    ZLinkSpotPublisherRuntime(
            ZLinkMessageSerializer serializer,
            ZLinkSpotRouteMessages messages,
            int parallelism,
            Function<ZLinkBackendObject, Duration> admissionTimeout,
            Function<Class<?>, String> contentTypeResolver,
            ZLinkMessageFlowTracer flow) {
        this.serializer = serializer;
        this.messages = messages;
        this.flow = flow;
        this.admissionTimeout = Objects.requireNonNull(admissionTimeout, "admissionTimeout");
        this.contentTypeResolver =
                Objects.requireNonNull(contentTypeResolver, "contentTypeResolver");
        this.multicastExecutor =
                new ThreadPoolExecutor(
                        parallelism,
                        parallelism,
                        WORKER_IDLE_SECONDS,
                        TimeUnit.SECONDS,
                        new SynchronousQueue<>(),
                        runnable -> {
                            Thread thread = new Thread(runnable, "zlink-logical-multicast");
                            thread.setDaemon(true);
                            return thread;
                        },
                        new ThreadPoolExecutor.AbortPolicy());
        this.multicastExecutor.allowCoreThreadTimeOut(true);
        this.multicastHandoffExecutor =
                new ThreadPoolExecutor(
                        1,
                        1,
                        WORKER_IDLE_SECONDS,
                        TimeUnit.SECONDS,
                        new SynchronousQueue<>(),
                        runnable -> {
                            Thread thread =
                                    new Thread(runnable, "zlink-logical-multicast-admission");
                            thread.setDaemon(true);
                            return thread;
                        },
                        new ThreadPoolExecutor.AbortPolicy());
        this.multicastHandoffExecutor.allowCoreThreadTimeOut(true);
    }

    void register(String channelName, ZLinkInternalSpotNode node) {
        inStateLane(
                () -> {
                    Map<String, ZLinkInternalSpotNode> configured = new HashMap<>(nodesByChannel);
                    configured.put(channelName, node);
                    nodesByChannel = Map.copyOf(configured);
                    return null;
                });
    }

    boolean contains(String channelName) {
        return nodesByChannel.containsKey(channelName);
    }

    ZLinkSpotPublisherClient client() {
        return new ZLinkDefaultSpotPublisherClient(this);
    }

    ZLinkPublishCall publish(String channelName, String topic, Object message) {
        return publish(channelName, channelName, topic, message);
    }

    ZLinkPublishCall publish(String meshName, String channelName, String topic, Object message) {
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
     * R1 value-passing (spec 27 §4): the publish flow — the ambient callback flow or a new
     * APPLICATION flow for a first outbound started outside framework callbacks — is captured as a
     * value on the submitting thread and handed to the encoder explicitly. The multicast executor
     * hop receives the value and no flow scope is installed. The terminal layer separately restores
     * an active serial turn only when admission is pending. At Off nothing is captured or
     * allocated.
     */
    private ZLinkFlowContext.State captureOutboundFlow() {
        if (flow == null || !flow.captureEnabled()) {
            return null;
        }
        ZLinkFlowContext.State current = ZLinkFlowContext.current();
        return current != null ? current : ZLinkFlowContext.create(ZLinkFlowOrigin.APPLICATION);
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
        List<Message> parts =
                messages.encodePublish(
                        channelName,
                        topic,
                        packetName,
                        payload,
                        contentType,
                        metadata.values(),
                        flowState);
        try {
            requireChannel(meshName)
                    .publish(channelName, topic, metadata.encode(), parts, SendFlags.DONT_WAIT);
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
            result.completeRejected(new ZLinkOneWayPublishAdmission(ZLinkOneWayCalls.SHUTDOWN));
            return result;
        }
        //  Captured on the submitting thread; the executor hop below would
        //  otherwise lose the ambient flow (R1 value-passing).
        ZLinkFlowContext.State flowState = captureOutboundFlow();
        MulticastTask operation =
                new MulticastTask(
                        result,
                        () ->
                                executeMulticast(
                                        meshName,
                                        channelName,
                                        topic,
                                        payload,
                                        packetName,
                                        contentType,
                                        metadata,
                                        flowState,
                                        result));
        try {
            multicastExecutor.execute(operation);
        } catch (RejectedExecutionException rejected) {
            if (isClosed() || multicastExecutor.isShutdown()) {
                result.completeRejected(emptyAdmission(ZLinkOneWayCalls.SHUTDOWN));
                return result;
            }
            int timeoutMillis =
                    ZLinkChannelAdmissionTimeout.normalizedMillis(
                            admissionTimeout.apply(requireChannel(meshName)));
            try {
                multicastHandoffExecutor.execute(
                        new MulticastTask(
                                result,
                                () -> awaitExecutorAdmission(operation, result, timeoutMillis)));
            } catch (RejectedExecutionException capacityExhausted) {
                result.completeRejected(
                        emptyAdmission(
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
            MulticastFuture result) {
        if (!result.beginCommit()) return;
        try {
            submitNow(
                    meshName,
                    channelName,
                    topic,
                    payload,
                    packetName,
                    contentType,
                    metadata,
                    flowState);
        } catch (RuntimeException error) {
            LOGGER.log(
                    Level.WARNING,
                    "Logical Multicast target processing failed after source-local admission.",
                    error);
        } finally {
            result.closePayload();
        }
    }

    CompletionStage<ZLinkOneWayPublishAdmission> submitAsync(
            String meshName,
            String channelName,
            String topic,
            Message payload,
            Optional<String> packetName,
            ZLinkApplicationMetadata metadata) {
        return submitAsync(meshName, channelName, topic, payload, packetName, null, metadata);
    }

    private void awaitExecutorAdmission(
            Runnable operation, MulticastFuture result, int timeoutMillis) {
        try {
            if (multicastExecutor
                    .getQueue()
                    .offer(operation, timeoutMillis, TimeUnit.MILLISECONDS)) {
                if ((isClosed() || multicastExecutor.isShutdown())
                        && multicastExecutor.getQueue().remove(operation)) {
                    result.completeRejected(emptyAdmission(ZLinkOneWayCalls.SHUTDOWN));
                }
                return;
            }
            result.completeRejected(
                    emptyAdmission(
                            isClosed() || multicastExecutor.isShutdown()
                                    ? ZLinkOneWayCalls.SHUTDOWN
                                    : ZLinkOneWayCalls.TIMED_OUT));
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            result.completeRejected(
                    emptyAdmission(
                            isClosed() || multicastExecutor.isShutdown()
                                    ? ZLinkOneWayCalls.SHUTDOWN
                                    : ZLinkOneWayCalls.TIMED_OUT));
        }
    }

    private static ZLinkOneWayPublishAdmission emptyAdmission(int status) {
        return new ZLinkOneWayPublishAdmission(status);
    }

    @Override
    public void close() {
        boolean started =
                inStateLane(
                        () -> {
                            if (closed) return false;
                            closed = true;
                            return true;
                        });
        if (!started) return;
        multicastExecutor.shutdownNow().forEach(ZLinkSpotPublisherRuntime::rejectDropped);
        multicastHandoffExecutor.shutdownNow().forEach(ZLinkSpotPublisherRuntime::rejectDropped);
    }

    private static void rejectDropped(Runnable task) {
        ((MulticastTask) task).rejectShutdown();
    }

    private final class MulticastTask implements Runnable {
        private final MulticastFuture result;
        private final Runnable action;

        private MulticastTask(MulticastFuture result, Runnable action) {
            this.result = result;
            this.action = action;
        }

        @Override
        public void run() {
            if (result.isDone()) return;
            if (isClosed() || multicastExecutor.isShutdown()) rejectShutdown();
            else action.run();
        }

        private void rejectShutdown() {
            result.completeRejected(emptyAdmission(ZLinkOneWayCalls.SHUTDOWN));
        }
    }

    private static final class MulticastFuture
            extends CompletableFuture<ZLinkOneWayPublishAdmission> {
        private final Message payload;

        MulticastFuture(Message payload) {
            this.payload = payload;
        }

        boolean beginCommit() {
            return super.complete(new ZLinkOneWayPublishAdmission(ZLinkOneWayCalls.SUBMITTED));
        }

        @Override
        public boolean cancel(boolean mayInterruptIfRunning) {
            boolean won =
                    super.completeExceptionally(new java.util.concurrent.CancellationException());
            if (won) closePayload();
            return won;
        }

        void closePayload() {
            try {
                payload.close();
            } catch (RuntimeException failure) {
                LOGGER.log(Level.WARNING, "Logical Multicast payload cleanup failed", failure);
            }
        }

        void completeRejected(ZLinkOneWayPublishAdmission value) {
            if (super.complete(value)) closePayload();
        }
    }

    private ZLinkInternalSpotNode requireChannel(String channelName) {
        ZLinkInternalSpotNode node = nodesByChannel.get(channelName);
        if (node == null) {
            throw new ZLinkConfigurationException(
                    "SPOT publisher client is not configured: " + channelName);
        }
        return node;
    }

    private boolean isClosed() {
        return closed;
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
            String meshName, String channelName, String topic, Object message) {
        return publishers.publish(meshName, channelName, topic, message);
    }

    @Override
    public ZLinkPublishCall publish(String channelName, String topic, Object message) {
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
        this(
                publishers,
                meshName,
                channelName,
                topic,
                payload,
                packetName,
                contentType,
                metadata,
                new AtomicBoolean());
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
        CompletionStage<Void> duplicate = ZLinkOneWayCalls.beginOneWay(submitGate);
        if (duplicate != null) {
            return duplicate;
        }
        CompletionStage<ZLinkOneWayPublishAdmission> source =
                publishers.submitAsync(
                        meshName, channelName, topic, payload, packetName, contentType, metadata);
        CompletableFuture<Void> result =
                source.thenCompose(admission -> ZLinkOneWayCalls.oneWayStatus(admission.status()))
                        .toCompletableFuture();
        if (result.isDone()) return result;
        ZLinkCompletionBridge.forwardCancellation(result, source);
        return ZLinkSerialExecutionQueue.manageCurrent(result);
    }
}

record ZLinkOneWayPublishAdmission(int status) {}
