package MyGame.channel;

import java.util.concurrent.*;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.framework.spots.*;

// Framework owns Spot identity and membership; the Zone has no application state.
public final class ZoneTransfer implements ZLinkSpotRelocationAdapter<Zone> {
  public CompletionStage<byte[]> capture(Zone spot, ZLinkRelocationCancellation cancellation) {
    Transfer.check(cancellation);
    return CompletableFuture.completedFuture(new byte[0]);
  }

  public CompletionStage<Void> restore(
      Zone spot, byte[] state, ZLinkRelocationCancellation cancellation) {
    Transfer.check(cancellation);
    if (state.length != 0) throw new IllegalArgumentException("Zone has no application state");
    return CompletableFuture.completedFuture(null);
  }
}
