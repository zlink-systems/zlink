package systems.zlink.framework.runtime.streams;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.runtime.configuration.ZLinkDispatchOptionsRegistration;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;

import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkStreamRuntimeBoundSessionSendDemuxTest {
    private static final RoutingId TARGET = RoutingId.from("target-node");
    private static final long TARGET_GENERATION = 17;
    private static final ZLinkServiceM6BWireCodec.BoundSessionSend COMMAND =
            new ZLinkServiceM6BWireCodec.BoundSessionSend(
                    new ZLinkServiceM6BWireCodec.ActorRouteFence(
                            new ZLinkBackendActorRef(TARGET, "actor-a", 3),
                            TARGET_GENERATION,
                            29,
                            31),
                    37);
    private static final ZLinkServiceM6AWireCodec.ApplicationPayload PAYLOAD =
            new ZLinkServiceM6AWireCodec.ApplicationPayload(
                    "callback", "application/octet-stream", new byte[] {1});

    @Test
    void rawCommand36RequiresExactlyOneSessionOwner() {
        AtomicInteger accepted = new AtomicInteger();
        ZLinkStreamRuntime.BoundSessionSendOwner first = owner(true, accepted);
        ZLinkStreamRuntime.BoundSessionSendOwner second = owner(true, accepted);

        assertFalse(dispatch(List.of(), null));
        assertEquals(0, accepted.get());

        assertTrue(dispatch(List.of(first), null));
        assertEquals(1, accepted.get());

        assertFalse(dispatch(List.of(first, second), null));
        assertEquals(1, accepted.get(), "an ambiguous command 36 must not enter either owner FIFO");
    }

    @Test
    void rejectedCommand36IsTracedAndAdmittedOneIsNot() {
        AtomicInteger accepted = new AtomicInteger();
        ZLinkMessageFlowTracer flow = flow();

        assertFalse(dispatch(List.of(owner(false, accepted)), flow));
        assertEquals(1, flow.tracedCount(), "a push refused as not current is traced");

        assertTrue(dispatch(List.of(owner(true, accepted)), flow));
        assertEquals(1, flow.tracedCount(), "an admitted push adds no rejection trace");
    }

    private static boolean dispatch(
            List<? extends ZLinkStreamRuntime.BoundSessionSendOwner> owners,
            ZLinkMessageFlowTracer flow) {
        return ZLinkStreamRuntime.dispatchBoundSessionSend(owners, flow, TARGET, COMMAND, PAYLOAD)
                .toCompletableFuture()
                .join();
    }

    private static ZLinkMessageFlowTracer flow() {
        ZLinkDispatchOptionsRegistration options = new ZLinkDispatchOptionsRegistration();
        options.messageFlow(ZLinkMessageFlowLogMode.NORMAL);
        return new ZLinkMessageFlowTracer(
                options, ZLinkHandlerActivator.reflection(), Runnable::run);
    }

    private static ZLinkStreamRuntime.BoundSessionSendOwner owner(
            boolean current, AtomicInteger accepted) {
        return new ZLinkStreamRuntime.BoundSessionSendOwner() {
            @Override
            public boolean matches(ZLinkServiceM6BWireCodec.BoundSessionSend command) {
                return current;
            }

            @Override
            public CompletionStage<Boolean> acceptAsync(
                    ZLinkServiceM6BWireCodec.BoundSessionSend command,
                    ZLinkServiceM6AWireCodec.ApplicationPayload payload) {
                accepted.incrementAndGet();
                return CompletableFuture.completedFuture(true);
            }
        };
    }
}
