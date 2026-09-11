package systems.zlink.framework.runtime.internal.backend;

import static org.junit.jupiter.api.Assertions.assertEquals;

import java.util.AbstractList;
import java.util.ArrayList;
import java.util.List;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.binding.spot.ReadyRecord;
import systems.zlink.framework.runtime.internal.binding.spot.ReceiveRecord;

final class ZLinkMeshDispatchRecordOwnershipTest {
    @Test
    void retainedPartsStayLazyUntilTypedDispatchReadsThem() {
        try (RetainedParts parts = new RetainedParts()) {
            ZLinkMeshDispatchRecord record = new ZLinkMeshDispatchRecord(
                new ReadyRecord(null, 0, null, null),
                new ReceiveRecord(
                    null, 0, null, null, null, null, null, null, null, null,
                    null, null, null, 0, 0, 0, 2),
                parts);

            assertEquals(0, parts.materializedCount());
            assertEquals("header", record.parts().getFirst().toUtf8String());
            assertEquals(1, parts.materializedCount());
            record.close();
            assertEquals(1, parts.closedCount());
        }
    }

    private static final class RetainedParts extends AbstractList<Message>
        implements AutoCloseable {
        private final List<Message> materialized = new ArrayList<>();
        private int closed;

        @Override
        public Message get(int index) {
            Message value = Message.from(index == 0 ? "header" : "payload");
            materialized.add(value);
            return value;
        }

        @Override
        public int size() {
            return 2;
        }

        int materializedCount() {
            return materialized.size();
        }

        int closedCount() {
            return closed;
        }

        @Override
        public void close() {
            materialized.forEach(message -> {
                message.close();
                closed++;
            });
            materialized.clear();
        }
    }
}
