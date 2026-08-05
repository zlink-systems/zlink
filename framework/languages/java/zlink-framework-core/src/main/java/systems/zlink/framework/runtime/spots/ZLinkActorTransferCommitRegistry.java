package systems.zlink.framework.runtime.spots;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.function.Supplier;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.actors.ZLinkActorSpotRoutePackets;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;

/**
 * Retains one target-side commit terminal for the bounded reply-loss window.
 * A repeated commit request observes the same lifecycle/replay result instead
 * of consuming admission or invoking target callbacks a second time.
 */
final class ZLinkActorTransferCommitRegistry {
    private final ConcurrentHashMap<String, Entry> entries =
        new ConcurrentHashMap<>();

    CompletionStage<ZLinkActorSpotAdmission.RoutedJoin> execute(
        ZLinkActorSpotRoutePackets.TransferRequest request,
        Supplier<CompletionStage<ZLinkActorSpotAdmission.RoutedJoin>> operation) {
        String fingerprint = fingerprint(request);
        Entry candidate = new Entry(fingerprint);
        Entry existing = entries.putIfAbsent(request.transferId(), candidate);
        Entry selected = existing == null ? candidate : existing;
        if (!selected.fingerprint.equals(fingerprint)) {
            return CompletableFuture.failedFuture(
                new ZLinkConfigurationException(
                    "actor transfer commit does not match its transfer identity: "
                        + request.transferId()));
        }
        if (existing == null) {
            CompletionStage<ZLinkActorSpotAdmission.RoutedJoin> started;
            try {
                started = operation.get();
            } catch (Throwable failure) {
                candidate.terminal.completeExceptionally(failure);
                expire(request, candidate);
                return candidate.terminal.thenApply(Terminal::toRoutedJoin);
            }
            java.util.Objects.requireNonNull(
                started, "actor transfer commit operation returned null")
                .whenComplete((join, error) -> {
                    if (error != null) {
                        candidate.terminal.completeExceptionally(error);
                    } else {
                        candidate.terminal.complete(Terminal.from(join));
                    }
                    expire(request, candidate);
                });
        }
        return selected.terminal.thenApply(Terminal::toRoutedJoin);
    }

    private void expire(
        ZLinkActorSpotRoutePackets.TransferRequest request,
        Entry entry) {
        long delayMillis = Math.max(60_000L,
            Math.min(300_000L, Math.max(1L, request.timeoutMillis()) + 60_000L));
        CompletableFuture.delayedExecutor(
                delayMillis,
                java.util.concurrent.TimeUnit.MILLISECONDS)
            .execute(() -> entries.remove(request.transferId(), entry));
    }

    private static String fingerprint(
        ZLinkActorSpotRoutePackets.TransferRequest request) {
        return String.join(
            "|",
            request.phase(),
            request.actorId(),
            request.actorType(),
            request.actorNodeRid().toString(),
            Long.toUnsignedString(request.actorGeneration()),
            request.sourceEntrySpotNodeRid().toString(),
            request.sourceEntrySpotId(),
            request.sourceEntryRouterChannelId(),
            request.sourceNodeRid() == null ? "" : request.sourceNodeRid().toString(),
            request.sourceSessionRid() == null ? "" : request.sourceSessionRid().toString(),
            request.adapterKey() == null ? "" : request.adapterKey(),
            Integer.toString(request.backlogCount()),
            Boolean.toString(request.coreTransfer()),
            Long.toUnsignedString(request.coreTransferIdHigh()),
            Long.toUnsignedString(request.coreTransferIdLow()),
            Long.toUnsignedString(request.coreMembershipEpoch()),
            Long.toUnsignedString(request.coreFinalSequence()),
            Long.toUnsignedString(request.coreReserveMessageCount()),
            Long.toUnsignedString(request.coreReserveByteCount()),
            String.valueOf(request.completionManifest()),
            Integer.toString(java.util.Arrays.hashCode(
                request.sessionRouteCommand44())));
    }

    private static final class Entry {
        private final String fingerprint;
        private final CompletableFuture<Terminal> terminal =
            new CompletableFuture<>();

        private Entry(String fingerprint) {
            this.fingerprint = fingerprint;
        }
    }

    private record Terminal(
        ZLinkBackendActorRef actorRef,
        ZLinkSpotActorJoinResultSnapshot response,
        List<byte[]> handoffReplies) {
        private static Terminal from(
            ZLinkActorSpotAdmission.RoutedJoin join) {
            List<byte[]> replies = join.handoffReplies().stream()
                .map(Message::toByteArray)
                .toList();
            return new Terminal(
                join.actorRef(),
                ZLinkSpotActorJoinResultSnapshot.from(join.response()),
                replies);
        }

        private ZLinkActorSpotAdmission.RoutedJoin toRoutedJoin() {
            List<Message> replies = handoffReplies.stream()
                .map(Message::from)
                .toList();
            return new ZLinkActorSpotAdmission.RoutedJoin(
                actorRef,
                response.toResult(),
                replies);
        }
    }

    private record ZLinkSpotActorJoinResultSnapshot(
        boolean accepted,
        ZLinkMessage reply) {
        private static ZLinkSpotActorJoinResultSnapshot from(
            systems.zlink.framework.spots.ZLinkSpotActorJoinResult result) {
            return new ZLinkSpotActorJoinResultSnapshot(
                result.accepted(), result.reply());
        }

        private systems.zlink.framework.spots.ZLinkSpotActorJoinResult toResult() {
            return new systems.zlink.framework.spots.ZLinkSpotActorJoinResult(
                accepted, reply);
        }
    }
}
