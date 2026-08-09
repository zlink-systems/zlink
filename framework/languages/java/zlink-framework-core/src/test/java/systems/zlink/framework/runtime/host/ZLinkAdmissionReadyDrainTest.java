package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertEquals;

import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.Consumer;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdmissionKey;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject;

/**
 * A bound-Session ready edge reports that the remote bound-session binding was
 * installed, not that one unit of send capacity became available. The install
 * emits exactly one edge, so every push that parked while the binding was
 * missing has to be released by it.
 */
final class ZLinkAdmissionReadyDrainTest {
    private static final RoutingId NODE = RoutingId.from("target");

    @Test
    void boundSessionReadyReleasesEveryParkedSubmission() {
        Backend backend = new Backend();
        AtomicBoolean installed = new AtomicBoolean();
        List<String> submitted = new ArrayList<>();
        ZLinkBackendAdmissionKey key =
            ZLinkBackendAdmissionKey.boundSession(NODE, "actor-1", 7);

        CompletionStage<Void> first = submit(backend, key, installed, submitted, "first");
        CompletionStage<Void> second = submit(backend, key, installed, submitted, "second");
        assertEquals(List.of(), submitted, "both pushes park until the bind lands");

        installed.set(true);
        backend.ready.accept(key);

        first.toCompletableFuture().join();
        second.toCompletableFuture().join();
        assertEquals(List.of("first", "second"), submitted,
            "one bind edge releases the whole parked queue in order");
    }

    @Test
    void otherKindsStillReleaseOneSubmissionPerReadyEdge() {
        Backend backend = new Backend();
        AtomicBoolean ready = new AtomicBoolean();
        List<String> submitted = new ArrayList<>();
        ZLinkBackendAdmissionKey key =
            ZLinkBackendAdmissionKey.actor(NODE, "actor-1", 7);

        CompletionStage<Void> first = submit(backend, key, ready, submitted, "first");
        submit(backend, key, ready, submitted, "second");

        ready.set(true);
        backend.ready.accept(key);

        first.toCompletableFuture().join();
        assertEquals(List.of("first"), submitted,
            "send capacity stays one submission per edge");
    }

    private static CompletionStage<Void> submit(
        Backend backend,
        ZLinkBackendAdmissionKey key,
        AtomicBoolean gate,
        List<String> submitted,
        String label) {
        return ZLinkAdmissionRuntime.factory(
                ignored -> backend,
                ignored -> Duration.ofSeconds(5),
                ignored -> 64,
                (ignored, handler) -> backend.ready = handler,
                (ignored, handler) -> { })
            .submit(
                backend,
                key,
                () -> {
                    if (!gate.get()) {
                        return false;
                    }
                    synchronized (submitted) {
                        submitted.add(label);
                    }
                    return true;
                },
                () -> { },
                Duration.ofSeconds(5));
    }

    private static final class Backend implements ZLinkBackendObject {
        private volatile Consumer<ZLinkBackendAdmissionKey> ready = key -> { };

        @Override public String name() {
            return "admission-drain";
        }

        @Override public void close() {
        }
    }
}
