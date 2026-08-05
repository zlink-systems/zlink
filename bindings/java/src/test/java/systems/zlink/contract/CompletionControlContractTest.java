/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contract;

import java.time.Duration;
import java.util.List;
import java.util.Arrays;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.atomic.AtomicBoolean;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.eventing.PollEvents;
import systems.zlink.contracts.eventing.Poller;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RouterSocket;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assertions.assertThrows;

public class CompletionControlContractTest {
    @Test
    public void completionControlUsesExistingConnectionAndLeavesApplicationUnread() {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext();
             RouterSocket server = context.createRouterSocket();
             RouterSocket client = context.createRouterSocket();
             Poller poller = Zlink.createPoller()) {
            RoutingId serverRid = RoutingId.from("control-server");
            RoutingId clientRid = RoutingId.from("control-client");
            server.setRoutingId(serverRid);
            client.setRoutingId(clientRid);
            client.options().setConnectRoutingId(serverRid);

            AtomicReference<String> delivered = new AtomicReference<>();
            AtomicBoolean replacedHandlerCalled = new AtomicBoolean();
            server.setCompletionControlHandler((source, parts) -> {
                replacedHandlerCalled.set(true);
                parts.forEach(Message::close);
            });
            server.setCompletionControlHandler((source, parts) -> {
                try {
                    delivered.set(source.toString() + ":"
                        + parts.get(0).toUtf8String() + ":"
                        + parts.get(1).toUtf8String());
                } finally {
                    parts.forEach(Message::close);
                }
            });

            String endpoint = TestSupport.inprocEndpoint("completion-control");
            server.bind(endpoint);
            client.connect(endpoint);
            poller.add(server, 1L, PollEventFlags.POLLCOMPLETION);

            try (Message validFirstPart = Message.from("must-not-stage")) {
                assertThrows(NullPointerException.class,
                    () -> client.trySendCompletionControl(serverRid,
                        Arrays.asList(validFirstPart, null)));
                assertEquals("must-not-stage", validFirstPart.toUtf8String());
            }

            try (Message application = Message.from("application-unread")) {
                client.send(serverRid).message(application).submit();
            }
            try (Message command = Message.from("opaque-command");
                 Message generation = Message.from("generation-1")) {
                assertTrue(client.trySendCompletionControl(serverRid,
                    List.of(command, generation)));
                assertEquals("opaque-command", command.toUtf8String());
                assertEquals("generation-1", generation.toUtf8String());
            }

            PollEvents events = new PollEvents(1);
            assertEquals(1, poller.wait(events, Duration.ofSeconds(2)));
            TestSupport.awaitCondition(() -> delivered.get() != null);
            assertEquals("control-client:opaque-command:generation-1",
                delivered.get());
            assertEquals(false, replacedHandlerCalled.get());

            try (Received application = new Received()) {
                assertTrue(server.recv(application, RecvFlags.NONE));
                assertEquals("application-unread",
                    application.singlePartOrThrow().toUtf8String());
            }
        }
    }
}
