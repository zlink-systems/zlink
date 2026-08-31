package systems.zlink.framework.runtime.actors;
import java.time.Duration;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CompletionException;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.function.Consumer;
import java.util.function.Supplier;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceMessageFollowWireCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddress;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

/** Owns the transfer-only backlog and bounded source-retirement state. */
final class ZLinkActorTransferHandoff implements AutoCloseable {
    private final ScheduledExecutorService retirementsExecutor =
        Executors.newSingleThreadScheduledExecutor(runnable -> {
            Thread thread = new Thread(runnable, "zlink-actor-transfer-retirement");
            thread.setDaemon(true);
            return thread;
        });

    // This transfer's C2 state is owned by one lane. Scheduler, packet terminal,
    // suppression, and removal callbacks deliberately run after their state turn.
    private final ZLinkStateLane stateLane = new ZLinkStateLane();
    private final Map<String, Backlog> backlogs = new HashMap<>();
    private long arrivalIndex;
    // Exact source-route keyed tenures share this owner with the pre-commit
    // backlog.  Keeping each tenure by token prevents A->B->A and an older
    // retirement callback from deleting a later route for the same Actor id.
    private final Map<Long, MessageFollowSource> messageFollowSources =
        new HashMap<>();
    private final Set<Retention> retirements = new HashSet<>();
    private long messageFollowToken;
    private final ZLinkMessageFollowSuppressionRegistry messageFollowSuppression =
        new ZLinkMessageFollowSuppressionRegistry();
    private boolean closed;

    private <T> T inStateLane(Supplier<T> work) {
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

    void begin(String actorId) {
        inStateLane(() -> {
            requireOpen();
            backlogs.put(actorId, new Backlog());
            return null;
        });
    }

    ZLinkActorHandoffPacket capture(
        String actorId,
        ZLinkStreamHeader header,
        Message payload,
        ZLinkActorReplyRoute replyRoute,
        byte[] acceptedJournalRecord) {
        CaptureResult result = inStateLane(() -> {
            Backlog backlog = backlogs.get(actorId);
            if (backlog == null) {
                return null;
            }
            ZLinkActorHandoffPacket packet = new ZLinkActorHandoffPacket(
                ++arrivalIndex, header, payload, replyRoute, acceptedJournalRecord);
            if (backlogs.get(actorId) != backlog) {
                return new CaptureResult(null, packet);
            }
            long bytes = packet.retainedBytes();
            backlog.packets.add(packet);
            backlog.bytes += bytes;
            return new CaptureResult(packet, null);
        });
        if (result == null) {
            return null;
        }
        if (result.discarded() != null) {
            result.discarded().close();
        }
        return result.captured();
    }

    List<ZLinkActorHandoffPacket> take(String actorId) {
        return inStateLane(() -> {
            Backlog backlog = backlogs.get(actorId);
            if (backlog == null) {
                return List.of();
            }
            List<ZLinkActorHandoffPacket> snapshot = List.copyOf(backlog.packets);
            backlog.packets.clear();
            backlog.bytes = 0;
            return snapshot;
        });
    }

    int pendingCount(String actorId) {
        return inStateLane(() -> {
            Backlog backlog = backlogs.get(actorId);
            return backlog == null ? 0 : backlog.packets.size();
        });
    }

    List<ZLinkActorHandoffPacket> finish(String actorId) {
        return inStateLane(() -> {
            Backlog backlog = backlogs.remove(actorId);
            return backlog == null ? List.of() : List.copyOf(backlog.packets);
        });
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
        return inStateLane(() -> {
            Backlog backlog = backlogs.get(actorId);
            if (backlog == null) {
                return committed == null || committed.isEmpty()
                    ? List.of()
                    : List.copyOf(committed);
            }
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
        });
    }

    void fail(String actorId, Throwable error) {
        List<ZLinkActorHandoffPacket> packets = inStateLane(() -> {
            Backlog backlog = backlogs.remove(actorId);
            return backlog == null ? List.of() : List.copyOf(backlog.packets);
        });
        packets.forEach(packet -> {
            if (packet.fail(error)) {
                packet.close();
            }
        });
    }

    void retain(
        String actorId,
        ZLinkBackendActorRef sourceActorRef,
        ZLinkBackendActorRef targetActorRef,
        Duration duration,
        Consumer<MessageFollowSource> removal) {
        retain(actorId, sourceActorRef, targetActorRef, null, duration, removal);
    }

    void retain(
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

    void retain(
        String actorId,
        ZLinkBackendActorRef sourceActorRef,
        ZLinkBackendActorRef targetActorRef,
        SpotTransportAddress targetAddress,
        ZLinkServiceMessageFollowWireCodec.ActorRoute sourceRoute,
        ZLinkServiceMessageFollowWireCodec.ActorRoute targetRoute,
        Duration duration,
        Consumer<MessageFollowSource> removal) {
        RetainState retainedState = inStateLane(() -> {
            requireOpen();
            if (targetAddress != null
                && (sourceRoute == null || targetRoute == null)) {
                throw new IllegalArgumentException(
                    "exact source and target routes are required when a Message Follow target address is set");
            }
            MessageFollowSource source = new MessageFollowSource(
                sourceActorRef, targetActorRef, targetAddress,
                sourceRoute, targetRoute,
                null, ++messageFollowToken, true, messageFollowSuppression);
            MessageFollowSource replaced = sourceRoute == null
                ? newestSource(actorId, false)
                : exactSource(sourceRoute);
            if (replaced != null) {
                messageFollowSources.remove(replaced.token(), replaced);
            }
            messageFollowSources.put(source.token(), source);
            Retention retained = new Retention(actorId, source, removal);
            retirements.add(retained);
            return new RetainState(source, replaced, retained);
        });
        if (retainedState.replaced() != null) {
            retainedState.replaced().expireMessageFollowNotices();
        }
        ScheduledFuture<?> future = retirementsExecutor.schedule(
            () -> retire(retainedState.retained()),
            duration.toMillis(),
            TimeUnit.MILLISECONDS);
        boolean retained = inStateLane(() -> {
            if (!retirements.contains(retainedState.retained())) {
                return false;
            }
            retainedState.retained().future(future);
            return true;
        });
        if (!retained) {
            future.cancel(false);
        }
    }

    Optional<MessageFollowSource> messageFollowSource(String actorId) {
        return inStateLane(() -> Optional.ofNullable(newestSource(actorId, true)));
    }

    Optional<MessageFollowSource> messageFollowSource(
        ZLinkServiceMessageFollowWireCodec.ActorRoute sourceRoute) {
        return inStateLane(() -> Optional.ofNullable(exactSource(sourceRoute)));
    }

    void stageRelocationRoute(
        ZLinkServiceMessageFollowWireCodec.ActorRoute sourceRoute,
        ZLinkServiceMessageFollowWireCodec.ActorRoute targetRoute,
        systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec
            .ActorRouteFence rawTargetRoute) {
        inStateLane(() -> {
            requireOpen();
            MessageFollowSource previous = exactSource(sourceRoute);
            if (previous != null) {
                if (previous.committed()) {
                    throw new IllegalStateException(
                        "committed Message Follow source route cannot be staged again");
                }
                messageFollowSources.remove(previous.token(), previous);
            }
            MessageFollowSource staged = new MessageFollowSource(
                actorRef(sourceRoute), actorRef(targetRoute), null,
                sourceRoute, targetRoute, rawTargetRoute,
                ++messageFollowToken, false, messageFollowSuppression);
            messageFollowSources.put(staged.token(), staged);
            return null;
        });
    }

    void commitRelocationRoute(
        ZLinkServiceMessageFollowWireCodec.ActorRoute sourceRoute,
        Duration duration) {
        Objects.requireNonNull(duration, "duration");
        if (duration.isNegative()) {
            throw new IllegalArgumentException(
                "Message Follow duration must not be negative");
        }
        CommitRouteState state = inStateLane(() -> {
            MessageFollowSource source = exactSource(sourceRoute);
            if (source == null) {
                throw new IllegalStateException(
                    "pre-commit Message Follow source route is unavailable");
            }
            source.commit();
            if (duration.isZero()) {
                messageFollowSources.remove(source.token(), source);
                source.expireMessageFollowNotices();
                return null;
            }
            Retention retained = new Retention(
                sourceRoute.actorId(), source, ignored -> { });
            retirements.add(retained);
            return new CommitRouteState(source, retained);
        });
        if (state == null) {
            return;
        }
        ScheduledFuture<?> future = retirementsExecutor.schedule(
            () -> retire(state.retained()), duration.toMillis(),
            TimeUnit.MILLISECONDS);
        boolean retained = inStateLane(() -> {
            if (!retirements.contains(state.retained())) {
                return false;
            }
            state.retained().future(future);
            return true;
        });
        if (!retained) {
            future.cancel(false);
        }
    }

    void refreshRelocationRoute(
        ZLinkServiceMessageFollowWireCodec.ActorRoute sourceRoute,
        ZLinkServiceMessageFollowWireCodec.ActorRoute targetRoute,
        systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec
            .ActorRouteFence rawTargetRoute) {
        inStateLane(() -> {
            MessageFollowSource source = exactSource(sourceRoute);
            if (source == null) {
                return null;
            }
            source.refreshTarget(targetRoute, rawTargetRoute);
            return null;
        });
    }

    void abortRelocationRoute(
        ZLinkServiceMessageFollowWireCodec.ActorRoute sourceRoute) {
        MessageFollowSource removed = inStateLane(() -> {
            MessageFollowSource source = exactSource(sourceRoute);
            if (source == null || source.committed()) {
                return null;
            }
            messageFollowSources.remove(source.token(), source);
            return source;
        });
        if (removed != null) {
            removed.expireMessageFollowNotices();
        }
    }

    Optional<MessageFollowSource> takeMessageFollowSource(String actorId) {
        TakeSourceState taken = inStateLane(() -> {
            MessageFollowSource source = newestSource(actorId, true);
            if (source == null) {
                return null;
            }
            messageFollowSources.remove(source.token(), source);
            Retention retained = retirements.stream()
                .filter(candidate -> candidate.actorId().equals(actorId)
                    && candidate.source().equals(source))
                .findFirst()
                .orElse(null);
            if (retained != null) {
                retirements.remove(retained);
            }
            return new TakeSourceState(source, retained);
        });
        if (taken == null) {
            return Optional.empty();
        }
        taken.source().expireMessageFollowNotices();
        if (taken.retained() != null && taken.retained().future() != null) {
            taken.retained().future().cancel(false);
        }
        return Optional.of(taken.source());
    }

    int messageFollowSourceCount() {
        return inStateLane(messageFollowSources::size);
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
        MessageFollowSource source = inStateLane(
            () -> newestSource(actorId, true));
        if (source == null) {
            return CompletableFuture.failedFuture(
                new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.UNAVAILABLE,
                    "committed Message Follow route is unavailable"));
        }
        return followWithQueueSnapshot(
            source, objectGeneration, payloadBytes, submission);
    }

    <T> CompletionStage<FollowResult<T>> followWithQueueSnapshot(
        MessageFollowSource source,
        long objectGeneration,
        long payloadBytes,
        Supplier<CompletionStage<T>> submission) {
        boolean current = inStateLane(() ->
            messageFollowSources.get(source.token()) == source);
        if (!current) {
            return CompletableFuture.failedFuture(
                new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.UNAVAILABLE,
                    "Message Follow tenure is unavailable"));
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
    public void close() {
        CloseState state = inStateLane(() -> {
            if (closed) {
                return null;
            }
            closed = true;
            List<Retention> retained = List.copyOf(retirements);
            retirements.clear();
            List<Backlog> heldBacklogs = List.copyOf(backlogs.values());
            backlogs.clear();
            messageFollowSources.clear();
            return new CloseState(retained, heldBacklogs);
        });
        if (state == null) {
            return;
        }
        retirementsExecutor.shutdownNow();
        state.retained().forEach(retained -> {
            ScheduledFuture<?> future = retained.future();
            if (future != null) {
                future.cancel(false);
            }
            try {
                retained.source().expireMessageFollowNotices();
                retained.removal().accept(retained.source());
            } catch (RuntimeException ignored) {
                // Runtime shutdown must continue retiring the remaining owned sources.
            }
        });
        state.backlogs().stream()
            .flatMap(backlog -> backlog.packets.stream())
            .forEach(packet -> {
                if (packet.fail(new IllegalStateException(
                    "Actor runtime closed during transfer."))) {
                    packet.close();
                }
            });
    }

    private void retire(Retention retained) {
        RetireState state = inStateLane(() -> {
            if (!retirements.remove(retained)) {
                return null;
            }
            messageFollowSources.remove(
                retained.source().token(), retained.source());
            return new RetireState(retained.source(), retained.removal());
        });
        if (state == null) {
            return;
        }
        state.source().expireMessageFollowNotices();
        state.removal().accept(state.source());
    }

    private void requireOpen() {
        if (closed) {
            throw new IllegalStateException("Actor transfer handoff is closed.");
        }
    }

    private MessageFollowSource exactSource(
        ZLinkServiceMessageFollowWireCodec.ActorRoute route) {
        return messageFollowSources.values().stream()
            .filter(source -> route.equals(source.sourceRoute()))
            .max(Comparator.comparingLong(MessageFollowSource::token))
            .orElse(null);
    }

    private MessageFollowSource newestSource(
        String actorId,
        boolean committedOnly) {
        return messageFollowSources.values().stream()
            .filter(source -> source.sourceActorRef().actorId().equals(actorId)
                && (!committedOnly || source.committed()))
            .max(Comparator.comparingLong(MessageFollowSource::token))
            .orElse(null);
    }

    private static ZLinkBackendActorRef actorRef(
        ZLinkServiceMessageFollowWireCodec.ActorRoute route) {
        return new ZLinkBackendActorRef(
            route.targetNodeRid(), route.actorId(), route.objectGeneration());
    }

    private static final class Retention {
        private final String actorId;
        private final MessageFollowSource source;
        private final Consumer<MessageFollowSource> removal;
        private ScheduledFuture<?> future;

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

    private record CaptureResult(
        ZLinkActorHandoffPacket captured,
        ZLinkActorHandoffPacket discarded) {
    }

    private record RetainState(
        MessageFollowSource source,
        MessageFollowSource replaced,
        Retention retained) {
    }

    private record CommitRouteState(
        MessageFollowSource source,
        Retention retained) {
    }

    private record TakeSourceState(MessageFollowSource source, Retention retained) {
    }

    private record RetireState(
        MessageFollowSource source,
        Consumer<MessageFollowSource> removal) {
    }

    private record CloseState(List<Retention> retained, List<Backlog> backlogs) {
    }

    static final class MessageFollowSource {
        private final ZLinkBackendActorRef sourceActorRef;
        private final ZLinkBackendActorRef targetActorRef;
        private final SpotTransportAddress targetAddress;
        private final ZLinkServiceMessageFollowWireCodec.ActorRoute sourceRoute;
        private ZLinkServiceMessageFollowWireCodec.ActorRoute targetRoute;
        private systems.zlink.framework.runtime.internal.service
            .ZLinkServiceM6BWireCodec.ActorRouteFence rawTargetRoute;
        private final long token;
        private boolean committed;
        private final ZLinkMessageFollowSuppressionRegistry suppression;
        private final ZLinkStateLane stateLane = new ZLinkStateLane();
        private final Set<ZLinkMessageFollowSuppressionRegistry.Key> suppressionKeys =
            new HashSet<>();
        private boolean suppressionExpired;
        private int pendingMessages;
        private long pendingBytes;

        private MessageFollowSource(
            ZLinkBackendActorRef sourceActorRef,
            ZLinkBackendActorRef targetActorRef,
            SpotTransportAddress targetAddress,
            ZLinkServiceMessageFollowWireCodec.ActorRoute sourceRoute,
            ZLinkServiceMessageFollowWireCodec.ActorRoute targetRoute,
            systems.zlink.framework.runtime.internal.service
                .ZLinkServiceM6BWireCodec.ActorRouteFence rawTargetRoute,
            long token,
            boolean committed,
            ZLinkMessageFollowSuppressionRegistry suppression) {
            this.sourceActorRef = Objects.requireNonNull(
                sourceActorRef, "sourceActorRef");
            this.targetActorRef = Objects.requireNonNull(
                targetActorRef, "targetActorRef");
            this.targetAddress = targetAddress;
            this.sourceRoute = sourceRoute;
            this.targetRoute = targetRoute;
            this.rawTargetRoute = rawTargetRoute;
            this.token = token;
            this.committed = committed;
            this.suppression = Objects.requireNonNull(suppression, "suppression");
        }

        ZLinkBackendActorRef sourceActorRef() { return sourceActorRef; }
        ZLinkBackendActorRef targetActorRef() { return targetActorRef; }
        SpotTransportAddress targetAddress() { return targetAddress; }
        ZLinkServiceMessageFollowWireCodec.ActorRoute sourceRoute() {
            return sourceRoute;
        }
        ZLinkServiceMessageFollowWireCodec.ActorRoute targetRoute() {
            return inStateLane(() -> targetRoute);
        }
        systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec
            .ActorRouteFence rawTargetRoute() {
            return inStateLane(() -> rawTargetRoute);
        }
        long token() { return token; }
        boolean committed() { return inStateLane(() -> committed); }

        void commit() {
            inStateLane(() -> {
                Objects.requireNonNull(targetRoute, "targetRoute");
                Objects.requireNonNull(rawTargetRoute, "rawTargetRoute");
                committed = true;
                return null;
            });
        }

        void refreshTarget(
            ZLinkServiceMessageFollowWireCodec.ActorRoute target,
            systems.zlink.framework.runtime.internal.service
                .ZLinkServiceM6BWireCodec.ActorRouteFence rawTarget) {
            inStateLane(() -> {
                targetRoute = Objects.requireNonNull(target, "target");
                rawTargetRoute = Objects.requireNonNull(rawTarget, "rawTarget");
                return null;
            });
        }

        private <T> T inStateLane(Supplier<T> work) {
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

        boolean matchesSourceRoute(
            ZLinkServiceMessageFollowWireCodec.ActorRoute candidate) {
            return sourceRoute != null && sourceRoute.equals(candidate);
        }

        Optional<ZLinkMessageFollowSuppressionRegistry.Claim>
            beginMessageFollowNotice(
                ZLinkServiceMessageFollowWireCodec.ActorRoute sourceRoute) {
            ZLinkMessageFollowSuppressionRegistry.Key key = inStateLane(() -> {
                if (!matchesSourceRoute(sourceRoute)
                    || targetRoute == null
                    || suppressionExpired) {
                    return null;
                }
                ZLinkMessageFollowSuppressionRegistry.Key candidate =
                    ZLinkMessageFollowSuppressionRegistry.Key.actor(
                        sourceRoute, targetRoute);
                suppressionKeys.add(candidate);
                return candidate;
            });
            if (key == null) {
                return Optional.empty();
            }
            Optional<ZLinkMessageFollowSuppressionRegistry.Claim> claim =
                suppression.begin(key);
            boolean accepted = inStateLane(() -> !suppressionExpired);
            if (accepted) {
                return claim;
            }
            suppression.expire(key);
            return Optional.empty();
        }

        void markMessageFollowNoticeSent(
            ZLinkMessageFollowSuppressionRegistry.Claim claim) {
            suppression.markSent(claim);
        }

        void abortMessageFollowNotice(
            ZLinkMessageFollowSuppressionRegistry.Claim claim) {
            suppression.abort(claim);
        }

        void expireMessageFollowNotices() {
            List<ZLinkMessageFollowSuppressionRegistry.Key> keys =
                inStateLane(() -> {
                    suppressionExpired = true;
                    List<ZLinkMessageFollowSuppressionRegistry.Key> snapshot =
                        List.copyOf(suppressionKeys);
                    suppressionKeys.clear();
                    return snapshot;
                });
            keys.forEach(suppression::expire);
        }

        private boolean tryAcquire(long bytes) {
            return inStateLane(() -> {
                if (bytes < 0) {
                    return false;
                }
                pendingMessages++;
                pendingBytes += bytes;
                return true;
            });
        }

        private void release(long bytes) {
            inStateLane(() -> {
                pendingMessages--;
                pendingBytes -= bytes;
                return null;
            });
        }

        int pendingMessages() { return inStateLane(() -> pendingMessages); }
        long pendingBytes() { return inStateLane(() -> pendingBytes); }

        MessageFollowQueueSnapshot queueSnapshot() {
            return inStateLane(() ->
                new MessageFollowQueueSnapshot(pendingMessages, pendingBytes));
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
