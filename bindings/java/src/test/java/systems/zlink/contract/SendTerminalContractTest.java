/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contract;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;

import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.PairSocket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.SendFlags;
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
                .submit(SendFlags.NONE);
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
                        .submit(SendFlags.DONT_WAIT);
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
}
