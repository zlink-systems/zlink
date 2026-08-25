package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.util.OptionalLong;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;

final class ZLinkJavaRawMeshNodeReceiveFlowTest {
    @Test
    void bindFailureDeregistersReceiveFlowBeforeTheServicePortClosesRouter() {
        String endpoint = "inproc://raw-mesh-receive-flow-failure-"
            + System.nanoTime();
        ZLinkApplicationJobQueue queue = new ZLinkApplicationJobQueue(
            ZLinkApplicationJobQueueProfile.BALANCED,
            OptionalLong.of(1),
            new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null),
            100,
            0);
        ZLinkApplicationJobQueue.Permit held =
            queue.acquire().toCompletableFuture().join();

        try (Context context = Zlink.createContext();
             ZLinkJavaRawMeshNode owner = new ZLinkJavaRawMeshNode(context, "owner");
             ZLinkJavaRawMeshNode failed = new ZLinkJavaRawMeshNode(context, "failed")) {
            owner.setBind(endpoint);
            owner.start();

            failed.setApplicationJobQueue(queue);
            failed.setBind(endpoint);
            assertThrows(RuntimeException.class, failed::start);

            held.close();
            assertEquals(0,
                queue.pressureMetrics().flowStateConfigFailureCount());
        } finally {
            queue.close();
        }
    }
}
