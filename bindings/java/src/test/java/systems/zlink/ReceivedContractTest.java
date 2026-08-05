package systems.zlink;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.core.RoutingId;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertNotSame;
import static org.junit.jupiter.api.Assertions.assertSame;

public class ReceivedContractTest {
    @Test
    public void constructorDoesNotAliasCallerArray() {
        try (Message first = Message.from("first");
             Message second = Message.from("second");
             Message replacement = Message.from("replacement")) {
            Message[] parts = {first, second};
            Received received = new Received((RoutingId) null, parts);
            parts[0] = replacement;

            assertSame(first, received.firstPart());
            assertSame(second, received.parts().get(1));
            assertNotSame(replacement, received.firstPart());

            received.close();
        }
    }
}
