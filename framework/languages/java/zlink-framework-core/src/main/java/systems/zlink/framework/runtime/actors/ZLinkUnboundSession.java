package systems.zlink.framework.runtime.actors;

import systems.zlink.framework.actors.ZLinkBoundSession;
import systems.zlink.framework.actors.ZLinkBoundSessionSendCall;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

/**
 * The bound session an Actor gets while it has no binding.
 *
 * <p>Spec 04-actor-model §8.1: an operation needing a bound session with no valid binding ends with
 * {@code InvalidOperation}, and the failure surfaces at the call's terminal like every other call
 * failure — not thrown by the accessor that creates the call. Returning this instead of throwing
 * from {@code boundSession()} keeps the JVM surface the same as the other languages, where {@code
 * exceptionally} observes the failure and no {@code try} is needed around the accessor.
 */
final class ZLinkUnboundSession implements ZLinkBoundSession {
    private final String actorId;

    ZLinkUnboundSession(String actorId) {
        this.actorId = actorId;
    }

    @Override
    public ZLinkBoundSessionSendCall send(Object message) {
        return new UnboundSendCall(actorId);
    }

    @Override
    public CompletionStage<Void> disconnect() {
        return CompletableFuture.failedFuture(failure(actorId));
    }

    private static ZLinkFrameworkException failure(String actorId) {
        return new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.INVALID_OPERATION,
                "actor has no bound session: " + actorId);
    }

    private static final class UnboundSendCall implements ZLinkBoundSessionSendCall {
        private final String actorId;

        UnboundSendCall(String actorId) {
            this.actorId = actorId;
        }

        @Override
        public ZLinkBoundSessionSendCall metadata(String key, String value) {
            return this;
        }

        @Override
        public CompletionStage<Void> submit() {
            return CompletableFuture.failedFuture(failure(actorId));
        }
    }
}
