package MyGame.table;

import MyGame.Json;

import systems.zlink.framework.actors.*;
import systems.zlink.framework.spots.*;

import java.util.concurrent.*;

public final class TableTransfer implements ZLinkSpotRelocationAdapter<Table> {
    public CompletionStage<byte[]> capture(Table entity, ZLinkRelocationCancellation cancellation) {
        Transfer.check(cancellation);
        try {
            return CompletableFuture.completedFuture(Json.MAPPER.writeValueAsBytes(entity.state));
        } catch (Exception error) {
            return CompletableFuture.failedFuture(error);
        }
    }

    public CompletionStage<Void> restore(
            Table entity, byte[] state, ZLinkRelocationCancellation cancellation) {
        Transfer.check(cancellation);
        try {
            entity.state = Json.MAPPER.readValue(state, TableState.class);
            return CompletableFuture.completedFuture(null);
        } catch (Exception error) {
            return CompletableFuture.failedFuture(error);
        }
    }
}
