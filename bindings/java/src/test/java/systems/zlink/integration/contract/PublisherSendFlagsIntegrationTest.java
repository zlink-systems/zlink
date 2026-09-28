/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.integration.contract;

import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.PublishOperation;
import systems.zlink.contracts.sockets.PubSocket;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.XPubSocket;
import systems.zlink.runtime.nativeapi.InternalAccess;

import java.util.function.Function;

class PublisherSendFlagsIntegrationTest {
    @Test
    void pubForwardsDefaultBlockingFlag() {
        TestSupport.assumeNative();
        try (Context context = Zlink.createContext();
                PubSocket pub = context.createPubSocket()) {
            assertBlockingFlagReachedSendPlane(pub::publish);
        }
    }

    @Test
    void xpubForwardsDefaultBlockingFlag() {
        TestSupport.assumeNative();
        try (Context context = Zlink.createContext();
                XPubSocket pub = context.createXPubSocket()) {
            assertBlockingFlagReachedSendPlane(pub::publish);
        }
    }

    private static void assertBlockingFlagReachedSendPlane(
            Function<String, PublishOperation> publish) {
        InternalAccess.enterCallback();
        try (Message message = Message.from("flags")) {
            assertThrows(
                    IllegalStateException.class,
                    () -> publish.apply("flags").message(message).submit());
            publish.apply("flags").message(message).flags(SendFlags.DONT_WAIT).submit();
        } finally {
            InternalAccess.leaveCallback();
        }
    }
}
