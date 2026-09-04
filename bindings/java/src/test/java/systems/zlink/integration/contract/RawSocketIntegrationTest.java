/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.integration.contract;

import java.nio.charset.StandardCharsets;
import java.util.concurrent.CompletableFuture;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.PairSocket;
import systems.zlink.contracts.sockets.RecvFlags;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

public class RawSocketIntegrationTest {
    @Test
    public void installedCorePackageSupportsRawMessageRoundTrip() {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext();
             PairSocket sender = context.createPairSocket();
             PairSocket receiver = context.createPairSocket()) {
            String endpoint = TestSupport.inprocEndpoint("raw-integration");
            sender.bind(endpoint);
            receiver.connect(endpoint);

            byte[] payload = "raw-jvm-11".getBytes(StandardCharsets.UTF_8);
            try (Message message = Message.from(payload)) {
                sender.send().message(message).submit()
                    .toCompletableFuture().join();
            }

            try (Received received = new Received()) {
                assertTrue(receiver.recv(received, RecvFlags.NONE));
                assertArrayEquals(payload,
                    received.singlePartOrThrow().toByteArray());
            }
        }
    }

    @Test
    public void immediateSendSuccessCompletesStageBeforeSubmitReturns() {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext();
             PairSocket sender = context.createPairSocket();
             PairSocket receiver = context.createPairSocket();
             Message message = Message.from("immediate")) {
            String endpoint = TestSupport.inprocEndpoint(
                "immediate-admission");
            sender.bind(endpoint);
            receiver.connect(endpoint);

            CompletableFuture<Void> completion = sender.send()
                .message(message).submit().toCompletableFuture();
            assertTrue(completion.isDone(),
                "ordinary SEND success must complete inline without a completion");
            assertFalse(completion.isCompletedExceptionally());
        }
    }

    @Test
    public void installedCorePackageSupportsMultipartRoundTrip() {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext();
             PairSocket sender = context.createPairSocket();
             PairSocket receiver = context.createPairSocket()) {
            String endpoint = TestSupport.inprocEndpoint(
                "raw-multipart-integration");
            sender.bind(endpoint);
            receiver.connect(endpoint);

            try (Message first = Message.from("first");
                 Message second = Message.from("second")) {
                sender.send().message(first).message(second).submit()
                    .toCompletableFuture().join();
            }

            try (Received received = new Received()) {
                assertTrue(receiver.recv(received, RecvFlags.NONE));
                assertTrue(received.parts().size() == 2);
                assertArrayEquals("first".getBytes(StandardCharsets.UTF_8),
                    received.parts().get(0).toByteArray());
                assertArrayEquals("second".getBytes(StandardCharsets.UTF_8),
                    received.parts().get(1).toByteArray());
            }
        }
    }

    @Test
    public void callerProvidedNonBlockingReceiveKeepsStateAcrossNoData() {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext();
             PairSocket sender = context.createPairSocket();
             PairSocket receiver = context.createPairSocket();
             Received received = new Received()) {
            String endpoint = TestSupport.inprocEndpoint("raw-reuse-integration");
            sender.bind(endpoint);
            receiver.connect(endpoint);

            try (Message first = Message.from("first")) {
                sender.send().message(first).submit()
                    .toCompletableFuture().join();
            }
            assertTrue(receiver.recv(received, RecvFlags.DONT_WAIT));
            assertArrayEquals("first".getBytes(StandardCharsets.UTF_8),
                received.singlePartOrThrow().toByteArray());

            assertFalse(receiver.recv(received, RecvFlags.DONT_WAIT));
            assertArrayEquals("first".getBytes(StandardCharsets.UTF_8),
                received.singlePartOrThrow().toByteArray());

            try (Message second = Message.from("second")) {
                sender.send().message(second).submit()
                    .toCompletableFuture().join();
            }
            assertTrue(receiver.recv(received, RecvFlags.DONT_WAIT));
            assertArrayEquals("second".getBytes(StandardCharsets.UTF_8),
                received.singlePartOrThrow().toByteArray());

            try (Message third = Message.from("third");
                 Message fourth = Message.from("fourth")) {
                sender.send().message(third).message(fourth).submit()
                    .toCompletableFuture().join();
            }
            assertTrue(receiver.recv(received, RecvFlags.DONT_WAIT));
            assertTrue(received.parts().size() == 2);
            assertArrayEquals("third".getBytes(StandardCharsets.UTF_8),
                received.parts().get(0).toByteArray());
            assertArrayEquals("fourth".getBytes(StandardCharsets.UTF_8),
                received.parts().get(1).toByteArray());
        }
    }
}
