package MyGame.town;

import MyGame.*;
import java.util.concurrent.*;
import systems.zlink.framework.actors.*;
import systems.zlink.framework.spots.*;

public final class TownTransfer implements ZLinkSpotRelocationAdapter<Town> {
  public CompletionStage<byte[]> capture(Town entity, ZLinkRelocationCancellation cancellation) {
    Transfer.check(cancellation);
    try {
      return CompletableFuture.completedFuture(Json.MAPPER.writeValueAsBytes(entity.state));
    } catch (java.io.IOException error) {
      return CompletableFuture.failedFuture(error);
    }
  }

  public CompletionStage<Void> restore(
      Town entity, byte[] payload, ZLinkRelocationCancellation cancellation) {
    Transfer.check(cancellation);
    try {
      entity.state = Json.MAPPER.readValue(payload, TownState.class);
      return CompletableFuture.completedFuture(null);
    } catch (java.io.IOException error) {
      return CompletableFuture.failedFuture(error);
    }
  }
}
