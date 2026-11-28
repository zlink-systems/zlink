/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contract;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.function.Executable;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.messaging.TopicMessage;
import systems.zlink.contracts.sockets.RecvResult;
import systems.zlink.internal.NativeErrorCodes;

import static org.junit.jupiter.api.Assertions.assertAll;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

class FirstPartEmptyContractTest {
    @Test
    void newReceiveStorageRejectsEmptyResult() {
        try (var received = new Received()) {
            assertInvalidState(received::firstPart);
        }
    }

    @Test
    void receivedRejectsEmptyPartsArray() {
        try (var received = new Received((RoutingId) null, new Message[0])) {
            assertInvalidState(received::firstPart);
        }
    }

    @Test
    void topicMessageRejectsEmptyResult() {
        try (var topic = new TopicMessage()) {
            assertInvalidState(topic::firstPart);
        }
    }

    private static void assertInvalidState(Executable firstPart) {
        var error = assertThrows(ZlinkRecvException.class, firstPart);
        assertAll(
            () -> assertEquals(RecvResult.INVALID_STATE, error.getResult()),
            () -> assertEquals(NativeErrorCodes.EINVAL, error.getNativeErrno()));
    }
}
