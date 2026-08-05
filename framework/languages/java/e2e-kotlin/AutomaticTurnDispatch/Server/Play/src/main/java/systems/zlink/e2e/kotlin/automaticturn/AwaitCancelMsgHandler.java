package systems.zlink.e2e.kotlin.automaticturn;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import systems.zlink.framework.spots.ZLinkSpotPacketHandler;

public final class AwaitCancelMsgHandler
    implements ZLinkSpotPacketHandler<ProbeSpot, Contracts.AwaitCancelMsg> {
    private final PlayEvidenceStore evidence;

    public AwaitCancelMsgHandler(PlayEvidenceStore evidence) {
        this.evidence = evidence;
    }

    @Override
    public CompletionStage<Void> handle(
        ProbeSpot spot,
        Contracts.AwaitCancelMsg command) {
        String value = "spot=" + spot.context().spotRid() + ";node=" + spot.context().nodeRid();
        evidence.record(command.requestId(), "cancel-await-started", value);
        evidence.record(command.requestId(), "cancel-await-released", value);
        CompletableFuture<Contracts.DelayRes> pending = spot.context().outbound()
            .requestToChannel(
                Contracts.DELAY_CHANNEL,
                new Contracts.DelayReq(command.requestId(), command.delayMillis()))
            .timeout(Duration.ofSeconds(5))
            .submit(Contracts.DelayRes.class)
            .toCompletableFuture();
        CompletableFuture.delayedExecutor(command.cancelAfterMillis(), TimeUnit.MILLISECONDS)
            .execute(() -> pending.cancel(true));
        return pending.handle((reply, error) -> {
            if (error == null) {
                evidence.record(command.requestId(), "cancel-await-unexpected-resumed", value);
            } else {
                evidence.record(command.requestId(), "cancel-await-completed", value + ";error=" + error);
            }
            return null;
        });
    }
}
