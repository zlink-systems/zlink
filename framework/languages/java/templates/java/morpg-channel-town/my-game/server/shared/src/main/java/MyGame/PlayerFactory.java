package MyGame;

import java.util.concurrent.*;
import systems.zlink.framework.actors.*;

public final class PlayerFactory implements ZLinkActorFactory {
  public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
    return CompletableFuture.completedFuture(new Player(context));
  }
}
