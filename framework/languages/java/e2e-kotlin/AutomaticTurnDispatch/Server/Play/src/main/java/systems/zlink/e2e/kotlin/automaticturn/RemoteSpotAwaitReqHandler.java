package systems.zlink.e2e.kotlin.automaticturn;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.Optional;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import systems.zlink.framework.handlers.ZLinkSpotRequest;
import systems.zlink.framework.spots.SpotHandle;
import systems.zlink.framework.spots.SpotHandleResolver;

public final class RemoteSpotAwaitReqHandler {
    private static final int TARGET_ADMISSION_ATTEMPTS = 5;
    private static final long TARGET_ADMISSION_RETRY_DELAY_MILLIS = 500;

    private final PlayEvidenceStore evidence;
    private final SpotHandleResolver spots;

    public RemoteSpotAwaitReqHandler(PlayEvidenceStore evidence, SpotHandleResolver spots) {
        this.evidence = evidence;
        this.spots = spots;
    }

    @ZLinkSpotRequest
    public CompletionStage<Contracts.ScenarioRes> handle(
        ProbeSpot spot,
        Contracts.RemoteSpotAwaitReq request) {
        String value = "spot=" + spot.context().spotId() + ";target=" + request.targetSpotRid();
        evidence.record(request.requestId(), "remote-await-started", value);
        evidence.record(request.requestId(), "remote-await-released", value);
        String targetSpotRid = request.targetSpotRid();
        return spots.resolveSpotHandle(targetSpotRid)
            .thenCompose(target -> requestTargetWithRetry(
                spot,
                requireSpot(target, targetSpotRid).spotId(),
                new Contracts.AwaitReq("ATD-D2", request.requestId(), "remote-spot"),
                TARGET_ADMISSION_ATTEMPTS))
            .thenApply((Contracts.ScenarioRes targetReply) -> {
                String resumed = value + ";targetNode=" + targetReply.result();
                evidence.record(request.requestId(), "remote-await-resumed", resumed);
                evidence.record(request.requestId(), "remote-await-completed", resumed);
                return new Contracts.ScenarioRes(
                    "ATD-D2", request.requestId(), spot.context().nodeRid().toString());
            });
    }

    private static CompletionStage<Contracts.ScenarioRes> requestTargetWithRetry(
        ProbeSpot spot,
        String targetSpotId,
        Contracts.AwaitReq request,
        int remainingAttempts) {
        return spot.context().outbound()
            .requestToSpot(targetSpotId, request)
            .timeout(Duration.ofSeconds(5))
            .submit(Contracts.ScenarioRes.class)
            .<CompletionStage<Contracts.ScenarioRes>>handle((reply, failure) -> {
                if (failure == null) {
                    return CompletableFuture.completedFuture(reply);
                }
                if (remainingAttempts <= 1 || !isAdmissionFailure(failure)) {
                    return CompletableFuture.failedFuture(failure);
                }
                return CompletableFuture.runAsync(
                        () -> { },
                        CompletableFuture.delayedExecutor(
                            TARGET_ADMISSION_RETRY_DELAY_MILLIS,
                            TimeUnit.MILLISECONDS))
                    .thenCompose(ignored -> requestTargetWithRetry(
                        spot,
                        targetSpotId,
                        request,
                        remainingAttempts - 1));
            })
            .thenCompose(stage -> stage);
    }

    private static boolean isAdmissionFailure(Throwable failure) {
        for (Throwable cause = failure; cause != null; cause = cause.getCause()) {
            if ("SPOT direct request was not admitted".equals(cause.getMessage())) {
                return true;
            }
        }
        return false;
    }

    private static SpotHandle requireSpot(Optional<SpotHandle> handle, String spotRid) {
        return handle.orElseThrow(() -> new IllegalStateException("spot not found: " + spotRid));
    }
}
