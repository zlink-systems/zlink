package systems.zlink.framework.spots;

import java.time.Duration;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.core.RoutingId;

public interface ZLinkInstanceSpotContext {
    String meshName();

    String spotId();

    long objectGeneration();

    RoutingId nodeRid();

    ZLinkInstanceSpotHandlerRegistry handlers();

    ZLinkSpotOutbound outbound();

    <T> ZLinkWorkerCall<T> runCpuWorker(ZLinkWorkerTask<T> work);

    <T> ZLinkWorkerCall<T> runIoWorker(ZLinkIoWorkerTask<T> work);

    CompletionStage<Boolean> close();

    CompletionStage<ZLinkTimer> addTimer(
        String name,
        Duration period,
        Class<?> handlerType,
        ZLinkTimerOptions options);
}
