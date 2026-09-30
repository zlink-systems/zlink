package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.assertFalse;

import org.junit.jupiter.api.Test;

import java.util.concurrent.atomic.AtomicBoolean;

final class DefaultZLinkStreamConnectorTraceTest {
    @Test
    void disabledTraceDoesNotBuildMessage() {
        AtomicBoolean messageBuilt = new AtomicBoolean();

        DefaultZLinkStreamConnector.trace(() -> buildTraceMessage(messageBuilt));

        assertFalse(messageBuilt.get());
    }

    private static String buildTraceMessage(AtomicBoolean messageBuilt) {
        messageBuilt.set(true);
        return "test trace";
    }
}
