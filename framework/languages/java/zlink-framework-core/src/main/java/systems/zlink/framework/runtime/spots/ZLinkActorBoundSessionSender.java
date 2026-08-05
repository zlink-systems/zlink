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
import systems.zlink.contracts.sockets.SendFlags;
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
            if (trySend()) {
                return;
            }
            if (System.nanoTime() >= deadline) {
                result.completeExceptionally(new ZLinkConfigurationException(
                    failureMessage + ": " + actorId));
                return;
            }
            CompletableFuture.delayedExecutor(RETRY_DELAY_MILLIS, TimeUnit.MILLISECONDS)
                .execute(this);
        }

        private boolean trySend() {
            try (Message frame = Message.from(frameBytes)) {
                if (node.sendActorBoundSession(actor, List.of(frame), SendFlags.NONE)) {
                    trace.accept("bound-session-send-ok"
                        + " actor=" + actorId
                        + " actorNode=" + actor.nodeRid());
                    result.complete(null);
                    return true;
                }
                trace.accept("bound-session-send-retry"
                    + " actor=" + actorId
                    + " actorNode=" + actor.nodeRid());
                return false;
            } catch (ZlinkSubmitException error) {
                SubmitResult submitResult = error.getResult();
                if (submitResult != SubmitResult.NOT_CONNECTED
                    && submitResult != SubmitResult.NOT_FOUND
                    && submitResult != SubmitResult.BACKPRESSURED) {
                    result.completeExceptionally(error);
                    return true;
                }
                trace.accept("bound-session-send-submit-retry"
                    + " actor=" + actorId
                    + " actorNode=" + actor.nodeRid()
                    + " result=" + submitResult);
                return false;
            } catch (RuntimeException error) {
                result.completeExceptionally(error);
                return true;
            }
        }
    }
}
