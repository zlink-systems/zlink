package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.channels.ZLinkSendCall;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;

final class ZLinkSendPacketNameTest {
    private static final String CHANNEL = "send-allocation";
    private static final RoutingId TARGET = RoutingId.from("send-allocation-target");

    @Test
    void constructorNameReachesTheEnvelopeAndMetadataClonesShareTheSubmissionGate() {
        for (var kind : ZLinkSendAdmissionAllocationTest.Kind.values()) {
            try (var fixture = new ZLinkSendAdmissionAllocationTest.Fixture(kind)) {
                Message payload = Message.from("payload");
                var original = call(fixture, payload, "Selected");
                var clone = original.metadata("key", "value");
                assertTrue(payload.refCount() > 0);
                var admission = new CompletableFuture<Void>();
                fixture.submission = admission;
                var submitted = original.submit();
                assertFalse(submitted.toCompletableFuture().isDone());
                assertEquals(
                        "Selected",
                        ZLinkChannelEnvelope.decodeHeader(fixture.parts.getFirst(), false)
                                .messageName());
                assertSame(payload, fixture.parts.getLast());
                assertDuplicate(original);
                assertDuplicate(clone);
                assertDuplicate(original.metadata("after", "submit"));
                admission.complete(null);
                submitted.toCompletableFuture().join();
                assertThrows(IllegalStateException.class, payload::refCount);
            }
        }
    }

    @Test
    void absentPacketNameKeepsTheBarePayloadAndClosesItOnAdmission() {
        for (var kind : ZLinkSendAdmissionAllocationTest.Kind.values()) {
            try (var fixture = new ZLinkSendAdmissionAllocationTest.Fixture(kind)) {
                Message payload = Message.from("bare");
                call(fixture, payload, null).submit().toCompletableFuture().join();
                assertEquals(1, fixture.parts.size());
                assertSame(payload, fixture.parts.getFirst());
                assertThrows(IllegalStateException.class, payload::refCount);
            }
        }
    }

    @Test
    void namedCallClosesThePayloadWhenAdmissionFails() {
        for (var kind : ZLinkSendAdmissionAllocationTest.Kind.values()) {
            try (var fixture = new ZLinkSendAdmissionAllocationTest.Fixture(kind)) {
                Message payload = Message.from("rejected");
                var rejection = new IllegalStateException("rejected");
                fixture.submission = CompletableFuture.failedFuture(rejection);
                var original = call(fixture, payload, "Original");
                var failure =
                        assertThrows(
                                CompletionException.class,
                                () -> original.submit().toCompletableFuture().join());
                assertSame(rejection, failure.getCause());
                assertThrows(IllegalStateException.class, payload::refCount);
                assertDuplicate(original);
            }
        }
    }

    private static ZLinkSendCall call(
            ZLinkSendAdmissionAllocationTest.Fixture fixture, Message payload, String name) {
        return switch (fixture.kind) {
            case CLIENT, MESH_CHANNEL ->
                    new ChannelSendCall(fixture.runtime, CHANNEL, fixture.sockets, payload, name);
            default ->
                    new RouteSendCall(
                            fixture.runtime,
                            CHANNEL,
                            fixture.sockets,
                            TARGET,
                            payload,
                            name,
                            ZLinkChannelContentTypeFrame.DEFAULT_CONTENT_TYPE,
                            null);
        };
    }

    private static void assertDuplicate(ZLinkSendCall call) {
        var failure =
                assertThrows(
                        CompletionException.class,
                        () -> call.submit().toCompletableFuture().join());
        assertEquals(
                ZLinkFrameworkErrorKind.INVALID_OPERATION,
                assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
    }
}
