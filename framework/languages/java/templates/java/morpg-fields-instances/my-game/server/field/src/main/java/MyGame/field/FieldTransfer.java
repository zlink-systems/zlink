package MyGame.field;

import MyGame.*;

import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.framework.spots.*;

import java.util.concurrent.*;

public final class FieldTransfer implements ZLinkSpotRelocationAdapter<Field> {
    public CompletionStage<byte[]> capture(Field spot, ZLinkRelocationCancellation cancellation) {
        Transfer.check(cancellation);
        try {
            return CompletableFuture.completedFuture(Json.MAPPER.writeValueAsBytes(spot.state));
        } catch (java.io.IOException error) {
            return CompletableFuture.failedFuture(error);
        }
    }

    public CompletionStage<Void> restore(
            Field spot, byte[] state, ZLinkRelocationCancellation cancellation) {
        Transfer.check(cancellation);
        try {
            spot.state = Json.MAPPER.readValue(state, FieldState.class);
            return CompletableFuture.completedFuture(null);
        } catch (java.io.IOException error) {
            return CompletableFuture.failedFuture(error);
        }
    }
}
