package systems.zlink.framework.spots;

import java.time.Duration;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;

public interface ZLinkSpotContext {
    String spotId();

    long objectGeneration();

    RoutingId nodeRid();

    default ZLinkSpotHandlerRegistry handlers() {
        throw new UnsupportedOperationException(
            "SPOT handler registration is only available on runtime-created contexts");
    }

    ZLinkSpotOutbound outbound();

    ZLinkSpotRelocationReadyCall relocationReady();

    default <T> ZLinkWorkerCall<T> runCpuWorker(ZLinkWorkerTask<T> work) {
        throw new UnsupportedOperationException(
            "worker offload is only available on runtime-created contexts");
    }

    default <T> ZLinkWorkerCall<T> runIoWorker(ZLinkIoWorkerTask<T> work) {
        throw new UnsupportedOperationException(
            "worker offload is only available on runtime-created contexts");
    }

    CompletionStage<Void> leaveActor(ZLinkActor actor);

    CompletionStage<Boolean> close();

    CompletionStage<ZLinkTimer> addTimer(
        String name,
        Duration period,
        Class<?> handlerType,
        ZLinkTimerOptions options);
}
