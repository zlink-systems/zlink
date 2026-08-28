/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contract;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.net.Socket;
import java.time.Duration;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.PairSocket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.StreamSocket;
import systems.zlink.contracts.sockets.SubmitResult;

class SendTerminalContractTest {
    @Test
    void synchronousTerminalAdmitsNormally() {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext();
             PairSocket sender = context.createPairSocket();
             PairSocket receiver = context.createPairSocket()) {
            String endpoint = TestSupport.inprocEndpoint("sync-send-admit");
            receiver.bind(endpoint);
            sender.connect(endpoint);

            sender.send().message(Message.from("sync"))
                .submit_sync(SendFlags.NONE);
            try (Received received = new Received()) {
                receiver.recv(received, RecvFlags.NONE);
                assertEquals("sync",
                    received.parts().getFirst().toUtf8String());
            }
        }
    }

    @Test
    void dontWaitReportsBackpressureImmediatelyWhenHwmIsFull() {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext();
             PairSocket sender = context.createPairSocket();
             PairSocket receiver = context.createPairSocket()) {
            sender.options().sendHwm(1L);
            receiver.options().recvHwm(1L);
            String endpoint = TestSupport.inprocEndpoint("sync-send-hwm");
            receiver.bind(endpoint);
            sender.connect(endpoint);

            ZlinkSubmitException failure = null;
            for (int attempt = 0; attempt < 10_000 && failure == null;
                 attempt++) {
                try {
                    sender.send().message(Message.from("fill-" + attempt))
                        .submit_sync(SendFlags.DONT_WAIT);
                } catch (ZlinkSubmitException error) {
                    failure = error;
                }
            }

            assertNotNull(failure, "DONT_WAIT must stop at the full HWM");
            assertEquals(SubmitResult.BACKPRESSURED, failure.getResult());
        }
    }

    @Test
    void asynchronousTerminalStillCompletesOnAdmission() throws Exception {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext();
             PairSocket sender = context.createPairSocket();
             PairSocket receiver = context.createPairSocket()) {
            String endpoint = TestSupport.inprocEndpoint("async-send-regression");
            receiver.bind(endpoint);
            sender.connect(endpoint);

            sender.send().message(Message.from("async")).submit()
                .toCompletableFuture().get(TestSupport.DEFAULT_TIMEOUT_MS,
                    TimeUnit.MILLISECONDS);
            try (Received received = new Received()) {
                receiver.recv(received, RecvFlags.NONE);
                assertEquals("async",
                    received.parts().getFirst().toUtf8String());
            }
        }
    }

    @Test
    void streamAsyncTerminalTargetsCallbackRoutingId() throws Exception {
        TestSupport.assumeNative();

        byte[] payload = "stream-async".getBytes(
            java.nio.charset.StandardCharsets.UTF_8);
        byte[] frame = streamFrame(payload);
        CountDownLatch received = new CountDownLatch(1);
        AtomicReference<RoutingId> route = new AtomicReference<>();
        AtomicReference<Throwable> callbackFailure = new AtomicReference<>();
        String endpoint = TestSupport.tcpEndpoint();
        int port = Integer.parseInt(
            endpoint.substring(endpoint.lastIndexOf(':') + 1));

        try (Context context = Zlink.createContext();
             StreamSocket server = context.createStreamSocket()) {
            server.options().notify(true);
            server.bind(endpoint);
            server.onPacket((routingId, header, body) -> {
                try {
                    if (routingId != null && body.size() != 0) {
                        route.set(RoutingId.from(routingId.toBytes()));
                        received.countDown();
                    }
                } catch (Throwable error) {
                    callbackFailure.compareAndSet(null, error);
                    received.countDown();
                }
            });

            try (Socket client = new Socket("127.0.0.1", port)) {
                client.setSoTimeout(TestSupport.DEFAULT_TIMEOUT_MS);
                client.getOutputStream().write(frame);
                client.getOutputStream().flush();
                assertTrue(received.await(TestSupport.DEFAULT_TIMEOUT_MS,
                    TimeUnit.MILLISECONDS), "STREAM callback timed out");
                assertNull(callbackFailure.get(),
                    "callback raised: " + callbackFailure.get());
                assertNotNull(route.get(), "STREAM routing id was missing");

                try (Message reply = Message.from(frame)) {
                    server.sendAsync(route.get())
                        .message(reply)
                        .timeout(Duration.ofMillis(
                            TestSupport.DEFAULT_TIMEOUT_MS))
                        .submit()
                        .toCompletableFuture()
                        .get(TestSupport.DEFAULT_TIMEOUT_MS,
                            TimeUnit.MILLISECONDS);
                }

                assertArrayEquals(frame,
                    client.getInputStream().readNBytes(frame.length));
            }
        }
    }

    private static byte[] streamFrame(byte[] payload) {
        byte[] frame = new byte[6 + payload.length];
        frame[2] = (byte) ((payload.length >>> 24) & 0xFF);
        frame[3] = (byte) ((payload.length >>> 16) & 0xFF);
        frame[4] = (byte) ((payload.length >>> 8) & 0xFF);
        frame[5] = (byte) (payload.length & 0xFF);
        System.arraycopy(payload, 0, frame, 6, payload.length);
        return frame;
    }
}
