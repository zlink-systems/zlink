package systems.zlink.framework.runtime.actors;
import java.util.Comparator;
import java.util.Objects;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.function.Supplier;

import java.time.Duration;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;
import java.util.function.Consumer;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceMessageFollowWireCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddress;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

/** Owns the transfer-only backlog and bounded source-retirement state. */
final class ZLinkActorTransferHandoff implements AutoCloseable {
    private final ScheduledExecutorService retirementsExecutor =
        Executors.newSingleThreadScheduledExecutor(runnable -> {
            Thread thread = new Thread(runnable, "zlink-actor-transfer-retirement");
            thread.setDaemon(true);
            return thread;
        });

    private final Map<String, Backlog> backlogs =
        new ConcurrentHashMap<>();
    private final AtomicLong arrivalIndex = new AtomicLong();
    private final Map<String, MessageFollowSource> messageFollowSources =
        new ConcurrentHashMap<>();
    private final Set<Retention> retirements = ConcurrentHashMap.newKeySet();
    private final AtomicLong messageFollowToken = new AtomicLong();
    private final ZLinkMessageFollowSuppressionRegistry messageFollowSuppression =
        new ZLinkMessageFollowSuppressionRegistry();
    private boolean closed;

    synchronized void begin(String actorId) {
        requireOpen();
        backlogs.put(actorId, new Backlog());
    }

    ZLinkActorHandoffPacket capture(
        String actorId,
        ZLinkStreamHeader header,
        Message payload,
        ZLinkActorReplyRoute replyRoute,
        byte[] acceptedJournalRecord) {
        Backlog backlog = backlogs.get(actorId);
        if (backlog == null) {
            return null;
        }
        ZLinkActorHandoffPacket packet =
            new ZLinkActorHandoffPacket(
                arrivalIndex.incrementAndGet(), header, payload, replyRoute,
                acceptedJournalRecord);
        synchronized (backlog) {
            if (backlogs.get(actorId) != backlog) {
                packet.close();
                return null;
            }
            long bytes = packet.retainedBytes();
            backlog.packets.add(packet);
            backlog.bytes += bytes;
        }
        return packet;
    }

    List<ZLinkActorHandoffPacket> take(String actorId) {
        Backlog backlog = backlogs.get(actorId);
        if (backlog == null) {
            return List.of();
        }
        synchronized (backlog) {
            List<ZLinkActorHandoffPacket> snapshot = List.copyOf(backlog.packets);
            backlog.packets.clear();
            backlog.bytes = 0;
            return snapshot;
        }
    }

    int pendingCount(String actorId) {
        Backlog backlog = backlogs.get(actorId);
        if (backlog == null) {
            return 0;
        }
        synchronized (backlog) {
            return backlog.packets.size();
        }
    }

    List<ZLinkActorHandoffPacket> finish(String actorId) {
        Backlog backlog = backlogs.remove(actorId);
        if (backlog == null) {
            return List.of();
        }
        synchronized (backlog) {
            return List.copyOf(backlog.packets);
        }
    }

    /**
     * Removes the transfer hold while retaining the packets for a source-side
     * serial replay. Packets captured concurrently with this operation either
     * join the returned snapshot or observe the removed hold and wait for the
     * move completion fence.
     */
    List<ZLinkActorHandoffPacket> takeForRestore(
        String actorId,
        List<ZLinkActorHandoffPacket> committed) {
        Backlog backlog = backlogs.get(actorId);
        if (backlog == null) {
            return committed == null || committed.isEmpty()
                ? List.of()
                : List.copyOf(committed);
        }
        synchronized (backlog) {
            List<ZLinkActorHandoffPacket> restored = new ArrayList<>(
                (committed == null ? 0 : committed.size()) + backlog.packets.size());
            if (committed != null) {
                restored.addAll(committed);
            }
            restored.addAll(backlog.packets);
            if (!backlogs.remove(actorId, backlog)) {
                throw new IllegalStateException(
                    "Actor transfer hold changed during source restore: " + actorId);
            }
            backlog.packets.clear();
            backlog.bytes = 0;
            restored.sort(Comparator.comparingLong(
                ZLinkActorHandoffPacket::arrivalIndex));
            return List.copyOf(restored);
        }
    }

    void fail(String actorId, Throwable error) {
        Backlog backlog = backlogs.remove(actorId);
        if (backlog == null) {
            return;
        }
        synchronized (backlog) {
            backlog.packets.forEach(packet -> {
                if (packet.fail(error)) {
                    packet.close();
                }
            });
        }
    }

    synchronized void retain(
        String actorId,
        ZLinkBackendActorRef sourceActorRef,
        ZLinkBackendActorRef targetActorRef,
        Duration duration,
        Consumer<MessageFollowSource> removal) {
        retain(actorId, sourceActorRef, targetActorRef, null, duration, removal);
    }

    synchronized void retain(
        String actorId,
        ZLinkBackendActorRef sourceActorRef,
        ZLinkBackendActorRef targetActorRef,
        SpotTransportAddress targetAddress,
        Duration duration,
        Consumer<MessageFollowSource> removal) {
        retain(
            actorId,
            sourceActorRef,
            targetActorRef,
            targetAddress,
            null,
            null,
            duration,
            removal);
    }

    synchronized void retain(
        String actorId,
        ZLinkBackendActorRef sourceActorRef,
        ZLinkBackendActorRef targetActorRef,
        SpotTransportAddress targetAddress,
        ZLinkServiceMessageFollowWireCodec.ActorRoute sourceRoute,
        ZLinkServiceMessageFollowWireCodec.ActorRoute targetRoute,
        Duration duration,
        Consumer<MessageFollowSource> removal) {
        requireOpen();
        if (targetAddress != null
            && (sourceRoute == null || targetRoute == null)) {
            throw new IllegalArgumentException(
                "exact source and target routes are required when a Message Follow target address is set");
        }
        MessageFollowSource source = new MessageFollowSource(
            sourceActorRef, targetActorRef, targetAddress,
            sourceRoute, targetRoute,
            messageFollowToken.incrementAndGet(), messageFollowSuppression);
        MessageFollowSource replaced = messageFollowSources.put(actorId, source);
        if (replaced != null) {
            replaced.expireMessageFollowNotices();
        }
        Retention retained = new Retention(actorId, source, removal);
        retirements.add(retained);
        ScheduledFuture<?> future = retirementsExecutor.schedule(
            () -> retire(retained),
            duration.toMillis(),
            TimeUnit.MILLISECONDS);
        retained.future(future);
    }

    Optional<MessageFollowSource> messageFollowSource(String actorId) {
        return Optional.ofNullable(messageFollowSources.get(actorId));
    }

    synchronized Optional<MessageFollowSource> takeMessageFollowSource(String actorId) {
        MessageFollowSource source = messageFollowSources.remove(actorId);
        if (source == null) {
            return Optional.empty();
        }
        source.expireMessageFollowNotices();
        Retention retained = retirements.stream()
            .filter(candidate -> candidate.actorId().equals(actorId)
                && candidate.source().equals(source))
            .findFirst()
            .orElse(null);
        if (retained != null && retirements.remove(retained)) {
            ScheduledFuture<?> future = retained.future();
            if (future != null) {
                future.cancel(false);
            }
        }
        return Optional.of(source);
    }

    int messageFollowSourceCount() {
        return messageFollowSources.size();
    }

    int messageFollowSuppressionCount() {
        return messageFollowSuppression.size();
    }

    <T> CompletionStage<T> follow(
        String actorId,
        long objectGeneration,
        long payloadBytes,
        Supplier<CompletionStage<T>> submission) {
        return followWithQueueSnapshot(
                actorId, objectGeneration, payloadBytes, submission)
            .thenApply(FollowResult::value);
    }

    <T> CompletionStage<FollowResult<T>> followWithQueueSnapshot(
        String actorId,
        long objectGeneration,
        long payloadBytes,
        Supplier<CompletionStage<T>> submission) {
        MessageFollowSource source = messageFollowSources.get(actorId);
        if (source == null) {
            return CompletableFuture.failedFuture(
                new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.UNAVAILABLE,
                    "committed Message Follow route is unavailable"));
        }
        if (source.sourceActorRef().generation() != objectGeneration
            || source.targetActorRef().generation() != objectGeneration) {
            return CompletableFuture.failedFuture(
                new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.INVALID_OPERATION,
                    "committed Message Follow generation does not match"));
        }
        if (!source.tryAcquire(payloadBytes)) {
            return CompletableFuture.failedFuture(
                new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.INVALID_OPERATION,
                    "committed Message Follow payload size is invalid"));
        }
        MessageFollowQueueSnapshot queueSnapshot = source.queueSnapshot();
        CompletionStage<T> submitted;
        try {
            submitted = Objects.requireNonNull(
                submission.get(), "Message Follow submission returned null");
        } catch (RuntimeException failure) {
            source.release(payloadBytes);
            return CompletableFuture.failedFuture(failure);
        }
        return submitted
            .thenApply(value -> new FollowResult<>(value, queueSnapshot))
            .whenComplete((ignored, failure) -> source.release(payloadBytes));
    }

    @Override
    public synchronized void close() {
        if (closed) {
            return;
        }
        closed = true;
        retirementsExecutor.shutdownNow();
        List.copyOf(retirements).forEach(retained -> {
            ScheduledFuture<?> future = retained.future();
            if (future != null) {
                future.cancel(false);
            }
            try {
                retire(retained);
            } catch (RuntimeException ignored) {
                // Runtime shutdown must continue retiring the remaining owned sources.
            }
        });
        backlogs.keySet().forEach(actorId -> fail(
            actorId, new IllegalStateException("Actor runtime closed during transfer.")));
    }

    private void retire(Retention retained) {
        if (!retirements.remove(retained)) {
            return;
        }
        messageFollowSources.remove(retained.actorId(), retained.source());
        retained.source().expireMessageFollowNotices();
        retained.removal().accept(retained.source());
    }

    private void requireOpen() {
        if (closed) {
            throw new IllegalStateException("Actor transfer handoff is closed.");
        }
    }

    private static final class Retention {
        private final String actorId;
        private final MessageFollowSource source;
        private final Consumer<MessageFollowSource> removal;
        private volatile ScheduledFuture<?> future;

        private Retention(
            String actorId,
            MessageFollowSource source,
            Consumer<MessageFollowSource> removal) {
            this.actorId = actorId;
            this.source = source;
            this.removal = removal;
        }

        String actorId() {
            return actorId;
        }

        MessageFollowSource source() {
            return source;
        }

        Consumer<MessageFollowSource> removal() {
            return removal;
        }

        ScheduledFuture<?> future() {
            return future;
        }

        void future(ScheduledFuture<?> future) {
            this.future = future;
        }
    }

    private static final class Backlog {
        private final List<ZLinkActorHandoffPacket> packets = new ArrayList<>();
        private long bytes;
    }

    static final class MessageFollowSource {
        private final ZLinkBackendActorRef sourceActorRef;
        private final ZLinkBackendActorRef targetActorRef;
        private final SpotTransportAddress targetAddress;
        private final ZLinkServiceMessageFollowWireCodec.ActorRoute sourceRoute;
        private final ZLinkServiceMessageFollowWireCodec.ActorRoute targetRoute;
        private final long token;
        private final ZLinkMessageFollowSuppressionRegistry suppression;
        private final Set<ZLinkMessageFollowSuppressionRegistry.Key> suppressionKeys =
            ConcurrentHashMap.newKeySet();
        private boolean suppressionExpired;
        private int pendingMessages;
        private long pendingBytes;

        private MessageFollowSource(
            ZLinkBackendActorRef sourceActorRef,
            ZLinkBackendActorRef targetActorRef,
            SpotTransportAddress targetAddress,
            ZLinkServiceMessageFollowWireCodec.ActorRoute sourceRoute,
            ZLinkServiceMessageFollowWireCodec.ActorRoute targetRoute,
            long token,
            ZLinkMessageFollowSuppressionRegistry suppression) {
            this.sourceActorRef = Objects.requireNonNull(
                sourceActorRef, "sourceActorRef");
            this.targetActorRef = Objects.requireNonNull(
                targetActorRef, "targetActorRef");
            this.targetAddress = targetAddress;
            this.sourceRoute = sourceRoute;
            this.targetRoute = targetRoute;
            this.token = token;
            this.suppression = Objects.requireNonNull(suppression, "suppression");
        }

        ZLinkBackendActorRef sourceActorRef() { return sourceActorRef; }
        ZLinkBackendActorRef targetActorRef() { return targetActorRef; }
        SpotTransportAddress targetAddress() { return targetAddress; }
        ZLinkServiceMessageFollowWireCodec.ActorRoute sourceRoute() {
            return sourceRoute;
        }
        ZLinkServiceMessageFollowWireCodec.ActorRoute targetRoute() {
            return targetRoute;
        }
        long token() { return token; }

        boolean matchesSourceRoute(
            ZLinkServiceMessageFollowWireCodec.ActorRoute candidate) {
            return sourceRoute != null && sourceRoute.equals(candidate);
        }

        synchronized Optional<ZLinkMessageFollowSuppressionRegistry.Claim>
            beginMessageFollowNotice(
                ZLinkServiceMessageFollowWireCodec.ActorRoute sourceRoute) {
            if (!matchesSourceRoute(sourceRoute)
                || targetRoute == null
                || suppressionExpired) {
                return Optional.empty();
            }
            ZLinkMessageFollowSuppressionRegistry.Key key =
                ZLinkMessageFollowSuppressionRegistry.Key.actor(sourceRoute, targetRoute);
            suppressionKeys.add(key);
            return suppression.begin(key);
        }

        void markMessageFollowNoticeSent(
            ZLinkMessageFollowSuppressionRegistry.Claim claim) {
            suppression.markSent(claim);
        }

        void abortMessageFollowNotice(
            ZLinkMessageFollowSuppressionRegistry.Claim claim) {
            suppression.abort(claim);
        }

        synchronized void expireMessageFollowNotices() {
            suppressionExpired = true;
            suppressionKeys.forEach(suppression::expire);
            suppressionKeys.clear();
        }

        private synchronized boolean tryAcquire(long bytes) {
            if (bytes < 0) {
                return false;
            }
            pendingMessages++;
            pendingBytes += bytes;
            return true;
        }

        private synchronized void release(long bytes) {
            pendingMessages--;
            pendingBytes -= bytes;
        }

        synchronized int pendingMessages() { return pendingMessages; }
        synchronized long pendingBytes() { return pendingBytes; }

        synchronized MessageFollowQueueSnapshot queueSnapshot() {
            return new MessageFollowQueueSnapshot(pendingMessages, pendingBytes);
        }
    }

    record MessageFollowQueueSnapshot(long messages, long bytes) {
        MessageFollowQueueSnapshot {
            if (messages < 0 || bytes < 0) {
                throw new IllegalArgumentException(
                    "Message Follow queue diagnostics must be non-negative");
            }
        }
    }

    record FollowResult<T>(T value, MessageFollowQueueSnapshot queue) {
    }
}
