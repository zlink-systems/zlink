package systems.zlink.framework.runtime.actors;

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
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.Consumer;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceMessageFollowWireCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddress;

/** Owns the transfer-only backlog and bounded source-retirement state. */
final class ZLinkActorTransferHandoff implements AutoCloseable {
    static final int MAX_MESSAGE_FOLLOW_MESSAGES = 1024;
    static final long MAX_MESSAGE_FOLLOW_BYTES = 16L * 1024L * 1024L;
    private final ScheduledExecutorService retirementsExecutor =
        Executors.newSingleThreadScheduledExecutor(runnable -> {
            Thread thread = new Thread(runnable, "zlink-actor-transfer-retirement");
            thread.setDaemon(true);
            return thread;
        });

    private final Map<String, List<ZLinkActorHandoffPacket>> backlogs =
        new ConcurrentHashMap<>();
    private final AtomicLong arrivalIndex = new AtomicLong();
    private final Map<String, MessageFollowSource> messageFollowSources =
        new ConcurrentHashMap<>();
    private final java.util.Set<Retention> retirements = ConcurrentHashMap.newKeySet();
    private final AtomicLong messageFollowToken = new AtomicLong();
    private boolean closed;

    synchronized void begin(String actorId) {
        requireOpen();
        backlogs.put(actorId, Collections.synchronizedList(new ArrayList<>()));
    }

    ZLinkActorHandoffPacket capture(
        String actorId,
        ZLinkStreamHeader header,
        Message payload,
        ZLinkActorReplyRoute replyRoute,
        byte[] acceptedJournalRecord) {
        List<ZLinkActorHandoffPacket> backlog = backlogs.get(actorId);
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
            backlog.add(packet);
        }
        return packet;
    }

    List<ZLinkActorHandoffPacket> take(String actorId) {
        List<ZLinkActorHandoffPacket> backlog = backlogs.get(actorId);
        if (backlog == null) {
            return List.of();
        }
        synchronized (backlog) {
            List<ZLinkActorHandoffPacket> snapshot = List.copyOf(backlog);
            backlog.clear();
            return snapshot;
        }
    }

    int pendingCount(String actorId) {
        List<ZLinkActorHandoffPacket> backlog = backlogs.get(actorId);
        if (backlog == null) {
            return 0;
        }
        synchronized (backlog) {
            return backlog.size();
        }
    }

    List<ZLinkActorHandoffPacket> finish(String actorId) {
        List<ZLinkActorHandoffPacket> backlog = backlogs.remove(actorId);
        if (backlog == null) {
            return List.of();
        }
        synchronized (backlog) {
            return List.copyOf(backlog);
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
        List<ZLinkActorHandoffPacket> backlog = backlogs.get(actorId);
        if (backlog == null) {
            return committed == null || committed.isEmpty()
                ? List.of()
                : List.copyOf(committed);
        }
        synchronized (backlog) {
            List<ZLinkActorHandoffPacket> restored = new ArrayList<>(
                (committed == null ? 0 : committed.size()) + backlog.size());
            if (committed != null) {
                restored.addAll(committed);
            }
            restored.addAll(backlog);
            if (!backlogs.remove(actorId, backlog)) {
                throw new IllegalStateException(
                    "Actor transfer hold changed during source restore: " + actorId);
            }
            backlog.clear();
            restored.sort(java.util.Comparator.comparingLong(
                ZLinkActorHandoffPacket::arrivalIndex));
            return List.copyOf(restored);
        }
    }

    void fail(String actorId, Throwable error) {
        List<ZLinkActorHandoffPacket> backlog = backlogs.remove(actorId);
        if (backlog == null) {
            return;
        }
        synchronized (backlog) {
            backlog.forEach(packet -> {
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
            duration,
            removal);
    }

    synchronized void retain(
        String actorId,
        ZLinkBackendActorRef sourceActorRef,
        ZLinkBackendActorRef targetActorRef,
        SpotTransportAddress targetAddress,
        ZLinkServiceMessageFollowWireCodec.ActorRoute targetRoute,
        Duration duration,
        Consumer<MessageFollowSource> removal) {
        requireOpen();
        if (targetAddress != null && targetRoute == null) {
            throw new IllegalArgumentException(
                "targetRoute is required when a Message Follow target address is set");
        }
        MessageFollowSource source = new MessageFollowSource(
            sourceActorRef, targetActorRef, targetAddress, targetRoute,
            messageFollowToken.incrementAndGet());
        messageFollowSources.put(actorId, source);
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

    <T> CompletionStage<T> follow(
        String actorId,
        long objectGeneration,
        long payloadBytes,
        java.util.function.Supplier<CompletionStage<T>> submission) {
        return followWithQueueSnapshot(
                actorId, objectGeneration, payloadBytes, submission)
            .thenApply(FollowResult::value);
    }

    <T> CompletionStage<FollowResult<T>> followWithQueueSnapshot(
        String actorId,
        long objectGeneration,
        long payloadBytes,
        java.util.function.Supplier<CompletionStage<T>> submission) {
        MessageFollowSource source = messageFollowSources.get(actorId);
        if (source == null
            || source.sourceActorRef().generation() != objectGeneration
            || source.targetActorRef().generation() != objectGeneration) {
            return java.util.concurrent.CompletableFuture.failedFuture(
                new IllegalStateException(
                    "committed Message Follow route is unavailable or stale"));
        }
        if (!source.tryAcquire(payloadBytes)) {
            return java.util.concurrent.CompletableFuture.failedFuture(
                new IllegalStateException(
                    "committed Message Follow route queue exceeds 1024 messages or 16 MiB"));
        }
        MessageFollowQueueSnapshot queueSnapshot = source.queueSnapshot();
        CompletionStage<T> submitted;
        try {
            submitted = java.util.Objects.requireNonNull(
                submission.get(), "Message Follow submission returned null");
        } catch (RuntimeException failure) {
            source.release(payloadBytes);
            return java.util.concurrent.CompletableFuture.failedFuture(failure);
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

    static final class MessageFollowSource {
        private final ZLinkBackendActorRef sourceActorRef;
        private final ZLinkBackendActorRef targetActorRef;
        private final SpotTransportAddress targetAddress;
        private final ZLinkServiceMessageFollowWireCodec.ActorRoute targetRoute;
        private final long token;
        private final AtomicBoolean messageFollowNoticeClaimed =
            new AtomicBoolean();
        private int pendingMessages;
        private long pendingBytes;

        private MessageFollowSource(
            ZLinkBackendActorRef sourceActorRef,
            ZLinkBackendActorRef targetActorRef,
            SpotTransportAddress targetAddress,
            ZLinkServiceMessageFollowWireCodec.ActorRoute targetRoute,
            long token) {
            this.sourceActorRef = java.util.Objects.requireNonNull(
                sourceActorRef, "sourceActorRef");
            this.targetActorRef = java.util.Objects.requireNonNull(
                targetActorRef, "targetActorRef");
            this.targetAddress = targetAddress;
            this.targetRoute = targetRoute;
            this.token = token;
        }

        ZLinkBackendActorRef sourceActorRef() { return sourceActorRef; }
        ZLinkBackendActorRef targetActorRef() { return targetActorRef; }
        SpotTransportAddress targetAddress() { return targetAddress; }
        ZLinkServiceMessageFollowWireCodec.ActorRoute targetRoute() {
            return targetRoute;
        }
        long token() { return token; }

        boolean tryClaimMessageFollowNotice() {
            return messageFollowNoticeClaimed.compareAndSet(false, true);
        }

        boolean messageFollowNoticeClaimed() {
            return messageFollowNoticeClaimed.get();
        }

        void releaseMessageFollowNoticeClaim() {
            messageFollowNoticeClaimed.set(false);
        }

        private synchronized boolean tryAcquire(long bytes) {
            if (bytes < 0 || bytes > MAX_MESSAGE_FOLLOW_BYTES
                || pendingMessages >= MAX_MESSAGE_FOLLOW_MESSAGES
                || pendingBytes + bytes > MAX_MESSAGE_FOLLOW_BYTES) {
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

    record MessageFollowQueueSnapshot(int messages, long bytes) {
    }

    record FollowResult<T>(T value, MessageFollowQueueSnapshot queue) {
    }
}
