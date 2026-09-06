/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.repro;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RouterSocket;

/**
 * Minimal reproduction, outside the bench harness: how many requests can one Java
 * ROUTER socket have outstanding at once?
 *
 * <p>It imports nothing from the bench: one context, two ROUTER sockets, an echo loop
 * on a dedicated thread, and N concurrent {@code request(...).submit()} calls whose
 * completions are counted. If this shows the same ceiling the bench shows, the harness
 * is not the cause.
 *
 * <pre>bench-repro &lt;outstanding&gt; [waitSeconds]</pre>
 */
public final class OutstandingRequestRepro {
    private static final RoutingId SERVER = RoutingId.from(
        "repro-server".getBytes(StandardCharsets.US_ASCII));
    private static final RoutingId CLIENT = RoutingId.from(
        "repro-client".getBytes(StandardCharsets.US_ASCII));

    private OutstandingRequestRepro() {
    }

    private static void runAgainstExternalServer(
        String endpoint, RoutingId peer, int outstanding, long waitSeconds)
        throws Exception {
        Context context = Zlink.createContext();
        RouterSocket client = context.createRouterSocket();
        client.setRoutingId(RoutingId.from(
            ("repro-ext-" + ProcessHandle.current().pid()).getBytes(StandardCharsets.US_ASCII)));
        client.options().mandatory(true);
        client.options().setConnectRoutingId(peer);
        client.connect(endpoint);

        byte[] envelope = ReproWire.REQUEST_ENVELOPE;
        client.request(peer)
            .message(Message.from(envelope))
            .message(Message.from(ReproWire.encodeBody(new byte[1024])))
            .timeout(Duration.ofSeconds(5)).submit()
            .toCompletableFuture().get(5, TimeUnit.SECONDS).forEach(Message::close);
        System.out.println("route established against " + endpoint);

        List<CompletableFuture<Integer>> pending = new ArrayList<>();
        for (int i = 0; i < outstanding; i++) {
            final int index = i;
            pending.add(client.request(peer)
                .message(Message.from(envelope))
                .message(Message.from(ReproWire.encodeBody(new byte[1024])))
                .timeout(Duration.ofSeconds(waitSeconds))
                .submit()
                .toCompletableFuture()
                .thenApply(parts -> {
                    parts.forEach(Message::close);
                    return index;
                })
                .exceptionally(error -> {
                    System.out.println("  request " + index + " failed: " + error);
                    return -1;
                }));
        }
        long deadline = System.nanoTime() + waitSeconds * 1_000_000_000L;
        while (System.nanoTime() < deadline
            && pending.stream().anyMatch(future -> !future.isDone())) {
            Thread.sleep(50);
        }
        long done = pending.stream().filter(CompletableFuture::isDone).count();
        long succeeded = pending.stream()
            .filter(CompletableFuture::isDone)
            .filter(future -> !future.isCompletedExceptionally())
            .map(CompletableFuture::join)
            .filter(value -> value >= 0)
            .count();
        System.out.println("external outstanding=" + outstanding
            + " done=" + done + "/" + outstanding + " succeeded=" + succeeded);
        client.close();
        context.close();
        System.exit(0);
    }

    public static void main(String[] args) throws Exception {
        int outstanding = args.length > 0 ? Integer.parseInt(args[0]) : 4;
        long waitSeconds = args.length > 1 ? Long.parseLong(args[1]) : 10;
        // With a third argument the repro talks to an ALREADY RUNNING server at that
        // endpoint (routing id in the fourth argument) instead of starting its own.
        // That is what separates "the client cannot hold N outstanding" from "this
        // particular server does not answer N".
        String externalEndpoint = args.length > 2 ? args[2] : null;
        RoutingId peer = args.length > 3
            ? RoutingId.from(args[3].getBytes(StandardCharsets.US_ASCII)) : SERVER;
        if (externalEndpoint != null) {
            runAgainstExternalServer(externalEndpoint, peer, outstanding, waitSeconds);
            return;
        }
        String endpoint = "tcp://127.0.0.1:5098";

        Context context = Zlink.createContext();
        RouterSocket server = context.createRouterSocket();
        server.setRoutingId(SERVER);
        server.options().mandatory(true);
        server.bind(endpoint);

        AtomicLong echoed = new AtomicLong();
        Thread echo = new Thread(() -> {
            try (Received received = new Received()) {
                while (true) {
                    try {
                        if (!server.recv(received, RecvFlags.NONE)) {
                            continue;
                        }
                        List<Message> parts = received.parts();
                        byte[] body = parts.get(parts.size() - 1).data();
                        if (received.replyToken().isPresent()) {
                            received.reply().message(Message.from(body)).submit();
                        } else {
                            received.send().message(Message.from(body)).submit();
                        }
                        echoed.incrementAndGet();
                    } catch (Exception error) {
                        System.err.println("echo failed: " + error);
                    } finally {
                        received.close();
                    }
                }
            }
        }, "repro-echo");
        echo.setDaemon(true);
        echo.start();

        RouterSocket client = context.createRouterSocket();
        client.setRoutingId(CLIENT);
        client.options().mandatory(true);
        client.options().setConnectRoutingId(SERVER);
        client.connect(endpoint);

        // One blocking request first, so the route is established before the batch.
        client.request(SERVER).message(Message.from("warm"))
            .timeout(Duration.ofSeconds(5)).submit()
            .toCompletableFuture().get(5, TimeUnit.SECONDS).forEach(Message::close);
        System.out.println("route established; echoed=" + echoed.get());

        List<CompletableFuture<Integer>> pending = new ArrayList<>();
        for (int i = 0; i < outstanding; i++) {
            final int index = i;
            pending.add(client.request(SERVER)
                .message(Message.from("req-" + index))
                .timeout(Duration.ofSeconds(waitSeconds))
                .submit()
                .toCompletableFuture()
                .thenApply(parts -> {
                    parts.forEach(Message::close);
                    return index;
                })
                .exceptionally(error -> {
                    System.out.println("  request " + index + " failed: " + error);
                    return -1;
                }));
        }

        long deadline = System.nanoTime() + waitSeconds * 1_000_000_000L;
        int completed = 0;
        while (System.nanoTime() < deadline) {
            completed = (int) pending.stream().filter(CompletableFuture::isDone).count();
            if (completed == outstanding) {
                break;
            }
            Thread.sleep(50);
        }
        completed = (int) pending.stream().filter(CompletableFuture::isDone).count();
        long succeeded = pending.stream()
            .filter(CompletableFuture::isDone)
            .filter(future -> !future.isCompletedExceptionally())
            .map(CompletableFuture::join)
            .filter(value -> value >= 0)
            .count();

        System.out.println("outstanding=" + outstanding
            + " done=" + completed + "/" + outstanding
            + " succeeded=" + succeeded
            + " serverEchoed=" + echoed.get());
        client.close();
        server.close();
        context.close();
        System.exit(0);
    }
}
