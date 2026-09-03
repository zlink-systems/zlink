package systems.zlink.framework.runtime.streams;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertSame;

import java.nio.charset.StandardCharsets;
import java.util.Optional;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamReceived;

final class ZLinkStreamPacketBoundaryTest {
    @Test
    void keepsOnePulledPacketAndItsOwnerWithoutAnotherAssembler() {
        RoutingId routingId = RoutingId.from("peer-a");
        Message header = Message.from("header".getBytes(StandardCharsets.UTF_8));
        Message body = Message.from("body".getBytes(StandardCharsets.UTF_8));
        AtomicInteger closes = new AtomicInteger();
        ZLinkBackendStreamReceived packet = new ZLinkBackendStreamReceived(
            Optional.of(routingId), header, body, () -> {
                header.close();
                body.close();
                closes.incrementAndGet();
            });

        assertEquals(Optional.of(routingId), packet.routingId());
        assertSame(header, packet.header());
        assertSame(body, packet.body());
        assertArrayEquals("header".getBytes(StandardCharsets.UTF_8),
            packet.header().toByteArray());
        assertArrayEquals("body".getBytes(StandardCharsets.UTF_8),
            packet.body().toByteArray());

        packet.close();
        packet.close();
        assertEquals(1, closes.get());
    }
}
