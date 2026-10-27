package MyGame.match.infrastructure.zlink;

import systems.zlink.framework.actors.*;
import systems.zlink.framework.spots.*;

import java.util.concurrent.*;

public final class RoomTransfer implements ZLinkSpotRelocationAdapter<Room> {
    public CompletionStage<byte[]> capture(Room entity, ZLinkRelocationCancellation cancellation) {
        Transfer.check(cancellation);
        try {
            return CompletableFuture.completedFuture(entity.capture());
        } catch (Exception error) {
            return CompletableFuture.failedFuture(error);
        }
    }

    public CompletionStage<Void> restore(
            Room entity, byte[] state, ZLinkRelocationCancellation cancellation) {
        Transfer.check(cancellation);
        try {
            entity.restore(state);
            return CompletableFuture.completedFuture(null);
        } catch (Exception error) {
            return CompletableFuture.failedFuture(error);
        }
    }
}
