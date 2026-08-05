package systems.zlink.framework.runtime.spots;

import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;

import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
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
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
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
    private final Function<systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject,
        Duration> admissionTimeout;
    private final Function<Class<?>, String> contentTypeResolver;
    private final Map<String, ZLinkInternalSpotNode> nodesByChannel = new HashMap<>();
    private final Map<String, ZLinkBackendSpot> spotsByChannel = new HashMap<>();
    private volatile boolean closed;

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
        Function<systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject,
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
        Function<systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject,
            Duration> admissionTimeout,
        Function<Class<?>, String> contentTypeResolver) {
        this.serializer = serializer;
        this.messages = messages;
        this.admissionTimeout = java.util.Objects.requireNonNull(
            admissionTimeout, "admissionTimeout");
        this.contentTypeResolver = java.util.Objects.requireNonNull(
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
        nodesByChannel.put(channelName, node);
    }

    boolean contains(String channelName) {
        return nodesByChannel.containsKey(channelName);
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
                contentTypeResolver.apply(message == null ? null : message.getClass()));
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

    void submitNow(
        String meshName,
        String channelName,
        String topic,
        Message payload,
        Optional<String> packetName,
        String contentType,
        ZLinkApplicationMetadata metadata) {
        List<Message> parts = messages.encode(packetName, payload, contentType);
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

    java.util.concurrent.CompletionStage<ZLinkOneWayPublishAdmission> submitAsync(
        String meshName,
        String channelName,
        String topic,
        Message payload,
        Optional<String> packetName,
        String contentType,
        ZLinkApplicationMetadata metadata) {
        MulticastFuture result = new MulticastFuture(payload);
        if (closed) {
            result.closePayloadOnce();
            result.completeRejected(new ZLinkOneWayPublishAdmission(
                ZLinkOneWayCalls.SHUTDOWN));
            return result;
        }
        Runnable operation = () -> {
                if (!result.beginCommit()) {
                    result.closePayloadOnce();
                    return;
                }
                result.completeCommitted(new ZLinkOneWayPublishAdmission(
                    ZLinkOneWayCalls.SUBMITTED));
                try {
                    submitNow(
                        meshName,
                        channelName,
                        topic,
                        payload,
                        packetName,
                        contentType,
                        metadata);
                } catch (RuntimeException error) {
                    LOGGER.log(
                        Level.WARNING,
                        "Logical Multicast target processing failed after source-local admission.",
                        error);
                } finally {
                    result.closePayloadOnce();
                }
            };
        try {
            multicastExecutor.execute(operation);
        } catch (RejectedExecutionException rejected) {
            if (closed || multicastExecutor.isShutdown()) {
                result.closePayloadOnce();
                result.completeRejected(emptyAdmission(ZLinkOneWayCalls.SHUTDOWN));
                return result;
            }
            int timeoutMillis = normalizedTimeoutMillis(
                admissionTimeout.apply(requireChannel(meshName)));
            try {
                multicastHandoffExecutor.execute(() -> awaitExecutorAdmission(
                    operation, result, timeoutMillis));
            } catch (RejectedExecutionException capacityExhausted) {
                result.closePayloadOnce();
                result.completeRejected(emptyAdmission(
                    closed || multicastExecutor.isShutdown()
                        ? ZLinkOneWayCalls.SHUTDOWN
                        : ZLinkOneWayCalls.TIMED_OUT));
            }
        }
        return result;
    }

    java.util.concurrent.CompletionStage<ZLinkOneWayPublishAdmission> submitAsync(
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
                closed || multicastExecutor.isShutdown()
                    ? ZLinkOneWayCalls.SHUTDOWN
                    : ZLinkOneWayCalls.TIMED_OUT));
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            result.closePayloadOnce();
            result.completeRejected(emptyAdmission(
                closed || multicastExecutor.isShutdown()
                    ? ZLinkOneWayCalls.SHUTDOWN
                    : ZLinkOneWayCalls.TIMED_OUT));
        }
    }

    private static ZLinkOneWayPublishAdmission emptyAdmission(
        int status) {
        return new ZLinkOneWayPublishAdmission(status);
    }

    private static int normalizedTimeoutMillis(java.time.Duration timeout) {
        if (timeout == null || timeout.isZero() || timeout.isNegative()) {
            throw new IllegalArgumentException("send timeout must be positive");
        }
        long millis = timeout.toMillis();
        if (timeout.compareTo(java.time.Duration.ofMillis(millis)) > 0) {
            millis++;
        }
        if (millis < 1L || millis > Integer.MAX_VALUE) {
            throw new IllegalArgumentException(
                "send timeout must normalize to 1..Integer.MAX_VALUE ms");
        }
        return (int) millis;
    }

    @Override
    public synchronized void close() {
        closed = true;
        RuntimeException firstFailure = null;
        for (ZLinkBackendSpot spot : spotsByChannel.values()) {
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
        spotsByChannel.clear();
        multicastExecutor.shutdownNow();
        multicastHandoffExecutor.shutdownNow();
        if (firstFailure != null) {
            throw firstFailure;
        }
    }

    private static final class MulticastFuture
        extends CompletableFuture<ZLinkOneWayPublishAdmission> {
        private final Message payload;
        private boolean committed;
        private boolean payloadReleased;

        MulticastFuture(Message payload) {
            this.payload = payload;
        }

        synchronized boolean beginCommit() {
            if (isCancelled()) {
                return false;
            }
            committed = true;
            return true;
        }

        @Override
        public synchronized boolean cancel(boolean mayInterruptIfRunning) {
            if (committed) {
                return false;
            }
            boolean cancelled = super.cancel(false);
            if (cancelled) {
                closePayloadOnce();
            }
            return cancelled;
        }

        synchronized void closePayloadOnce() {
            if (payloadReleased) {
                return;
            }
            payloadReleased = true;
            payload.close();
        }

        synchronized boolean completeCommitted(ZLinkOneWayPublishAdmission value) {
            return super.complete(value);
        }

        synchronized boolean completeRejected(ZLinkOneWayPublishAdmission value) {
            return super.complete(value);
        }
    }

    private synchronized ZLinkBackendSpot publisherSpot(String channelName) {
        if (closed) {
            throw new ZLinkConfigurationException("SPOT publisher runtime is closed");
        }
        ZLinkInternalSpotNode node = requireChannel(channelName);
        return spotsByChannel.computeIfAbsent(channelName, ignored -> node.createSpot());
    }

    private ZLinkInternalSpotNode requireChannel(String channelName) {
        ZLinkInternalSpotNode node = nodesByChannel.get(channelName);
        if (node == null) {
            throw new ZLinkConfigurationException(
                "SPOT publisher client is not configured: " + channelName);
        }
        return node;
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
    private final java.util.concurrent.atomic.AtomicBoolean submitGate =
        new java.util.concurrent.atomic.AtomicBoolean();
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
            metadata);
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
            metadata.with(key, value));
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
            metadata.withAll(values));
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> submit() {
        java.util.concurrent.CompletionStage<Void> duplicate =
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
