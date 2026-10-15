package MyGame;

import java.util.concurrent.CancellationException;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;

public final class Transfer {
  private Transfer() {}

  public static void check(ZLinkRelocationCancellation cancellation) {
    if (cancellation.isCancellationRequested())
      throw new CancellationException("Application state transfer cancelled");
  }
}
