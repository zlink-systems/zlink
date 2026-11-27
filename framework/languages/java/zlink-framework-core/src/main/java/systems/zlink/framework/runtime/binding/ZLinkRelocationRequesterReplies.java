package systems.zlink.framework.runtime.binding;

import systems.zlink.framework.runtime.internal.locations.ZLinkRelocationReplyRow;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceRelocationWireCodec.ReplyRelay;

import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.function.BiPredicate;
import java.util.function.LongSupplier;

/** Correlates authenticated relocation results with the original request terminal. */
final class ZLinkRelocationRequesterReplies {
    private static final long RECEIPT_RETENTION_NANOS =
            ZLinkRelocationReplyRow.ACTIVE_RETENTION.toNanos();

    private final ConcurrentHashMap<UUID, Route> routes = new ConcurrentHashMap<>();
    private final LongSupplier nanoTime;

    ZLinkRelocationRequesterReplies(LongSupplier nanoTime) {
        this.nanoTime = nanoTime;
    }

    void register(
            UUID operation,
            long replyRouteId,
            CompletionStage<?> terminal,
            BiPredicate<ReplyRelay, List<byte[]>> accept) {
        var pending = new Pending(replyRouteId, accept);
        routes.put(operation, pending);
        terminal.whenComplete((ignored, failure) -> routes.remove(operation, pending));
    }

    boolean contains(ReplyRelay relay) {
        return matches(routes.get(operation(relay)), relay);
    }

    // 0 means no receipt, 1 is a newly accepted terminal, and 2 is an exact duplicate.
    int receive(ReplyRelay relay, List<byte[]> payload) {
        var id = operation(relay);
        var now = nanoTime.getAsLong();
        var fingerprint = fingerprint(payload);
        var outcome = new int[1];
        routes.computeIfPresent(
                id,
                (operation, route) -> {
                    if (!matches(route, relay)) return route;
                    if (route instanceof Pending pending && pending.accept.test(relay, payload)) {
                        outcome[0] = 1;
                        return new Receipt(
                                relay.replyRouteId(),
                                relay,
                                fingerprint,
                                now + RECEIPT_RETENTION_NANOS);
                    }
                    if (route instanceof Receipt receipt
                            && now - receipt.expiresAt < 0
                            && receipt.relay.equals(relay)
                            && MessageDigest.isEqual(receipt.fingerprint, fingerprint)) {
                        outcome[0] = 2;
                        return new Receipt(
                                relay.replyRouteId(),
                                relay,
                                fingerprint,
                                now + RECEIPT_RETENTION_NANOS);
                    }
                    return route;
                });
        expire(now);
        return outcome[0];
    }

    void clear() {
        routes.clear();
    }

    private static UUID operation(ReplyRelay relay) {
        return new UUID(relay.operation().high(), relay.operation().low());
    }

    private static boolean matches(Route route, ReplyRelay relay) {
        return route != null && route.replyRouteId() == relay.replyRouteId();
    }

    private void expire(long now) {
        routes.values()
                .removeIf(
                        route -> route instanceof Receipt receipt && now - receipt.expiresAt >= 0);
    }

    private sealed interface Route permits Pending, Receipt {
        long replyRouteId();
    }

    private record Pending(long replyRouteId, BiPredicate<ReplyRelay, List<byte[]>> accept)
            implements Route {}

    private record Receipt(long replyRouteId, ReplyRelay relay, byte[] fingerprint, long expiresAt)
            implements Route {}

    private static byte[] fingerprint(List<byte[]> payload) {
        try {
            var digest = MessageDigest.getInstance("SHA-256");
            payload.forEach(digest::update);
            return digest.digest();
        } catch (NoSuchAlgorithmException impossible) {
            throw new AssertionError(impossible);
        }
    }
}
