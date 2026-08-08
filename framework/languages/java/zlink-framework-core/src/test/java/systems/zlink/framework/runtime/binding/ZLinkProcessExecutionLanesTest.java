package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertNotSame;
import static org.junit.jupiter.api.Assertions.assertSame;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;

final class ZLinkProcessExecutionLanesTest {
    @Test
    void topologiesShareInfrastructureWithoutBlockingEachOthersApplicationLane()
        throws Exception {
        assertSame(
            ZLinkProcessExecutionLanes.deadlines(),
            ZLinkProcessExecutionLanes.deadlines());
        var blockedTopology = ZLinkProcessExecutionLanes.applicationLane();
        var progressingTopology = ZLinkProcessExecutionLanes.applicationLane();
        assertNotSame(blockedTopology, progressingTopology);
        CompletableFuture<Void> release = new CompletableFuture<>();
        CompletableFuture<Void> blockedStarted = new CompletableFuture<>();
        CompletableFuture<Void> progressed = new CompletableFuture<>();

        blockedTopology.execute(() -> {
            blockedStarted.complete(null);
            release.join();
        });
        blockedStarted.get(3, TimeUnit.SECONDS);
        progressingTopology.execute(() -> progressed.complete(null));

        progressed.get(3, TimeUnit.SECONDS);
        release.complete(null);
    }
}
