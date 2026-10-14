package MyGame.channel;

import java.util.concurrent.*;
import systems.zlink.framework.spots.*;

public final class Entry implements ZLinkEntrySpot<Player> {
  private final ZLinkEntrySpotContext context;

  public Entry(ZLinkEntrySpotContext context) {
    this.context = context;
  }

  public ZLinkEntrySpotContext context() {
    return context;
  }

  public CompletionStage<Void> onJoinedActor(Player actor) {
    return CompletableFuture.completedFuture(null);
  }

  public CompletionStage<Void> onLeaveActor(Player actor) {
    return CompletableFuture.completedFuture(null);
  }
}
