package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;

import java.util.List;
import java.util.Optional;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;

final class ZLinkSpotAcceptedJournalTest {
    @Test
    void acceptedRouteRecordPreservesRelayIdentityMetadataAndParts() {
        ZLinkBackendReceived received = new ZLinkBackendReceived(
            ZLinkBackendRequestResult.OK,
            Optional.of(RoutingId.from("journal-node")),
            Optional.of("source-spot"),
            Optional.of(17L),
            new byte[0],
            ZLinkAcceptedJournalTestRecords.spot(
                "source-spot",
                "target-spot",
                17,
                "packet",
                java.util.Map.of(),
                new byte[] {6, 7}),
            List.of(Message.from("packet"), Message.from(new byte[] {6, 7})),
            null,
            () -> { });

        ZLinkSpotAcceptedJournal.Record record =
            ZLinkSpotAcceptedJournal.decode(
                ZLinkSpotAcceptedJournal.encode(received));

        assertEquals(received.result(), record.result());
        assertEquals(received.routingId(), record.routingId());
        assertEquals(received.spotId(), record.spotId());
        assertEquals(received.requestSeq(), record.requestSequence());
        assertArrayEquals(new byte[0], record.applicationMetadata());
        assertArrayEquals("packet".getBytes(java.nio.charset.StandardCharsets.UTF_8),
            record.parts().get(0));
        assertArrayEquals(new byte[] {6, 7}, record.parts().get(1));
        received.close();
    }
}
