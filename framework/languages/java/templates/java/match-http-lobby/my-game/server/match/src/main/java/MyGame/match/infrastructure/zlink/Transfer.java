package MyGame.match.infrastructure.zlink;

import systems.zlink.framework.actors.ZLinkRelocationCancellation;

import java.util.concurrent.CancellationException;

final class Transfer {
    private Transfer() {}

    static void check(ZLinkRelocationCancellation cancellation) {
        if (cancellation.isCancellationRequested())
            throw new CancellationException("Application state transfer cancelled");
    }
}
