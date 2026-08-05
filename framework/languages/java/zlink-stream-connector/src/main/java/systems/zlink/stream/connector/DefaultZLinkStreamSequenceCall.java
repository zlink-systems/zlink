package systems.zlink.stream.connector;

import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Predicate;

final class DefaultZLinkStreamSequenceCall implements ZLinkStreamSequenceCall {
    private final ZLinkStreamConnector connector;
    private final String name;
    private final Duration timeout;
    private final ZLinkStreamTypedCodec codec;
    private final List<Predicate<ZLinkStreamMessage<ZLinkStreamEncodedPayload>>> predicates;

    DefaultZLinkStreamSequenceCall(
        ZLinkStreamConnector connector,
        String name,
        Duration timeout,
        ZLinkStreamTypedCodec codec) {
        this(connector, name, timeout, codec, List.of());
    }

    private DefaultZLinkStreamSequenceCall(
        ZLinkStreamConnector connector,
        String name,
        Duration timeout,
        ZLinkStreamTypedCodec codec,
        List<Predicate<ZLinkStreamMessage<ZLinkStreamEncodedPayload>>> predicates) {
        this.connector = Objects.requireNonNull(connector, "connector");
        this.name = Objects.requireNonNull(name, "name");
        this.timeout = Objects.requireNonNull(timeout, "timeout");
        this.codec = codec;
        this.predicates = List.copyOf(predicates);
    }

    @Override
    public ZLinkStreamSequenceCall expect(
        Predicate<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> predicate) {
        Objects.requireNonNull(predicate, "predicate");
        List<Predicate<ZLinkStreamMessage<ZLinkStreamEncodedPayload>>> next =
            new ArrayList<>(predicates);
        next.add(predicate);
        return new DefaultZLinkStreamSequenceCall(connector, name, timeout, codec, next);
    }

    @Override
    public <TPayload> ZLinkStreamSequenceCall expect(
        Class<TPayload> payloadType,
        Predicate<ZLinkStreamMessage<TPayload>> predicate) {
        Objects.requireNonNull(payloadType, "payloadType");
        Objects.requireNonNull(predicate, "predicate");
        if (codec == null) {
            throw new IllegalStateException(
                "typed stream payload API requires ZLinkStreamConnectorOptions.typedCodec");
        }
        return expect(message -> predicate.test(decodeMessage(message, payloadType)));
    }

    @Override
    public ZLinkStreamSequenceCall timeout(Duration timeout) {
        Objects.requireNonNull(timeout, "timeout");
        if (timeout.isNegative()) {
            throw new IllegalArgumentException("timeout must not be negative");
        }
        return new DefaultZLinkStreamSequenceCall(connector, name, timeout, codec, predicates);
    }

    @Override
    public CompletionStage<List<ZLinkStreamMessage<ZLinkStreamEncodedPayload>>> submit() {
        if (predicates.isEmpty()) {
            throw new IllegalStateException("waitForSequence requires at least one expectation");
        }
        if (connector instanceof DefaultZLinkStreamConnector concrete) {
            CompletableFuture<List<ZLinkStreamMessage<ZLinkStreamEncodedPayload>>> result =
                new CompletableFuture<>();
            List<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> messages = new ArrayList<>();
            Object sequenceLock = new Object();
            AtomicReference<CompletableFuture<ZLinkStreamMessage<ZLinkStreamEncodedPayload>>>
                currentWaiter = new AtomicReference<>();
            result.whenComplete((ignored, error) -> {
                if (!result.isCancelled()) {
                    return;
                }
                CompletableFuture<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> waiter;
                synchronized (sequenceLock) {
                    waiter = currentWaiter.getAndSet(null);
                    closeMessages(messages);
                }
                if (waiter != null) {
                    waiter.cancel(false);
                }
            });
            long deadline = System.nanoTime() + timeout.toNanos();
            awaitNext(concrete, result, messages, deadline, 0, sequenceLock, currentWaiter);
            return result;
        }
        CompletableFuture<List<ZLinkStreamMessage<ZLinkStreamEncodedPayload>>> result =
            new CompletableFuture<>();
        List<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> messages = new ArrayList<>();
        Object sequenceLock = new Object();
        AutoCloseable subscription = connector.on(name, message -> {
            synchronized (sequenceLock) {
                if (result.isDone()) {
                    message.payload().payload().close();
                } else try {
                    int current = messages.size();
                    if (!predicates.get(current).test(message)) {
                        message.payload().payload().close();
                        result.completeExceptionally(new IllegalStateException(
                            "Message '" + name + "' arrived out of the expected sequence."));
                    } else {
                        messages.add(message);
                        if (messages.size() == predicates.size()) {
                            result.complete(List.copyOf(messages));
                        }
                    }
                } catch (RuntimeException error) {
                    message.payload().payload().close();
                    result.completeExceptionally(error);
                }
            }
            return CompletableFuture.completedFuture(null);
        });
        result.whenComplete((ignored, error) -> {
            closeQuietly(subscription);
            if (error != null) {
                synchronized (sequenceLock) {
                    messages.forEach(item -> item.payload().payload().close());
                }
            }
        });
        return result.orTimeout(timeout.toMillis(), TimeUnit.MILLISECONDS);
    }

    private void awaitNext(
        DefaultZLinkStreamConnector concrete,
        CompletableFuture<List<ZLinkStreamMessage<ZLinkStreamEncodedPayload>>> result,
        List<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> messages,
        long deadline,
        int index,
        Object sequenceLock,
        AtomicReference<CompletableFuture<ZLinkStreamMessage<ZLinkStreamEncodedPayload>>>
            currentWaiter) {
        long remainingNanos = deadline - System.nanoTime();
        if (remainingNanos <= 0) {
            synchronized (sequenceLock) {
                if (!result.isCancelled()
                    && result.completeExceptionally(new java.util.concurrent.TimeoutException(
                        "Timed out waiting for '" + name + "' sequence."))) {
                    closeMessages(messages);
                }
            }
            return;
        }
        CompletableFuture<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> waiter = concrete
            .awaitMessage(name, predicates.get(index))
            .toCompletableFuture()
            .orTimeout(remainingNanos, TimeUnit.NANOSECONDS);
        currentWaiter.set(waiter);
        if (result.isCancelled() && currentWaiter.compareAndSet(waiter, null)) {
            waiter.cancel(false);
            return;
        }
        waiter
            .whenComplete((message, error) -> {
                boolean continueSequence = false;
                synchronized (sequenceLock) {
                    currentWaiter.compareAndSet(waiter, null);
                    if (error != null) {
                        if (result.completeExceptionally(error)) {
                            closeMessages(messages);
                        }
                    } else if (result.isCancelled()) {
                        closeMessage(message);
                    } else {
                        messages.add(message);
                        if (messages.size() == predicates.size()) {
                            result.complete(List.copyOf(messages));
                        } else {
                            continueSequence = true;
                        }
                    }
                }
                if (continueSequence) {
                    awaitNext(
                        concrete,
                        result,
                        messages,
                        deadline,
                        index + 1,
                        sequenceLock,
                        currentWaiter);
                }
            });
    }

    @Override
    public <TPayload> CompletionStage<List<ZLinkStreamMessage<TPayload>>> submit(
        Class<TPayload> payloadType) {
        Objects.requireNonNull(payloadType, "payloadType");
        if (codec == null) {
            throw new IllegalStateException(
                "typed stream payload API requires ZLinkStreamConnectorOptions.typedCodec");
        }
        CompletionStage<List<ZLinkStreamMessage<ZLinkStreamEncodedPayload>>> source = submit();
        CompletableFuture<List<ZLinkStreamMessage<TPayload>>> result = new CompletableFuture<>();
        source.whenComplete((messages, error) -> {
            if (error != null) {
                result.completeExceptionally(error);
                return;
            }
            List<ZLinkStreamMessage<TPayload>> decoded = new ArrayList<>();
            try {
                for (ZLinkStreamMessage<ZLinkStreamEncodedPayload> message : messages) {
                    decoded.add(decodeMessage(message, payloadType));
                }
                result.complete(List.copyOf(decoded));
            } catch (Throwable failure) {
                result.completeExceptionally(failure);
            } finally {
                messages.forEach(message -> message.payload().payload().close());
            }
        });
        result.whenComplete((ignored, error) -> {
            if (result.isCancelled()) {
                source.toCompletableFuture().cancel(false);
            }
        });
        return result;
    }

    private static void closeMessages(
        List<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> messages) {
        messages.forEach(DefaultZLinkStreamSequenceCall::closeMessage);
    }

    private static void closeMessage(
        ZLinkStreamMessage<ZLinkStreamEncodedPayload> message) {
        try {
            message.payload().payload().close();
        } catch (RuntimeException ignored) {
            // The cancelled sequence no longer owns a message that it cannot deliver.
        }
    }

    private <TPayload> ZLinkStreamMessage<TPayload> decodeMessage(
        ZLinkStreamMessage<ZLinkStreamEncodedPayload> message,
        Class<TPayload> payloadType) {
        return new ZLinkStreamMessage<>(
            message.packetName(),
            codec.decode(message.payload(), payloadType),
            message.metadata(),
            message.flowId(),
            message.flowOrigin());
    }

    private static void closeQuietly(AutoCloseable closeable) {
        try {
            closeable.close();
        } catch (Exception ignored) {
        }
    }
}
