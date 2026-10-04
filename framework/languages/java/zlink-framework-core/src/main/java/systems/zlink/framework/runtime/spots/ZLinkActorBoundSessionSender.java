package systems.zlink.framework.runtime.spots;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.ZLinkCompletionBridge;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.BooleanSupplier;

final class ZLinkActorBoundSessionSender {
    private final Duration timeout;
    private final BooleanSupplier closing;

    ZLinkActorBoundSessionSender(Duration timeout, BooleanSupplier closing) {
        this.timeout = timeout;
        this.closing = closing;
    }

    CompletionStage<Void> send(
            ZLinkInternalSpotNode node,
            ZLinkBackendActorRef actor,
            String actorId,
            byte[] frameBytes,
            String failureMessage) {
        if (closing.getAsBoolean()) {
            return CompletableFuture.completedFuture(null);
        }
        boolean remote;
        try {
            remote = node.hasRemoteActorBoundSessionRoute(actor);
            if (!remote && !node.hasLocalActorBoundSessionRoute(actor)) {
                return CompletableFuture.failedFuture(
                        new ZLinkFrameworkException(
                                ZLinkFrameworkErrorKind.INVALID_OPERATION,
                                failureMessage + ": " + actorId));
            }
        } catch (RuntimeException failure) {
            return CompletableFuture.failedFuture(failure);
        }
        Message frame = Message.from(frameBytes);
        CompletionStage<Void> submission;
        try {
            submission =
                    remote
                            ? node.sendRemoteActorBoundSession(actor, List.of(frame))
                            : node.sendLocalActorBoundSessionAsync(actor, List.of(frame), timeout);
        } catch (RuntimeException failure) {
            frame.close();
            return CompletableFuture.failedFuture(failure);
        }
        CompletableFuture<Void> result = new CompletableFuture<>();
        ZLinkCompletionBridge.forwardCancellation(result, submission);
        submission.whenComplete(
                (ignored, failure) -> {
                    frame.close();
                    if (failure == null) {
                        result.complete(null);
                    } else {
                        result.completeExceptionally(failure);
                    }
                });
        return result;
    }
}
