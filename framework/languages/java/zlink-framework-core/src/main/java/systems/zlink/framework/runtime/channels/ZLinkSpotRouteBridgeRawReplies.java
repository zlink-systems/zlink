package systems.zlink.framework.runtime.channels;

import java.time.Duration;
import java.util.ArrayDeque;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;

final class ZLinkSpotRouteBridgeRawReplies {
    private final Map<String, ArrayDeque<Pending>> pendingByChannel = new HashMap<>();
    private final Map<String, Map<RoutingId, Long>> recentlyCompletedByChannel = new HashMap<>();

    Pending enqueue(
        String channelName,
        RoutingId expectedPeerRid,
        List<byte[]> requestParts,
        CompletableFuture<List<Message>> reply,
        Duration retention) {
        Pending pending = new Pending(expectedPeerRid, requestParts, reply, retention);
        synchronized (pendingByChannel) {
            pendingByChannel
                .computeIfAbsent(channelName, ignored -> new ArrayDeque<>())
                .addLast(pending);
        }
        reply.whenComplete((ignored, error) -> complete(channelName, pending));
        return pending;
    }

    void remove(String channelName, CompletableFuture<List<Message>> reply) {
        synchronized (pendingByChannel) {
            ArrayDeque<Pending> queue = pendingByChannel.get(channelName);
            if (queue == null) {
                return;
            }
            queue.removeIf(pending -> pending.reply() == reply);
            if (queue.isEmpty()) {
                pendingByChannel.remove(channelName);
            }
        }
    }

    boolean tryComplete(String channelName, ZLinkBackendReceived received) {
        if (received.routingId().isEmpty()) {
            return false;
        }
        Pending pending;
        synchronized (pendingByChannel) {
            ArrayDeque<Pending> queue = pendingByChannel.get(channelName);
            if (queue == null || queue.isEmpty()) {
                return false;
            }
            pending = queue.peekFirst();
        }
        if (pending.expectedPeerRid() != null
            && !pending.expectedPeerRid().equals(received.routingId().get())) {
            return false;
        }
        if (isEcho(pending, received.parts())) {
            return false;
        }
        if (ZLinkChannelRuntime.isFrameworkErrorReply(received.parts())) {
            pending.reply().completeExceptionally(new ZLinkFrameworkException(
                ZLinkChannelRuntime.frameworkErrorReplyKind(received.parts()),
                ZLinkChannelRuntime.frameworkErrorReplyMessage(received.parts())));
            return true;
        }
        List<Message> copiedReply = copyReplyMessages(received.parts());
        if (sameFirstMessagePart(pending.requestParts(), copiedReply)) {
            copiedReply.forEach(Message::close);
            return false;
        }
        pending.reply().complete(copiedReply);
        return true;
    }

    boolean hasPending(String channelName, RoutingId peerRid) {
        synchronized (pendingByChannel) {
            ArrayDeque<Pending> queue = pendingByChannel.get(channelName);
            if (queue == null || queue.isEmpty()) {
                return false;
            }
            Pending pending = queue.peekFirst();
            return pending.expectedPeerRid() == null
                || pending.expectedPeerRid().equals(peerRid);
        }
    }

    boolean discardRecentlyCompleted(String channelName, ZLinkBackendReceived received) {
        if (received.routingId().isEmpty() || received.requestSeq().isEmpty()) {
            return false;
        }
        RoutingId routingId = received.routingId().get();
        long now = System.nanoTime();
        synchronized (pendingByChannel) {
            Map<RoutingId, Long> recent = recentlyCompletedByChannel.get(channelName);
            if (recent == null) {
                return false;
            }
            recent.entrySet().removeIf(entry -> entry.getValue() < now);
            Long deadline = recent.remove(routingId);
            if (recent.isEmpty()) {
                recentlyCompletedByChannel.remove(channelName);
            }
            return deadline != null && deadline >= now;
        }
    }

    private void complete(String channelName, Pending completed) {
        synchronized (pendingByChannel) {
            ArrayDeque<Pending> queue = pendingByChannel.get(channelName);
            if (queue != null) {
                queue.removeIf(pending -> pending.reply() == completed.reply());
                if (queue.isEmpty()) {
                    pendingByChannel.remove(channelName);
                }
            }
            if (completed.expectedPeerRid() != null) {
                recentlyCompletedByChannel
                    .computeIfAbsent(channelName, ignored -> new HashMap<>())
                    .put(
                        completed.expectedPeerRid(),
                        System.nanoTime() + completed.retention().toNanos());
            }
        }
    }

    static boolean isEcho(Pending pending, List<Message> parts) {
        return sameMessageParts(pending.requestParts(), parts)
            || sameFirstMessagePart(pending.requestParts(), parts);
    }

    static List<Message> copyReplyMessages(List<Message> parts) {
        int payloadOffset = 0;
        if (ZLinkChannelRuntime.looksLikeSpotRouteBridgePacket(parts)) {
            payloadOffset = 1;
        }
        if (parts.size() - payloadOffset > 1) {
            payloadOffset++;
        }
        if (payloadOffset > 0 && payloadOffset < parts.size()) {
            return ZLinkChannelRuntime.copyMessages(parts.subList(payloadOffset, parts.size()));
        }
        return ZLinkChannelRuntime.copyMessages(parts);
    }

    private static boolean sameMessageParts(List<byte[]> expected, List<Message> actual) {
        if (expected.size() != actual.size()) {
            return false;
        }
        for (int i = 0; i < expected.size(); i++) {
            if (!java.util.Arrays.equals(expected.get(i), actual.get(i).toByteArray())) {
                return false;
            }
        }
        return true;
    }

    private static boolean sameFirstMessagePart(List<byte[]> expected, List<Message> actual) {
        if (expected.isEmpty() || actual.isEmpty()) {
            return false;
        }
        return java.util.Arrays.equals(expected.get(0), actual.get(0).toByteArray());
    }

    record Pending(
        RoutingId expectedPeerRid,
        List<byte[]> requestParts,
        CompletableFuture<List<Message>> reply,
        Duration retention) {
    }
}
