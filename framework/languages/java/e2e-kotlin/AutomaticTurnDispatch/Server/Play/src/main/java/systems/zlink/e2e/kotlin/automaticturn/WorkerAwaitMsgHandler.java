package systems.zlink.e2e.kotlin.automaticturn;

import java.time.Duration;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.spots.ZLinkSpotPacketHandler;

public final class WorkerAwaitMsgHandler
    implements ZLinkSpotPacketHandler<ProbeSpot, Contracts.WorkerAwaitMsg> {
    private final PlayEvidenceStore evidence;

    public WorkerAwaitMsgHandler(PlayEvidenceStore evidence) {
        this.evidence = evidence;
    }

    @Override
    public CompletionStage<Void> handle(
        ProbeSpot spot,
        Contracts.WorkerAwaitMsg command) {
        String value = "spot=" + spot.context().spotRid()
            + ";handler=spot";
        evidence.record(command.requestId(), "worker-await-started", value);
        var call = spot.context().runCpuWorker(cancellation -> {
                Thread.sleep(command.delayMillis());
                return command.requestId();
            })
            .timeout(Duration.ofSeconds(10));
        evidence.record(command.requestId(), "worker-await-released", value);
        return call.submit().thenAccept(result -> {
            evidence.record(result, "worker-await-resumed", value);
            evidence.record(result, "worker-await-completed", value);
        });
    }
}
