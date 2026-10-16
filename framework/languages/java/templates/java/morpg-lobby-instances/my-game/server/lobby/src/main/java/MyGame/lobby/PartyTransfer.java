package MyGame.lobby;

import MyGame.*;

import systems.zlink.framework.actors.*;
import systems.zlink.framework.spots.*;

import java.util.concurrent.*;

public final class PartyTransfer implements ZLinkSpotRelocationAdapter<Party> {
    public CompletionStage<byte[]> capture(Party entity, ZLinkRelocationCancellation cancellation) {
        Transfer.check(cancellation);
        try {
            return CompletableFuture.completedFuture(Json.MAPPER.writeValueAsBytes(entity.state));
        } catch (java.io.IOException error) {
            return CompletableFuture.failedFuture(error);
        }
    }

    public CompletionStage<Void> restore(
            Party entity, byte[] payload, ZLinkRelocationCancellation cancellation) {
        Transfer.check(cancellation);
        try {
            entity.state = Json.MAPPER.readValue(payload, PartyState.class);
            return CompletableFuture.completedFuture(null);
        } catch (java.io.IOException error) {
            return CompletableFuture.failedFuture(error);
        }
    }
}
