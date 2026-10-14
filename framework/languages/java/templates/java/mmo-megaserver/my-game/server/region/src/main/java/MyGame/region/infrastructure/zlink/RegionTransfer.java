package MyGame.region.infrastructure.zlink;

import MyGame.*;
import MyGame.region.domain.Position;

import com.fasterxml.jackson.core.type.TypeReference;

import systems.zlink.framework.actors.*;
import systems.zlink.framework.spots.*;

import java.util.*;
import java.util.concurrent.*;

public final class RegionTransfer implements ZLinkSpotRelocationAdapter<Region> {
    private static final TypeReference<LinkedHashMap<String, Position>> STATE =
            new TypeReference<>() {};

    public CompletionStage<byte[]> capture(Region spot, ZLinkRelocationCancellation cancellation) {
        Transfer.check(cancellation);
        try {
            return CompletableFuture.completedFuture(
                    Json.MAPPER.writeValueAsBytes(spot.positions.snapshot()));
        } catch (java.io.IOException error) {
            return CompletableFuture.failedFuture(error);
        }
    }

    public CompletionStage<Void> restore(
            Region spot, byte[] state, ZLinkRelocationCancellation cancellation) {
        Transfer.check(cancellation);
        try {
            spot.positions.restore(Json.MAPPER.readValue(state, STATE));
            return CompletableFuture.completedFuture(null);
        } catch (java.io.IOException error) {
            return CompletableFuture.failedFuture(error);
        }
    }
}
