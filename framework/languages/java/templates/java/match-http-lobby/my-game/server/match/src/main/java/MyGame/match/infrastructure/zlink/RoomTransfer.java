package MyGame.match.infrastructure.zlink;

import MyGame.Json;
import systems.zlink.framework.actors.*;
import systems.zlink.framework.spots.*;

import java.util.concurrent.*;

public final class RoomTransfer implements ZLinkSpotRelocationAdapter<Room> {
    public CompletionStage<byte[]> capture(Room entity, ZLinkRelocationCancellation cancellation) {
        Transfer.check(cancellation);
        try {
            return CompletableFuture.completedFuture(
                    Json.MAPPER.writeValueAsBytes(
                            new RoomTransferSnapshot(entity.state.capture(), entity.roster.capture())));
        } catch (Exception error) {
            return CompletableFuture.failedFuture(error);
        }
    }

    public CompletionStage<Void> restore(
            Room entity, byte[] state, ZLinkRelocationCancellation cancellation) {
        Transfer.check(cancellation);
        try {
            var snapshot = Json.MAPPER.readValue(state, RoomTransferSnapshot.class);
            if (snapshot == null) throw new IllegalArgumentException("Missing Room snapshot");
            entity.roster.restore(snapshot.roster());
            entity.state.restore(snapshot.game());
            return CompletableFuture.completedFuture(null);
        } catch (Exception error) {
            return CompletableFuture.failedFuture(error);
        }
    }
}
