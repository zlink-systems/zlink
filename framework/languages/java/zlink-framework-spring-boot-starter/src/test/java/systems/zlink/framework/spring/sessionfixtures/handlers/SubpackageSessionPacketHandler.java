package systems.zlink.framework.spring.sessionfixtures.handlers;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkTypedSessionPacketHandler;
import systems.zlink.framework.handlers.ZLinkPacket;

public final class SubpackageSessionPacketHandler
    implements ZLinkTypedSessionPacketHandler<
        ZLinkSessionContext,
        SubpackageSessionPacketHandler.SubpackageSessionPacket> {
    private final AtomicInteger count;
    private final CompletableFuture<Void> handled;

    public SubpackageSessionPacketHandler(
        AtomicInteger count,
        CompletableFuture<Void> handled) {
        this.count = count;
        this.handled = handled;
    }

    @Override
    public Class<SubpackageSessionPacket> messageType() {
        return SubpackageSessionPacket.class;
    }

    @Override
    public CompletionStage<Void> handle(
        ZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        SubpackageSessionPacket payload) {
        count.incrementAndGet();
        handled.complete(null);
        return CompletableFuture.completedFuture(null);
    }

    @ZLinkPacket("subpackage.session.packet")
    public record SubpackageSessionPacket(String value) {
    }
}
