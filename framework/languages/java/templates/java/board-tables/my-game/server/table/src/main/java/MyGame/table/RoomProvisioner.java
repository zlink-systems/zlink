package MyGame.table;

import MyGame.*;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import systems.zlink.framework.spots.*;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

public final class RoomProvisioner implements ZLinkInstanceSpot {
    private static final Logger LOG = LoggerFactory.getLogger(RoomProvisioner.class);
    private final ZLinkInstanceSpotContext context;
    private final ZLinkSpotManager spots;

    public RoomProvisioner(ZLinkInstanceSpotContext context, ZLinkSpotManager spots) {
        this.context = context;
        this.spots = spots;
    }

    public ZLinkInstanceSpotContext context() {
        return context;
    }

    public void configure() {
        context.handlers().addPacket(Prepare.class);
    }

    public static final class Prepare
            implements ZLinkSpotPacketHandler<RoomProvisioner, PrepareRoom> {
        public CompletionStage<Void> handle(RoomProvisioner provisioner, PrepareRoom message) {
            return provisioner.prepare(message);
        }
    }

    private CompletionStage<Void> prepare(PrepareRoom message) {
        return spots.getOrCreate(message.assignmentId(), Names.TABLE)
                .inMesh(Names.MESH)
                .submit()
                .thenCompose(
                        result ->
                                result.state() == ZLinkSpotCreateState.REJECTED
                                        ? CompletableFuture.completedFuture(false)
                                        : context.outbound()
                                                .sendToSpot(message.assignmentId(), message)
                                                .submit()
                                                .thenApply(ignored -> true))
                .exceptionally(
                        failure -> {
                            LOG.warn(
                                    "Room preparation failed for {}",
                                    message.assignmentId(),
                                    failure);
                            return false;
                        })
                .thenCompose(
                        sent ->
                                sent
                                        ? CompletableFuture.completedFuture(null)
                                        : context.outbound()
                                                .sendToSpot(
                                                        Names.POOL_KEY,
                                                        new RoomPrepared(
                                                                message.assignmentId(), false))
                                                .instanceSpot(Names.POOL)
                                                .inMesh(Names.MESH)
                                                .submit());
    }
}
