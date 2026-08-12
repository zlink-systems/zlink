package systems.zlink.framework.runtime.streams;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.List;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;

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
        ZLinkStreamRuntime.BoundSessionSendOwner first =
            matchingOwner(accepted);
        ZLinkStreamRuntime.BoundSessionSendOwner second =
            matchingOwner(accepted);

        assertFalse(ZLinkStreamRuntime.dispatchBoundSessionSend(
            List.of(), TARGET, TARGET_GENERATION, COMMAND, PAYLOAD));
        assertEquals(0, accepted.get());

        assertTrue(ZLinkStreamRuntime.dispatchBoundSessionSend(
            List.of(first), TARGET, TARGET_GENERATION, COMMAND, PAYLOAD));
        assertEquals(1, accepted.get());

        assertFalse(ZLinkStreamRuntime.dispatchBoundSessionSend(
            List.of(first, second),
            TARGET,
            TARGET_GENERATION,
            COMMAND,
            PAYLOAD));
        assertEquals(1, accepted.get(),
            "an ambiguous command 36 must not enter either owner FIFO");
    }

    private static ZLinkStreamRuntime.BoundSessionSendOwner matchingOwner(
        AtomicInteger accepted) {
        return new ZLinkStreamRuntime.BoundSessionSendOwner() {
            @Override
            public boolean matches(
                RoutingId sourceNodeRid,
                long sourceNodeGeneration,
                ZLinkServiceM6BWireCodec.BoundSessionSend command) {
                return true;
            }

            @Override
            public boolean accept(
                RoutingId sourceNodeRid,
                long sourceNodeGeneration,
                ZLinkServiceM6BWireCodec.BoundSessionSend command,
                ZLinkServiceM6AWireCodec.ApplicationPayload payload) {
                accepted.incrementAndGet();
                return true;
            }
        };
    }
}
