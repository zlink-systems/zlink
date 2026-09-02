package systems.zlink.contract;

import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.PairSocket;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.StreamSocket;
import systems.zlink.contracts.sockets.SubSocket;
import systems.zlink.contracts.sockets.XSubSocket;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.sockets.SubmitResult;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

public class ReceivedContractTest {
    @Test
    public void receiveSurfaceHasNoLeaseEraEntryPoints() {
        for (Class<?> socketType : List.of(PairSocket.class,
            DealerSocket.class, RouterSocket.class, StreamSocket.class,
            SubSocket.class, XSubSocket.class)) {
            assertFalse(Arrays.stream(socketType.getMethods())
                .anyMatch(method -> method.getName().equals("recvRetained")
                    || method.getName().equals("subscribeRetained")));
        }
    }

    @Test
    public void recvReturnsAggregateWithRoutingIdAndMultipartView() {
        TestSupport.assumeNative();

        RoutingId dealerRid = RoutingId.from("dealer-a".getBytes(StandardCharsets.UTF_8));
        try (Context ctx = Zlink.createContext();
             RouterSocket router = ctx.createRouterSocket();
             DealerSocket dealer = ctx.createDealerSocket()) {
            dealer.setRoutingId(dealerRid);
            String endpoint = TestSupport.inprocEndpoint("received-contract");
            router.bind(endpoint);
            dealer.connect(endpoint);

            dealer.send()
                .message(Message.from("part-1"))
                .message(Message.from("part-2"))
                .submit()
                .toCompletableFuture().join();

            try (systems.zlink.contracts.messaging.Received inbound = new systems.zlink.contracts.messaging.Received()) {


                router.recv(inbound, systems.zlink.contracts.sockets.RecvFlags.NONE);
                assertTrue(inbound.getRoutingId().isPresent());
                assertArrayEquals(dealerRid.toBytes(),
                    inbound.getRoutingId().orElseThrow().toBytes());
                assertEquals(2, inbound.parts().size());
                assertFalse(inbound.isSinglePart());
                assertThrows(UnsupportedOperationException.class,
                    () -> inbound.parts().add(Message.from("x")));
                assertArrayEquals("part-1".getBytes(StandardCharsets.UTF_8),
                    inbound.firstPart().toByteArray());
                assertTrue(inbound.replyToken().isEmpty());
                ZlinkSubmitException ex = assertThrows(ZlinkSubmitException.class,
                    () -> inbound.reply()
                        .message(Message.from("ack"))
                        .submit());
                assertEquals(SubmitResult.INVALID_STATE, ex.getResult());
            }
        }
    }

    @Test
    public void routedRequestCompletionCanReplyToAnEarlierRouterRequest()
        throws Exception {
        TestSupport.assumeNative();

        RoutingId callerRid = RoutingId.from("completion-caller");
        RoutingId sourceRid = RoutingId.from("completion-source");
        RoutingId targetRid = RoutingId.from("completion-target");
        try (Context ctx = Zlink.createContext();
             RouterSocket caller = ctx.createRouterSocket();
             RouterSocket source = ctx.createRouterSocket();
             RouterSocket target = ctx.createRouterSocket()) {
            caller.setRoutingId(callerRid);
            source.setRoutingId(sourceRid);
            target.setRoutingId(targetRid);
            String sourceEndpoint = TestSupport.inprocEndpoint(
                "completion-source");
            String targetEndpoint = TestSupport.inprocEndpoint(
                "completion-target");
            source.bind(sourceEndpoint);
            caller.connect(sourceEndpoint);
            target.bind(targetEndpoint);
            source.connect(targetEndpoint);

            var callerPending = caller.request(sourceRid)
                .message(Message.from("caller-request"))
                .timeout(Duration.ofMillis(TestSupport.DEFAULT_TIMEOUT_MS))
                .submit();
            RoutingId replyTarget;
            systems.zlink.contracts.messaging.ReplyToken replySequence;
            try (Received inbound = new Received()) {
                assertTrue(source.recv(inbound, RecvFlags.NONE));
                replyTarget = inbound.getRoutingId().orElseThrow();
                replySequence = inbound.replyToken().orElseThrow();
                assertEquals("caller-request",
                    inbound.singlePartOrThrow().toUtf8String());
            }

            CompletableFuture<Void> forwardedTerminal = new CompletableFuture<>();
            AtomicReference<String> completionThread = new AtomicReference<>();
            var targetPending = source.request(targetRid)
                .message(Message.from("forwarded-request"))
                .timeout(Duration.ofMillis(TestSupport.DEFAULT_TIMEOUT_MS))
                .submit();
            targetPending.whenComplete((reply, failure) -> {
                completionThread.set(Thread.currentThread().getName());
                if (failure != null) {
                    forwardedTerminal.completeExceptionally(failure);
                    return;
                }
                try {
                    source.reply(replyTarget, replySequence)
                        .message(reply.getFirst())
                        .submit();
                    forwardedTerminal.complete(null);
                } catch (Throwable rejected) {
                    forwardedTerminal.completeExceptionally(rejected);
                } finally {
                    Message.closeAll(reply);
                }
            });

            try (Received inbound = new Received();
                 Message reply = Message.from("target-reply")) {
                assertTrue(target.recv(inbound, RecvFlags.NONE));
                inbound.reply().message(reply).submit();
            }

            forwardedTerminal.get(TestSupport.DEFAULT_TIMEOUT_MS,
                TimeUnit.MILLISECONDS);
            assertEquals("zlink-send-completion", completionThread.get());
            List<Message> reply = callerPending.toCompletableFuture().get(
                TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS);
            try {
                assertEquals("target-reply", reply.getFirst().toUtf8String());
            } finally {
                Message.closeAll(reply);
            }
        }
    }
}
