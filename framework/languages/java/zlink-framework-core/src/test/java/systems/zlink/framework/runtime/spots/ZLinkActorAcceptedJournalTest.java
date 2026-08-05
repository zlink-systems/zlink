package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;

import java.util.EnumSet;
import java.util.Map;
import java.util.Optional;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

final class ZLinkActorAcceptedJournalTest {
    @Test
    void acceptedActorRecordPreservesHeaderAndPayload() {
        ZLinkStreamHeader header = new ZLinkStreamHeader(
            ZLinkStreamMessageKind.REQUEST,
            ZLinkStreamCodec.JSON,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            Optional.of(19L),
            "probe",
            Map.of("trace", "value"));
        try (Message payload = Message.from(new byte[] {1, 2, 3})) {
            ZLinkActorAcceptedJournal.Record record =
                ZLinkActorAcceptedJournal.decode(
                    ZLinkActorAcceptedJournal.encode(
                        "actor-1",
                        header,
                        payload,
                        ZLinkAcceptedJournalTestRecords.actor(
                            "actor-1",
                            19,
                            "probe",
                            Map.of("trace", "value"),
                            new byte[] {1, 2, 3})));

            assertEquals("actor-1", record.actorId());
            assertEquals(header, record.header());
            assertArrayEquals(new byte[] {1, 2, 3}, record.payload());
        }
    }
}
