/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contract;

import java.nio.ByteBuffer;
import java.util.function.Consumer;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.SubmitResult;

import static org.junit.jupiter.api.Assertions.assertAll;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

final class ClosedMessageReadsContractTest {
    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void closedMessageReadsAsEmpty(boolean consumed) {
        checkClosedMessage(consumed, message -> {
            var destination = ByteBuffer.allocate(0);
            assertAll(
                () -> assertEquals(0, message.size()),
                () -> assertTrue(message.isEmpty()),
                () -> assertEquals(0, message.refCount()),
                () -> assertEquals(0, message.data().length),
                () -> assertEquals(0, message.toByteArray().length),
                () -> assertEquals("", message.toUtf8String()),
                () -> assertEquals(0, message.dataBuffer().remaining()),
                () -> assertEquals(0, message.mutableDataBuffer().remaining()),
                () -> assertEquals(0, message.copyTo(new byte[0])),
                () -> assertEquals(0, message.copyTo(destination)),
                () -> assertTrue(message.tryCopyTo(destination)),
                () -> assertThrows(IndexOutOfBoundsException.class,
                    () -> message.readByte(0)));
        });
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void copyingClosedMessageCreatesOwnedEmptyMessages(boolean consumed) {
        checkClosedMessage(consumed, message -> {
            try (var copy = message.copy();
                 var clone = message.clone();
                 var from = Message.from(message)) {
                assertTrue(copy.isEmpty());
                assertTrue(clone.isEmpty());
                assertTrue(from.isEmpty());
                assertEquals(1, copy.refCount());
                assertEquals(1, clone.refCount());
                assertEquals(1, from.refCount());
            }
        });
    }

    private static void checkClosedMessage(boolean consumed, Consumer<Message> check) {
        TestSupport.assumeNative();
        try (var context = Zlink.createContext();
             var dealer = context.createDealerSocket();
             var router = context.createRouterSocket();
             var received = new Received();
             var message = Message.from("payload")) {
            var endpoint = TestSupport.inprocEndpoint("closed-message-reads");
            router.bind(endpoint);
            dealer.connect(endpoint);
            if (consumed) {
                dealer.send().message(message).submit_sync();
                assertTrue(router.recv(received, RecvFlags.NONE));
                assertEquals("payload", received.firstPart().toUtf8String());
            } else {
                message.close();
            }

            var error = assertThrows(ZlinkSubmitException.class,
                () -> dealer.send().message(message).submit_sync());
            assertEquals(SubmitResult.INVALID_HANDLE, error.getResult());
            assertFalse(router.recv(received, RecvFlags.DONT_WAIT));
            check.accept(message);
        }
    }
}
