package MyGame.channel;

import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.framework.spots.*;

import java.util.List;
import java.util.concurrent.*;

public final class ZoneTransfer implements ZLinkSpotRelocationAdapter<Zone> {
    public CompletionStage<byte[]> capture(Zone spot, ZLinkRelocationCancellation cancellation) {
        Transfer.check(cancellation);
        try {
            return CompletableFuture.completedFuture(
                    MyGame.Json.MAPPER.writeValueAsBytes(spot.snapshot()));
        } catch (java.io.IOException e) {
            return CompletableFuture.failedFuture(e);
        }
    }

    public CompletionStage<Void> restore(
            Zone spot, byte[] state, ZLinkRelocationCancellation cancellation) {
        Transfer.check(cancellation);
        try {
            spot.restore(List.of(MyGame.Json.MAPPER.readValue(state, String[].class)));
            return CompletableFuture.completedFuture(null);
        } catch (java.io.IOException e) {
            return CompletableFuture.failedFuture(e);
        }
    }
}
