package systems.zlink.framework.runtime.spots;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.function.BooleanSupplier;
import java.util.function.Consumer;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;

final class ZLinkActorBoundSessionSender {
    private static final long RETRY_DELAY_MILLIS = 25;
    private final Duration timeout;
    private final BooleanSupplier closing;
    private final Consumer<String> trace;

    ZLinkActorBoundSessionSender(
        Duration timeout,
        BooleanSupplier closing,
        Consumer<String> trace) {
        this.timeout = timeout;
        this.closing = closing;
        this.trace = trace;
    }

    CompletionStage<Void> send(
        ZLinkInternalSpotNode node,
        ZLinkBackendActorRef actor,
        String actorId,
        byte[] frameBytes,
        String failureMessage) {
        CompletableFuture<Void> result = new CompletableFuture<>();
        CompletableFuture.runAsync(new SendAttempt(
            node,
            actor,
            actorId,
            frameBytes,
            failureMessage,
            System.nanoTime() + timeout.toNanos(),
            result));
        return result;
    }

    private final class SendAttempt implements Runnable {
        private final ZLinkInternalSpotNode node;
        private final ZLinkBackendActorRef actor;
        private final String actorId;
        private final byte[] frameBytes;
        private final String failureMessage;
        private final long deadline;
        private final CompletableFuture<Void> result;

        private SendAttempt(
            ZLinkInternalSpotNode node,
            ZLinkBackendActorRef actor,
            String actorId,
            byte[] frameBytes,
            String failureMessage,
            long deadline,
            CompletableFuture<Void> result) {
            this.node = node;
            this.actor = actor;
            this.actorId = actorId;
            this.frameBytes = frameBytes;
            this.failureMessage = failureMessage;
            this.deadline = deadline;
            this.result = result;
        }

        @Override
        public void run() {
            if (result.isDone()) {
                return;
            }
            if (closing.getAsBoolean()) {
                result.complete(null);
                return;
            }
            if (node.hasRemoteActorBoundSessionRoute(actor)) {
                submitRemote();
                return;
            }
            if (node.hasLocalActorBoundSessionRoute(actor)) {
                submitLocal();
                return;
            }
            scheduleLogicalRouteRetry();
        }

        private void scheduleLogicalRouteRetry() {
            if (System.nanoTime() >= deadline) {
                result.completeExceptionally(new ZLinkConfigurationException(
                    failureMessage + ": " + actorId));
                return;
            }
            CompletableFuture.delayedExecutor(RETRY_DELAY_MILLIS, TimeUnit.MILLISECONDS)
                .execute(this);
        }

        private void submitLocal() {
            long remainingNanos = deadline - System.nanoTime();
            if (remainingNanos <= 0L) {
                scheduleLogicalRouteRetry();
                return;
            }
            Message frame = Message.from(frameBytes);
            CompletionStage<Void> submission;
            try {
                submission = node.sendLocalActorBoundSessionAsync(
                    actor, List.of(frame), Duration.ofNanos(remainingNanos));
            } catch (RuntimeException failure) {
                frame.close();
                result.completeExceptionally(failure);
                return;
            }
            result.whenComplete((ignored, failure) -> {
                if (result.isCancelled()) {
                    submission.toCompletableFuture().cancel(true);
                }
            });
            submission.whenComplete((ignored, failure) -> {
                frame.close();
                if (result.isDone()) {
                    return;
                }
                if (closing.getAsBoolean()) {
                    result.complete(null);
                } else if (failure == null) {
                    trace.accept("bound-session-send-ok"
                        + " actor=" + actorId
                        + " actorNode=" + actor.nodeRid());
                    result.complete(null);
                } else if (isMissingLogicalRoute(failure)) {
                    scheduleLogicalRouteRetry();
                } else {
                    result.completeExceptionally(failure);
                }
            });
        }

        private boolean isMissingLogicalRoute(Throwable failure) {
            Throwable current = failure;
            while ((current instanceof java.util.concurrent.CompletionException
                    || current instanceof java.util.concurrent.ExecutionException)
                && current.getCause() != null) {
                current = current.getCause();
            }
            return current instanceof ZlinkSubmitException submit
                && submit.getResult() == SubmitResult.NOT_FOUND;
        }

        private void submitRemote() {
            Message frame = Message.from(frameBytes);
            CompletionStage<Void> submission;
            try {
                submission = node.sendRemoteActorBoundSession(
                    actor, List.of(frame));
            } catch (RuntimeException failure) {
                Message.closeAll(List.of(frame));
                result.completeExceptionally(failure);
                return;
            }
            submission.whenComplete((ignored, failure) -> {
                Message.closeAll(List.of(frame));
                if (failure == null) {
                    result.complete(null);
                } else {
                    result.completeExceptionally(failure);
                }
            });
        }
    }
}
