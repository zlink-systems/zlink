package MyGame.channel;

import java.util.concurrent.CancellationException;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;

final class Transfer {
  private Transfer() {}

  static void check(ZLinkRelocationCancellation cancellation) {
    if (cancellation.isCancellationRequested())
      throw new CancellationException("Application state transfer cancelled");
  }
}
