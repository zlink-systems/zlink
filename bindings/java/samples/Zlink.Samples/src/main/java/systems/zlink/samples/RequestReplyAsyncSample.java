package systems.zlink.samples;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.core.RoutingId;
import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

public final class RequestReplyAsyncSample {
    public static void main(String[] args) throws Exception {
// --8<-- [start:doc]
        SampleSupport.ensureNative();
        String endpoint = SampleSupport.tcpEndpoint();
        CountDownLatch requestHandled = new CountDownLatch(1);

        try (Context ctx = Zlink.createContext();
             RouterSocket routerSocket = ctx.createRouterSocket();
             DealerSocket dealerSocket = ctx.createDealerSocket();
             var routerMonitor = routerSocket.monitorOpen(
                 systems.zlink.contracts.eventing.MonitorEventType.CONNECTION_READY);
             var dealerMonitor = dealerSocket.monitorOpen(
                 systems.zlink.contracts.eventing.MonitorEventType.CONNECTION_READY)) {
            dealerSocket.setRoutingId(RoutingId.from("request-reply-client".getBytes()));
            routerSocket.bind(endpoint);
            dealerSocket.connect(endpoint);
            SampleSupport.waitConnected(routerMonitor, dealerMonitor);

            CompletableFuture<Void> replyHandled = new CompletableFuture<>();

            Thread routerThread = new Thread(() -> {
                try (systems.zlink.contracts.messaging.Received received = new systems.zlink.contracts.messaging.Received()) {
                    routerSocket.recv(received, systems.zlink.contracts.sockets.RecvFlags.NONE);
                    String request = SampleSupport.singleUtf8(received);
                    if (!SampleSupport.DEALER_REQUEST.equals(request)) {
                        throw new IllegalStateException("unexpected request: " + request);
                    }
                    if (received.replyToken().isEmpty()) {
                        throw new IllegalStateException("missing request sequence");
                    }
                    try (Message reply = Message.from(SampleSupport.DEALER_REPLY)) {
                        received.reply().message(reply).submit();
                    }
                    requestHandled.countDown();
                    replyHandled.complete(null);
                } catch (Throwable error) {
                    replyHandled.completeExceptionally(error);
                    throw error instanceof RuntimeException runtimeException
                        ? runtimeException
                        : new IllegalStateException(
                            "request reply async sample failed", error);
                }
            }, "request-reply-async-router");
            routerThread.start();

            java.util.concurrent.CompletionStage<Void> roundTrip;
            try (Message request = Message.from(SampleSupport.DEALER_REQUEST)) {
                roundTrip = dealerSocket.request()
                    .message(request)
                    .timeout(Duration.ofSeconds(2))
                    .submit()
                    .thenAccept(reply -> {
                        try {
                            String value = reply.get(0).toUtf8String();
                            if (!SampleSupport.DEALER_REPLY.equals(value)) {
                                throw new IllegalStateException("unexpected reply: " + value);
                            }
                        } finally {
                            Message.closeAll(reply);
                        }
                    });
            }

            roundTrip.toCompletableFuture().get(2, TimeUnit.SECONDS);
            replyHandled.get(2, TimeUnit.SECONDS);
            SampleSupport.await(requestHandled, "request reply async");
            System.out.println("[dealer-router/request-reply/async] send: \""
                + SampleSupport.DEALER_REQUEST + "\" -> recv: \""
                + SampleSupport.DEALER_REPLY + "\"");
        }
// --8<-- [end:doc]
    }
}
