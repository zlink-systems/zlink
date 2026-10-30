package systems.zlink.framework.runtime.spots;

import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode;
import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

import java.util.concurrent.CompletionStage;

/** Test-only access to the package-private state-lane constructor for other packages' tests. */
public final class ZLinkSpotSerialExecutorTesting {
    private ZLinkSpotSerialExecutorTesting() {}

    public static ZLinkSpotSerialExecutor inlinePerActor(ZLinkStateLane stateLane) {
        return new ZLinkSpotSerialExecutor(
                new ZLinkSerialExecutionQueue(Runnable::run, ZLinkExecutionLanePolicy.spot()),
                new ZLinkSerialExecutionQueue(Runnable::run, ZLinkExecutionLanePolicy.spot()),
                Runnable::run,
                ZLinkUserSpotExecutionMode.PER_ACTOR,
                false,
                stateLane);
    }

    public static CompletionStage<Void> closeAsync(ZLinkSpotSerialExecutor executor) {
        return executor.closeAsync();
    }
}
