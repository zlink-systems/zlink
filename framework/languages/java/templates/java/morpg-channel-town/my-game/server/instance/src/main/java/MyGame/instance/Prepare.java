package MyGame.instance;

import MyGame.*;
import java.util.concurrent.*;
import systems.zlink.framework.handlers.*;

@ZLinkHandlerGroup(Names.DUNGEON)
public final class Prepare {
  @ZLinkSpotRequest
  public CompletionStage<DungeonPrepared> prepare(Dungeon spot, PrepareDungeon request) {
    return CompletableFuture.completedFuture(spot.prepare(request));
  }
}
