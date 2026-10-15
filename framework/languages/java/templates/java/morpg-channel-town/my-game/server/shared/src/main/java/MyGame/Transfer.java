package MyGame;

import systems.zlink.framework.actors.ZLinkRelocationCancellation;

import java.util.concurrent.CancellationException;

public final class Transfer {
    private Transfer() {}

    public static void check(ZLinkRelocationCancellation cancellation) {
        if (cancellation.isCancellationRequested())
            throw new CancellationException("Application state transfer cancelled");
    }
}
