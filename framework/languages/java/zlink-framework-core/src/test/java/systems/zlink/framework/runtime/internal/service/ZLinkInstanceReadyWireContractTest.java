package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;

import java.nio.ByteBuffer;
import java.util.Arrays;

final class ZLinkInstanceReadyWireContractTest {
    @Test
    void readyInstanceCallProducesCanonicalRouteWithInstanceIntent() throws Exception {
        assertCanonicalIntent(true);
    }

    @Test
    void readyOrdinaryCallProducesCanonicalRouteWithoutInstanceIntent() throws Exception {
        assertCanonicalIntent(false);
    }

    private static void assertCanonicalIntent(boolean instanceIntent) throws Exception {
        var codec = new ZLinkServiceM6BWireCodec();
        var message =
                new ZLinkServiceM6BWireCodec.InstanceSpotMessage(
                        0,
                        new ZLinkServiceM6BWireCodec.InstanceRouteFence(
                                RoutingId.from("owner"),
                                1,
                                "instance",
                                2,
                                "owner",
                                3,
                                4,
                                "version"),
                        instanceIntent,
                        5,
                        RoutingId.from("source"),
                        null,
                        true,
                        6,
                        7,
                        8L);
        byte[] encoded = codec.encodeInstanceSpotHeader(message);
        int bodyLength = Short.toUnsignedInt(ByteBuffer.wrap(encoded, 6, 2).getShort());
        var route =
                assertInstanceOf(
                        ServiceWireCodec.InstanceRouteV1Ready.class,
                        ServiceWireCodec.decodeInstanceRouteV1(
                                Arrays.copyOfRange(encoded, 5, 8 + bodyLength), null));
        assertEquals(
                instanceIntent ? ServiceWireCodec.Bool8.TRUE : ServiceWireCodec.Bool8.FALSE,
                route.instanceIntent());
        assertEquals(message, codec.decodeInstanceSpotHeader(encoded));
    }
}
