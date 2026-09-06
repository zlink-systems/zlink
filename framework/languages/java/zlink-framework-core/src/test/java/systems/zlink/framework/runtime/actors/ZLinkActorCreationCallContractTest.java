package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.function.Supplier;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.function.Executable;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;

final class ZLinkActorCreationCallContractTest {
    @Test
    void createRejectsDuplicateOptionsBeforeSubmissionAndPreservesFirstValues() {
        Fixture fixture = new Fixture();
        var call = fixture.runtime.create("actor-a", "player")
            .inMesh("mesh-a")
            .request("first")
            .timeout(Duration.ofSeconds(2));

        assertInvalidOperation(() -> call.inMesh("other-mesh"));
        assertInvalidOperation(() -> call.request("second"));
        assertInvalidOperation(() -> call.request(ZLinkMessage.of("third")));
        assertInvalidOperation(() -> call.timeout(Duration.ofSeconds(3)));
        assertEquals(0, fixture.submissions);

        fixture.completion.complete(fixture.result);
        assertSame(fixture.result, call.submit().toCompletableFuture().join());
        fixture.assertSubmission(false);
    }

    @Test
    void getOrCreateRejectsDuplicateOptionsBeforeSubmissionAndPreservesFirstValues() {
        Fixture fixture = new Fixture();
        var call = fixture.runtime.getOrCreate("actor-a", "player")
            .inMesh("mesh-a")
            .request(ZLinkMessage.of("first"))
            .timeout(Duration.ofSeconds(2));

        assertInvalidOperation(() -> call.inMesh("other-mesh"));
        assertInvalidOperation(() -> call.request(ZLinkMessage.of("second")));
        assertInvalidOperation(() -> call.request("third"));
        assertInvalidOperation(() -> call.timeout(Duration.ofSeconds(3)));
        assertEquals(0, fixture.submissions);

        fixture.completion.complete(fixture.result);
        assertSame(fixture.result, call.submit().toCompletableFuture().join());
        fixture.assertSubmission(true);
    }

    @Test
    void createRejectsResubmissionWhilePendingAndAfterCompletion() {
        Fixture fixture = new Fixture();
        var call = fixture.runtime.create("actor-a", "player");

        assertSingleSubmission(call::submit, fixture);
    }

    @Test
    void getOrCreateRejectsResubmissionWhilePendingAndAfterCompletion() {
        Fixture fixture = new Fixture();
        var call = fixture.runtime.getOrCreate("actor-a", "player");

        assertSingleSubmission(call::submit, fixture);
    }

    private static void assertSingleSubmission(
        Supplier<CompletionStage<ZLinkActorCreateResult>> submit,
        Fixture fixture) {
        CompletionStage<ZLinkActorCreateResult> first = submit.get();
        assertResubmissionRejected(submit);
        assertEquals(1, fixture.submissions);

        fixture.completion.complete(fixture.result);
        assertSame(fixture.result, first.toCompletableFuture().join());
        assertResubmissionRejected(submit);
        assertEquals(1, fixture.submissions);
    }

    private static void assertResubmissionRejected(
        Supplier<CompletionStage<ZLinkActorCreateResult>> submit) {
        CompletionException failure = assertThrows(
            CompletionException.class,
            () -> submit.get().toCompletableFuture().join());
        assertEquals(
            ZLinkFrameworkErrorKind.INVALID_OPERATION,
            assertInstanceOf(ZLinkFrameworkException.class, failure.getCause())
                .kind());
    }

    private static void assertInvalidOperation(Executable operation) {
        assertEquals(
            ZLinkFrameworkErrorKind.INVALID_OPERATION,
            assertThrows(ZLinkFrameworkException.class, operation).kind());
    }

    private static final class Fixture {
        final CompletableFuture<ZLinkActorCreateResult> completion =
            new CompletableFuture<>();
        final ZLinkActorCreateResult result = new ZLinkActorCreateResult.Created(
            new ActorRef("actor-a", 1, "mesh-a", RoutingId.from("node-a")),
            ZLinkMessage.empty());
        final ZLinkActorRuntime runtime;
        int submissions;
        String actorId;
        String actorType;
        ZLinkMessage request;
        boolean getOrCreate;
        Duration timeout;

        Fixture() {
            ZLinkInternalSpotNode node = (ZLinkInternalSpotNode)
                Proxy.newProxyInstance(
                    ZLinkInternalSpotNode.class.getClassLoader(),
                    new Class<?>[] {ZLinkInternalSpotNode.class},
                    (proxy, method, arguments) -> {
                        if (method.getName().equals("routingId")) {
                            return RoutingId.from("node-a");
                        }
                        throw new AssertionError("unexpected backend call: " + method);
                    });
            runtime = new ZLinkActorRuntime(
                node, Map.of(), Duration.ofSeconds(5),
                new ZLinkJsonMessageSerializer());
            runtime.setMeshName("mesh-a");
            runtime.setCreationSubmitter((id, type, message, get, deadline) -> {
                submissions++;
                actorId = id;
                actorType = type;
                request = message;
                getOrCreate = get;
                timeout = deadline;
                return completion;
            });
        }

        void assertSubmission(boolean expectedGetOrCreate) {
            assertEquals(1, submissions);
            assertEquals("actor-a", actorId);
            assertEquals("player", actorType);
            assertEquals("first", request.decode(String.class));
            assertEquals(expectedGetOrCreate, getOrCreate);
            assertEquals(Duration.ofSeconds(2), timeout);
        }
    }
}
