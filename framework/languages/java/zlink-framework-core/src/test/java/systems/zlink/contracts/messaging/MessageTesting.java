package systems.zlink.contracts.messaging;

import static org.junit.jupiter.api.Assertions.assertEquals;

/** A consumed or closed Message reads as empty: size 0 and refCount 0. */
public final class MessageTesting {
    private MessageTesting() {}

    public static void assertConsumed(Message message) {
        assertEquals(0, message.size(), "consumed Message size");
        assertEquals(0, message.refCount(), "consumed Message refCount");
    }
}
