package systems.zlink.framework.runtime.internal.relocation;

import java.util.concurrent.CompletableFuture;

public final class ZLinkCompositeRelocationBarrierTestAccess {
    private ZLinkCompositeRelocationBarrierTestAccess() {}

    public static CompletableFuture<Void> transition(ZLinkCompositeRelocationBarrier barrier) {
        return barrier.transition;
    }

    public static void transition(
            ZLinkCompositeRelocationBarrier barrier, CompletableFuture<Void> transition) {
        barrier.transition = transition;
    }
}
