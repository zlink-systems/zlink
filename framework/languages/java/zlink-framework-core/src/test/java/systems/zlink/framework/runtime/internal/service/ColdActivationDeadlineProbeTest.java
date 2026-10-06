package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.util.Arrays;

final class ColdActivationDeadlineProbeTest {
    @Test
    void targetAcceptsCanonicalColdActivationDeadline() throws Exception {
        long deadline = System.currentTimeMillis() + 1000;
        byte[] cold =
                ServiceWireCodec.encodeInstanceRouteV1(
                        new ServiceWireCodec.InstanceRouteV1ColdActivation(
                                ServiceWireCodec.InstanceRouteKind.COLD_ACTIVATION,
                                new ServiceWireCodec.Rid(RoutingId.from("owner").toBytes()),
                                new ServiceWireCodec.NonzeroU64(1),
                                new ServiceWireCodec.Text8("spot"),
                                new ServiceWireCodec.Text8("mesh"),
                                new ServiceWireCodec.Text8("type"),
                                new ServiceWireCodec.Text8("descriptor"),
                                new ServiceWireCodec.NonzeroU64(deadline)),
                        null);
        assertEquals(
                deadline,
                assertInstanceOf(
                                ServiceWireCodec.InstanceRouteV1ColdActivation.class,
                                ServiceWireCodec.decodeInstanceRouteV1(cold, null))
                        .deadlineUnixMs()
                        .value());
        var codec = new ZLinkServiceM6BWireCodec();
        byte[] ready =
                codec.encodeInstanceSpotHeader(
                        new ZLinkServiceM6BWireCodec.InstanceSpotMessage(
                                0,
                                new ZLinkServiceM6BWireCodec.InstanceRouteFence(
                                        RoutingId.from("owner"),
                                        1,
                                        "spot",
                                        1,
                                        "owner",
                                        1,
                                        1,
                                        "version"),
                                true,
                                1,
                                RoutingId.from("source"),
                                null,
                                true,
                                1,
                                1,
                                1L));
        int oldRouteEnd = 8 + Short.toUnsignedInt(ByteBuffer.wrap(ready, 6, 2).getShort());
        var header = new ByteArrayOutputStream();
        header.write(ready, 0, 5);
        header.write(cold);
        header.write(ready, oldRouteEnd, ready.length - oldRouteEnd);
        assertDoesNotThrow(
                () -> codec.decodeInstanceSpotHeader(header.toByteArray()),
                "target decoder must accept canonical route kind 2 carrying the activation deadline");
        var request = codec.decodeInstanceSpotHeader(header.toByteArray());
        assertArrayEquals(header.toByteArray(), codec.encodeInstanceSpotHeader(request));
        byte[] send = Arrays.copyOf(header.toByteArray(), header.size() - Long.BYTES);
        send[send.length - 17] = 1;
        var decodedSend = codec.decodeInstanceSpotHeader(send);
        assertFalse(decodedSend.request());
        assertEquals(1, decodedSend.operationHigh());
        assertEquals(1, decodedSend.operationLow());
        Arrays.fill(send, send.length - 16, send.length, (byte) 0);
        assertThrows(
                ZLinkServiceWireException.class,
                () -> codec.decodeInstanceSpotHeader(send),
                "cold send must retain a nonzero operation identity");
    }
}
