package MyGame.zone;

import MyGame.*;

import systems.zlink.framework.actors.*;
import systems.zlink.framework.spots.*;

import java.util.concurrent.*;

public final class ZoneTransfer implements ZLinkSpotRelocationAdapter<Zone> {
    public CompletionStage<byte[]> capture(Zone spot, ZLinkRelocationCancellation cancellation) {
        Transfer.check(cancellation);
        try {
            return CompletableFuture.completedFuture(Json.MAPPER.writeValueAsBytes(spot.state));
        } catch (java.io.IOException error) {
            return CompletableFuture.failedFuture(error);
        }
    }

    public CompletionStage<Void> restore(
            Zone spot, byte[] state, ZLinkRelocationCancellation cancellation) {
        Transfer.check(cancellation);
        try {
            spot.state = Json.MAPPER.readValue(state, ZoneState.class);
            return CompletableFuture.completedFuture(null);
        } catch (java.io.IOException error) {
            return CompletableFuture.failedFuture(error);
        }
    }
}
