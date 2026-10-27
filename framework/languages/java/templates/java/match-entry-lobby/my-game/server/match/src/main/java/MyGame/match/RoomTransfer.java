package MyGame.match;

import MyGame.Json;

import systems.zlink.framework.actors.*;
import systems.zlink.framework.spots.*;

import java.util.concurrent.*;

public final class RoomTransfer implements ZLinkSpotRelocationAdapter<Room> {
    public CompletionStage<byte[]> capture(Room entity, ZLinkRelocationCancellation cancellation) {
        Transfer.check(cancellation);
        try {
            return CompletableFuture.completedFuture(
                    Json.MAPPER.writeValueAsBytes(entity.state.snapshot()));
        } catch (Exception error) {
            return CompletableFuture.failedFuture(error);
        }
    }

    public CompletionStage<Void> restore(
            Room entity, byte[] state, ZLinkRelocationCancellation cancellation) {
        Transfer.check(cancellation);
        try {
            var snapshot = Json.MAPPER.readValue(state, RoomState.Snapshot.class);
            entity.state.restore(snapshot);
            return CompletableFuture.completedFuture(null);
        } catch (Exception error) {
            return CompletableFuture.failedFuture(error);
        }
    }
}
