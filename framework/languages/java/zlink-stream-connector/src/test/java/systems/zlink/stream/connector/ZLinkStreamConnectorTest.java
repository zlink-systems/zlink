package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import io.netty.buffer.UnpooledByteBufAllocator;
import io.netty.handler.ssl.SslContextBuilder;
import io.netty.handler.ssl.SslHandler;
import io.netty.handler.ssl.util.InsecureTrustManagerFactory;

import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.Test;

import systems.zlink.contracts.messaging.Message;

import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.net.URI;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;

import javax.net.ssl.SSLHandshakeException;

final class ZLinkStreamConnectorTest {
    @Test
    void transportSurfaceMatchesContract() throws Exception {
        Class<?> transport = Class.forName("systems.zlink.stream.connector.ZLinkStreamTransport");
        assertEquals(
                List.of("TCP", "TLS", "WEB_SOCKET", "WEB_SOCKET_SECURE"),
                Arrays.stream(transport.getEnumConstants()).map(Object::toString).toList());
    }

    @Test
    void packetNameOverrideSurfaceMatchesContract() throws Exception {
        assertEquals(
                ZLinkStreamSendCall.class,
                ZLinkStreamSendCall.class.getMethod("packetName", String.class).getReturnType());
        assertEquals(
                ZLinkStreamRequestCall.class,
                ZLinkStreamRequestCall.class.getMethod("packetName", String.class).getReturnType());
        assertEquals(
                ZLinkTypedStreamSendCall.class,
                ZLinkTypedStreamSendCall.class
                        .getMethod("packetName", String.class)
                        .getReturnType());
        assertEquals(
                ZLinkTypedStreamRequestCall.class,
                ZLinkTypedStreamRequestCall.class
                        .getMethod("packetName", String.class)
                        .getReturnType());
    }

    @Test
    void packetNameOverrideWinsForRawAndTypedSendAndRequest() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    createConnector(server.options(ZLinkStreamDispatchMode.IMMEDIATE));
            ConnectorTestAwait.await(connector.connect());

            CompletableFuture<TcpStreamConnectorTestServer.ReceivedFrame> rawSend =
                    server.readFrameAsync();
            connector.send(payload("RawDefault", "send")).packetName("RawOverride").submit();
            assertEquals("RawOverride", rawSend.join().header().name());

            CompletableFuture<TcpStreamConnectorTestServer.ReceivedFrame> typedSend =
                    server.readFrameAsync();
            connector.send(new NamedPayload("send")).packetName("TypedOverride").submit();
            assertEquals("TypedOverride", typedSend.join().header().name());

            CompletableFuture<TcpStreamConnectorTestServer.ReceivedFrame> rawRequestFrame =
                    server.readFrameAsync();
            CompletableFuture<ZLinkStreamEncodedPayload> rawReply =
                    connector
                            .request(payload("RawRequestDefault", "request"))
                            .packetName("RawRequestOverride")
                            .submit()
                            .toCompletableFuture();
            TcpStreamConnectorTestServer.ReceivedFrame rawRequest = rawRequestFrame.join();
            assertEquals("RawRequestOverride", rawRequest.header().name());
            server.sendAsync(
                            TcpStreamConnectorTestServer.responseTo(
                                    rawRequest, "ignored", Map.of()),
                            TcpStreamConnectorTestServer.bytes("reply"))
                    .join();
            rawReply.join().payload().close();

            CompletableFuture<TcpStreamConnectorTestServer.ReceivedFrame> typedRequestFrame =
                    server.readFrameAsync();
            CompletableFuture<NamedPayload> typedReply =
                    connector
                            .request(new NamedPayload("request"))
                            .packetName("TypedRequestOverride")
                            .submit(NamedPayload.class)
                            .toCompletableFuture();
            TcpStreamConnectorTestServer.ReceivedFrame typedRequest = typedRequestFrame.join();
            assertEquals("TypedRequestOverride", typedRequest.header().name());
            server.sendAsync(
                            new ZLinkStreamWireProtocol.Header(
                                    ZLinkStreamWireProtocol.KIND_RESPONSE,
                                    ZLinkStreamWireProtocol.CODEC_JSON,
                                    ZLinkStreamWireProtocol.FLAG_HAS_REQUEST_SEQ,
                                    typedRequest.header().requestSeq(),
                                    "ignored",
                                    Map.of(),
                                    null),
                            TcpStreamConnectorTestServer.bytes("{\"value\":\"reply\"}"))
                    .join();
            assertEquals(new NamedPayload("reply"), typedReply.join());
        }
    }

    @Test
    void oneWayAndTypedCallSurfacesMatchTheJavaContract() throws Exception {
        assertEquals(
                CompletionStage.class,
                ZLinkStreamSendCall.class.getMethod("submit").getReturnType());
        assertEquals(
                CompletionStage.class,
                ZLinkTypedStreamSendCall.class.getMethod("submit").getReturnType());
        assertEquals(
                ZLinkTypedStreamSendCall.class,
                ZLinkStreamConnector.class.getMethod("send", Object.class).getReturnType());
        assertEquals(
                ZLinkTypedStreamRequestCall.class,
                ZLinkStreamConnector.class.getMethod("request", Object.class).getReturnType());
        assertTrue(
                Arrays.stream(ZLinkStreamSendCall.class.getMethods())
                        .anyMatch(method -> method.getName().equals("packetName")));
        assertTrue(
                Arrays.stream(ZLinkStreamRequestCall.class.getMethods())
                        .anyMatch(method -> method.getName().equals("packetName")));
        assertFalse(
                Arrays.stream(ZLinkStreamRequestCall.class.getMethods())
                        .anyMatch(method -> method.getName().equals("await")));
        assertFalse(
                Arrays.stream(ZLinkStreamConnector.class.getMethods())
                        .anyMatch(method -> method.getName().equals("await")));
    }

    private final List<ZLinkStreamConnector> connectors = new ArrayList<>();

    @AfterEach
    void closeConnectors() throws Exception {
        for (ZLinkStreamConnector connector : connectors) {
            ConnectorTestAwait.await(connector.close());
        }
        connectors.clear();
    }

    private ZLinkStreamConnector createConnector(ZLinkStreamConnectorOptions options) {
        ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(options);
        connectors.add(connector);
        return connector;
    }

    @Test
    void defaultNameResolverUsesPacketNameAnnotationValue() {
        assertEquals(
                "custom.packet",
                ZLinkStreamPacketNameResolver.defaultResolver().resolve(NamedPayload.class));
    }

    @Test
    void defaultNameResolverPreservesBlankPacketNameAnnotationValue() {
        assertEquals(
                "",
                ZLinkStreamPacketNameResolver.defaultResolver().resolve(BlankNamedPayload.class));
    }

    @Test
    void manualDispatchInvokesRegisteredHandlerOnlyWhenDispatched() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    createConnector(server.options(ZLinkStreamDispatchMode.MANUAL));
            AtomicInteger handled = new AtomicInteger();
            connector.on(
                    "Ping",
                    message -> {
                        handled.incrementAndGet();
                        assertEquals("Ping", message.packetName());
                        assertEquals("42", message.metadata().get("seq"));
                        message.payload().payload().close();
                        return CompletableFuture.completedFuture(null);
                    });

            ConnectorTestAwait.await(connector.connect());
            server.sendAsync(
                            new ZLinkStreamWireProtocol.Header(
                                    ZLinkStreamWireProtocol.KIND_SEND,
                                    ZLinkStreamWireProtocol.CODEC_RAW,
                                    ZLinkStreamWireProtocol.FLAG_HAS_METADATA,
                                    null,
                                    "Ping",
                                    Map.of("seq", "42"),
                                    null),
                            TcpStreamConnectorTestServer.bytes("hello"))
                    .join();

            TcpStreamConnectorTestServer.awaitCondition(
                    () -> connector.pendingDispatchCount() == 1);
            assertEquals(1, connector.pendingDispatchCount());
            assertEquals(1, connector.receivedCount("Ping"));
            assertEquals(0, handled.get());

            ConnectorTestAwait.await(connector.dispatch());

            assertEquals(0, connector.pendingDispatchCount());
            //  Common connector spec 32 10: receivedCount counts what
            //  arrived and dispatching it does not lower the count.
            assertEquals(1, connector.receivedCount("Ping"));
            assertEquals(1, handled.get());
        }
    }

    @Test
    void requestWritesFrameAndCorrelatesResponse() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    createConnector(server.options(ZLinkStreamDispatchMode.MANUAL));
            ConnectorTestAwait.await(connector.connect());

            var requestFrame = server.readFrameAsync();
            var replyFuture =
                    connector
                            .request(payload("Echo", "hello"))
                            .timeout(Duration.ofMillis(500))
                            .submit()
                            .toCompletableFuture();

            TcpStreamConnectorTestServer.ReceivedFrame request = requestFrame.join();
            assertEquals(ZLinkStreamWireProtocol.KIND_REQUEST, request.header().kind());
            assertEquals("Echo", request.header().name());
            assertEquals("hello", new String(request.payload(), StandardCharsets.UTF_8));

            server.sendAsync(
                            TcpStreamConnectorTestServer.responseTo(request, "Echo", Map.of()),
                            TcpStreamConnectorTestServer.bytes("reply"))
                    .join();

            ZLinkStreamEncodedPayload reply = replyFuture.join();
            try {
                assertEquals("Echo", reply.packetName());
                assertEquals(
                        "reply", new String(reply.payload().toByteArray(), StandardCharsets.UTF_8));
            } finally {
                reply.payload().close();
            }
        }
    }

    @Test
    void completedSendTailReentryKeepsTheSecondFrameBehindTheFirstWrite() throws Exception {
        DefaultZLinkStreamConnector connector =
                new DefaultZLinkStreamConnector(options(ZLinkStreamDispatchMode.IMMEDIATE));
        Field lifecycleField = DefaultZLinkStreamConnector.class.getDeclaredField("lifecycle");
        lifecycleField.setAccessible(true);
        Object lifecycle = lifecycleField.get(connector);
        AtomicBoolean firstWriteReturned = new AtomicBoolean();
        AtomicReference<Boolean> secondSawFirstReturn = new AtomicReference<>();
        AtomicReference<CompletionStage<Void>> reentrantSend = new AtomicReference<>();
        ZLinkStreamTransportConnection transport =
                (ZLinkStreamTransportConnection)
                        Proxy.newProxyInstance(
                                ZLinkStreamTransportConnection.class.getClassLoader(),
                                new Class<?>[] {ZLinkStreamTransportConnection.class},
                                (proxy, method, arguments) -> {
                                    if (!method.getName().equals("writeAsync")) {
                                        if (method.getName().equals("isOpen")) {
                                            return true;
                                        }
                                        return null;
                                    }
                                    String name =
                                            ZLinkStreamWireProtocol.decodeHeader(
                                                            ZLinkStreamWireProtocol.decodeFrame(
                                                                            (byte[]) arguments[0])
                                                                    .header())
                                                    .name();
                                    if (name.equals("first")) {
                                        reentrantSend.set(invokeSendFrame(connector, "second"));
                                        firstWriteReturned.set(true);
                                    } else {
                                        secondSawFirstReturn.set(firstWriteReturned.get());
                                    }
                                    return CompletableFuture.completedFuture(null);
                                });
        setField(lifecycle, "connection", transport);
        setField(lifecycle, "state", ZLinkStreamConnectionState.CONNECTED);

        CompletionStage<Void> first = invokeSendFrame(connector, "first");
        first.toCompletableFuture().join();
        reentrantSend.get().toCompletableFuture().join();

        assertEquals(
                Boolean.TRUE,
                secondSawFirstReturn.get(),
                "the reentered frame must start after the active write returns");
    }

    /** A proxy transport whose write of the frame named "first" completes only when released. */
    private static ZLinkStreamTransportConnection heldFirstWriteTransport(
            CompletableFuture<Void> firstWrite, List<String> written, AtomicBoolean closed) {
        return (ZLinkStreamTransportConnection)
                Proxy.newProxyInstance(
                        ZLinkStreamTransportConnection.class.getClassLoader(),
                        new Class<?>[] {ZLinkStreamTransportConnection.class},
                        (proxy, method, arguments) -> {
                            if (method.getName().equals("writeAsync")) {
                                String name =
                                        ZLinkStreamWireProtocol.decodeHeader(
                                                        ZLinkStreamWireProtocol.decodeFrame(
                                                                        (byte[]) arguments[0])
                                                                .header())
                                                .name();
                                written.add(name);
                                return name.equals("first")
                                        ? firstWrite
                                        : CompletableFuture.completedFuture(null);
                            }
                            if (method.getName().equals("close")) {
                                closed.set(true);
                            }
                            if (method.getName().equals("isOpen")) {
                                return !closed.get();
                            }
                            return null;
                        });
    }

    /** Spec 32 5.2: a Send completes once its frame is written to the transport. */
    @Test
    void sendCompletesOnlyAfterItsFrameIsWrittenToTheTransport() throws Exception {
        DefaultZLinkStreamConnector connector =
                new DefaultZLinkStreamConnector(options(ZLinkStreamDispatchMode.IMMEDIATE));
        Field lifecycleField = DefaultZLinkStreamConnector.class.getDeclaredField("lifecycle");
        lifecycleField.setAccessible(true);
        Object lifecycle = lifecycleField.get(connector);
        CompletableFuture<Void> firstWrite = new CompletableFuture<>();
        List<String> written = new ArrayList<>();
        AtomicBoolean closed = new AtomicBoolean();
        setField(lifecycle, "connection", heldFirstWriteTransport(firstWrite, written, closed));
        setField(lifecycle, "state", ZLinkStreamConnectionState.CONNECTED);

        CompletableFuture<Void> first =
                connector.send(payload("first", "a")).submit().toCompletableFuture();
        CompletableFuture<Void> second =
                connector.send(payload("second", "b")).submit().toCompletableFuture();

        assertEquals(List.of("first"), written);
        assertFalse(first.isDone(), "the first Send's frame write has not completed");
        assertFalse(second.isDone(), "the second Send's frame has not been written");

        firstWrite.complete(null);
        first.get(5, TimeUnit.SECONDS);
        second.get(5, TimeUnit.SECONDS);
        assertEquals(List.of("first", "second"), written);
        ConnectorTestAwait.await(connector.close());
    }

    /**
     * Spec 32 7, 9: close does not write frames not yet written and does not wait for the peer.
     * Their Sends and Requests fail with Disconnected, the reason is ClientClose and no reconnect
     * follows.
     */
    @Test
    void closeFailsUnwrittenFramesWithoutWaitingForThePeer() throws Exception {
        DefaultZLinkStreamConnector connector =
                new DefaultZLinkStreamConnector(
                        options(
                                URI.create("tcp://127.0.0.1:1"),
                                ZLinkStreamDispatchMode.IMMEDIATE,
                                64 * 1024,
                                64 * 1024,
                                true,
                                false,
                                ZLinkStreamCompression.NONE));
        Field lifecycleField = DefaultZLinkStreamConnector.class.getDeclaredField("lifecycle");
        lifecycleField.setAccessible(true);
        Object lifecycle = lifecycleField.get(connector);
        CompletableFuture<Void> firstWrite = new CompletableFuture<>();
        List<String> written = new ArrayList<>();
        AtomicBoolean closed = new AtomicBoolean();
        setField(lifecycle, "connection", heldFirstWriteTransport(firstWrite, written, closed));
        setField(lifecycle, "state", ZLinkStreamConnectionState.CONNECTED);

        CompletableFuture<Void> first =
                connector.send(payload("first", "a")).submit().toCompletableFuture();
        CompletableFuture<Void> second =
                connector.send(payload("second", "b")).submit().toCompletableFuture();
        CompletableFuture<?> request =
                connector
                        .request(payload("third", "c"))
                        .timeout(Duration.ofSeconds(30))
                        .submit()
                        .toCompletableFuture();

        //  The peer never lets the first write finish; close must still end.
        connector.close().submit().toCompletableFuture().get(5, TimeUnit.SECONDS);

        assertTrue(closed.get());
        assertEquals(List.of("first"), written);
        for (CompletableFuture<?> unwritten : List.of(second, request)) {
            CompletionException failure = assertThrows(CompletionException.class, unwritten::join);
            assertEquals(
                    ZLinkStreamErrorCode.DISCONNECTED,
                    ((ZLinkStreamException) failure.getCause()).errorCode());
        }
        assertTrue(first.isDone());
        assertEquals(ZLinkStreamConnectionState.CLOSED, connector.state());
        assertEquals(Optional.of(ZLinkStreamCloseReason.CLIENT_CLOSE), connector.closeReason());
        firstWrite.complete(null);
        assertEquals(ZLinkStreamConnectionState.CLOSED, connector.state());
    }

    /**
     * Spec 32 7: close closes the transport before it fails the frames it did not write, so no
     * frame whose Send failed with Disconnected reaches the peer. The write pump starts the second
     * frame on another thread; the second Send's failure lets that write continue, so the write
     * lands exactly after the queue reset and before anything close does after it.
     */
    @Test
    void closeNeverLetsThePeerReceiveAFrameWhoseSendFailedWithDisconnected() throws Exception {
        DefaultZLinkStreamConnector connector =
                new DefaultZLinkStreamConnector(options(ZLinkStreamDispatchMode.IMMEDIATE));
        Object lifecycle = lifecycleOf(connector);
        CompletableFuture<Void> firstWrite = new CompletableFuture<>();
        CountDownLatch secondWriteStarted = new CountDownLatch(1);
        CountDownLatch releaseSecondWrite = new CountDownLatch(1);
        AtomicBoolean closed = new AtomicBoolean();
        List<String> received = Collections.synchronizedList(new ArrayList<>());
        ZLinkStreamTransportConnection transport =
                (ZLinkStreamTransportConnection)
                        Proxy.newProxyInstance(
                                ZLinkStreamTransportConnection.class.getClassLoader(),
                                new Class<?>[] {ZLinkStreamTransportConnection.class},
                                (proxy, method, arguments) -> {
                                    switch (method.getName()) {
                                        case "writeAsync" -> {
                                            if (frameName(arguments[0]).equals("first")) {
                                                return firstWrite;
                                            }
                                            secondWriteStarted.countDown();
                                            assertTrue(
                                                    releaseSecondWrite.await(5, TimeUnit.SECONDS));
                                            //  A closed transport puts nothing on the wire.
                                            if (closed.get()) {
                                                return CompletableFuture.failedFuture(
                                                        new java.io.IOException("socket closed"));
                                            }
                                            received.add("second");
                                            return CompletableFuture.completedFuture(null);
                                        }
                                        case "close" -> closed.set(true);
                                        case "isOpen" -> {
                                            return !closed.get();
                                        }
                                        default -> {}
                                    }
                                    return null;
                                });
        setField(lifecycle, "connection", transport);
        setField(lifecycle, "state", ZLinkStreamConnectionState.CONNECTED);

        CompletableFuture<Void> first =
                connector.send(payload("first", "a")).submit().toCompletableFuture();
        CompletableFuture<Void> second =
                connector.send(payload("second", "b")).submit().toCompletableFuture();
        //  Finishing the first write on another thread makes that thread
        //  start the second write, which then waits inside the transport.
        Thread pump = new Thread(() -> firstWrite.complete(null));
        pump.start();
        assertTrue(secondWriteStarted.await(5, TimeUnit.SECONDS));
        second.whenComplete(
                (ignored, failure) -> {
                    releaseSecondWrite.countDown();
                    try {
                        pump.join(TimeUnit.SECONDS.toMillis(5));
                    } catch (InterruptedException interrupted) {
                        Thread.currentThread().interrupt();
                    }
                });

        connector.close().submit().toCompletableFuture().get(5, TimeUnit.SECONDS);

        assertNull(first.get(5, TimeUnit.SECONDS));
        CompletionException failure = assertThrows(CompletionException.class, second::join);
        assertEquals(
                ZLinkStreamErrorCode.DISCONNECTED,
                ((ZLinkStreamException) failure.getCause()).errorCode());
        assertEquals(List.of(), received, "a frame whose Send failed must not reach the peer");
    }

    /**
     * Spec 32 7, 9: the transport close ends the frame write in progress, and that write's failure
     * belongs to the connection ending, so its Send fails with Disconnected and not SendFailed.
     */
    @Test
    void closeEndsTheFrameBeingWrittenWithDisconnected() throws Exception {
        DefaultZLinkStreamConnector connector =
                new DefaultZLinkStreamConnector(options(ZLinkStreamDispatchMode.IMMEDIATE));
        Object lifecycle = lifecycleOf(connector);
        CompletableFuture<Void> heldWrite = new CompletableFuture<>();
        ZLinkStreamTransportConnection transport =
                (ZLinkStreamTransportConnection)
                        Proxy.newProxyInstance(
                                ZLinkStreamTransportConnection.class.getClassLoader(),
                                new Class<?>[] {ZLinkStreamTransportConnection.class},
                                (proxy, method, arguments) -> {
                                    switch (method.getName()) {
                                        case "writeAsync" -> {
                                            return heldWrite;
                                        }
                                        //  A socket close fails the write it
                                        //  was carrying, on the closing thread.
                                        case "close" ->
                                                heldWrite.completeExceptionally(
                                                        new java.io.IOException("socket closed"));
                                        case "isOpen" -> {
                                            return !heldWrite.isDone();
                                        }
                                        default -> {}
                                    }
                                    return null;
                                });
        setField(lifecycle, "connection", transport);
        setField(lifecycle, "state", ZLinkStreamConnectionState.CONNECTED);

        CompletableFuture<Void> send =
                connector.send(payload("held", "a")).submit().toCompletableFuture();
        connector.close().submit().toCompletableFuture().get(5, TimeUnit.SECONDS);

        CompletionException failure = assertThrows(CompletionException.class, send::join);
        assertEquals(
                ZLinkStreamErrorCode.DISCONNECTED,
                ((ZLinkStreamException) failure.getCause()).errorCode());
    }

    /** Spec 32 7: close completes an admitted connect attempt without waiting for its transport. */
    @Test
    void closeCompletesAnInFlightConnectAsDisconnected() throws Exception {
        DefaultZLinkStreamConnector connector =
                new DefaultZLinkStreamConnector(options(ZLinkStreamDispatchMode.IMMEDIATE));
        Object lifecycle = lifecycleOf(connector);
        CompletableFuture<Void> attempt = new CompletableFuture<>();
        setField(lifecycle, "state", ZLinkStreamConnectionState.CONNECTING);
        setField(lifecycle, "connectionAttempt", attempt);

        CompletableFuture<Void> connecting = connector.connect().submit().toCompletableFuture();
        ConnectorTestAwait.await(connector.close());
        CompletionException failure = assertThrows(CompletionException.class, connecting::join);
        assertEquals(
                ZLinkStreamErrorCode.DISCONNECTED,
                ((ZLinkStreamException) failure.getCause()).errorCode());
    }

    /**
     * Spec 32 9 and 12: a transport write failure is SendFailed for that write, ends the connection
     * as TransportError, and fails every other operation in progress as Disconnected.
     */
    @Test
    void transportWriteFailureEndsTheConnectionAndFailsOnlyItsWriteAsSendFailed() throws Exception {
        DefaultZLinkStreamConnector connector =
                new DefaultZLinkStreamConnector(
                        options(
                                URI.create("tcp://127.0.0.1:1"),
                                ZLinkStreamDispatchMode.IMMEDIATE,
                                64 * 1024,
                                64 * 1024,
                                false,
                                false,
                                ZLinkStreamCompression.LZ4));
        Object lifecycle = lifecycleOf(connector);
        AtomicInteger writes = new AtomicInteger();
        ZLinkStreamTransportConnection transport =
                (ZLinkStreamTransportConnection)
                        Proxy.newProxyInstance(
                                ZLinkStreamTransportConnection.class.getClassLoader(),
                                new Class<?>[] {ZLinkStreamTransportConnection.class},
                                (proxy, method, arguments) ->
                                        switch (method.getName()) {
                                            case "writeAsync" ->
                                                    writes.incrementAndGet() == 1
                                                            ? CompletableFuture.completedFuture(
                                                                    null)
                                                            : CompletableFuture.failedFuture(
                                                                    new java.io.IOException(
                                                                            "broken pipe"));
                                            case "isOpen" -> true;
                                            default -> null;
                                        });
        setField(lifecycle, "connection", transport);
        setField(lifecycle, "state", ZLinkStreamConnectionState.CONNECTED);
        CompletableFuture<ZLinkStreamReplyReceivedContext> replyContext = new CompletableFuture<>();
        connector.onReplyReceived(replyContext::complete);

        CompletableFuture<ZLinkStreamEncodedPayload> pending =
                connector.request(payload("Pending", "a")).submit().toCompletableFuture();
        CompletionException sendFailure =
                assertThrows(
                        CompletionException.class,
                        () ->
                                connector
                                        .send(payload("Ping", "b"))
                                        .submit()
                                        .toCompletableFuture()
                                        .join());
        CompletionException requestFailure = assertThrows(CompletionException.class, pending::join);

        assertEquals(
                ZLinkStreamErrorCode.SEND_FAILED,
                ((ZLinkStreamException) sendFailure.getCause()).errorCode());
        assertEquals(
                ZLinkStreamErrorCode.DISCONNECTED,
                ((ZLinkStreamException) requestFailure.getCause()).errorCode());
        assertEquals(
                ZLinkStreamErrorCode.DISCONNECTED,
                replyContext.get(5, TimeUnit.SECONDS).error().code());
        assertEquals(Optional.of(ZLinkStreamCloseReason.TRANSPORT_ERROR), connector.closeReason());
        assertEquals(ZLinkStreamConnectionState.DISCONNECTED, connector.state());
        ConnectorTestAwait.await(connector.close());
    }

    /**
     * Spec 32 7: closing the TLS transport does not wait for the peer. The server stops reading, so
     * the frames fill the socket buffers and a frame write stays in progress; close must still
     * release the socket at once instead of flushing close_notify behind that frame.
     */
    @Test
    void tlsCloseReleasesTheSocketWithoutWaitingForThePeerToRead() throws Exception {
        try (TlsStreamConnectorTestServer server = new TlsStreamConnectorTestServer()) {
            DefaultZLinkStreamConnector connector =
                    new DefaultZLinkStreamConnector(
                            server.options(ZLinkStreamDispatchMode.IMMEDIATE));
            ConnectorTestAwait.await(connector.connect());
            server.stopReading();
            String block = "A".repeat(60 * 1024);
            CompletableFuture<Void> stalled = null;
            for (int index = 0; index < 10_000 && stalled == null; index++) {
                CompletableFuture<Void> send =
                        connector.send(payload("Fill", block)).submit().toCompletableFuture();
                try {
                    send.get(1, TimeUnit.SECONDS);
                } catch (TimeoutException blocked) {
                    stalled = send;
                }
            }
            assertTrue(stalled != null, "the peer that stopped reading must stall a frame write");
            io.netty.channel.Channel socket =
                    (io.netty.channel.Channel)
                            fieldValue(fieldValue(lifecycleOf(connector), "connection"), "channel");

            connector.close().submit().toCompletableFuture().get(5, TimeUnit.SECONDS);

            //  With close_notify queued behind the stalled frame the socket
            //  stays open until the 3 s flush timeout ends.
            assertTrue(
                    socket.closeFuture().await(1, TimeUnit.SECONDS),
                    "the TLS socket must close without waiting for the peer");
            CompletionException failure = assertThrows(CompletionException.class, stalled::join);
            assertEquals(
                    ZLinkStreamErrorCode.DISCONNECTED,
                    ((ZLinkStreamException) failure.getCause()).errorCode());
        }
    }

    private static Object lifecycleOf(DefaultZLinkStreamConnector connector) throws Exception {
        return fieldValue(connector, "lifecycle");
    }

    private static Object fieldValue(Object target, String name) throws Exception {
        Field field = target.getClass().getDeclaredField(name);
        field.setAccessible(true);
        return field.get(target);
    }

    private static String frameName(Object frame) {
        return ZLinkStreamWireProtocol.decodeHeader(
                        ZLinkStreamWireProtocol.decodeFrame((byte[]) frame).header())
                .name();
    }

    @Test
    void sendAdmissionCannotPassAConnectionEnd() throws Exception {
        DefaultZLinkStreamConnector connector =
                new DefaultZLinkStreamConnector(options(ZLinkStreamDispatchMode.IMMEDIATE));
        Field lifecycleField = DefaultZLinkStreamConnector.class.getDeclaredField("lifecycle");
        lifecycleField.setAccessible(true);
        Object lifecycle = lifecycleField.get(connector);
        Field admissionLockField = lifecycle.getClass().getDeclaredField("connectionAttemptLock");
        admissionLockField.setAccessible(true);
        Object admissionLock = admissionLockField.get(lifecycle);
        AtomicInteger writes = new AtomicInteger();
        ZLinkStreamTransportConnection transport =
                (ZLinkStreamTransportConnection)
                        Proxy.newProxyInstance(
                                ZLinkStreamTransportConnection.class.getClassLoader(),
                                new Class<?>[] {ZLinkStreamTransportConnection.class},
                                (proxy, method, arguments) -> {
                                    if (method.getName().equals("writeAsync")) {
                                        writes.incrementAndGet();
                                        return CompletableFuture.completedFuture(null);
                                    }
                                    if (method.getName().equals("isOpen")) {
                                        return true;
                                    }
                                    return null;
                                });
        setField(lifecycle, "connection", transport);
        setField(lifecycle, "state", ZLinkStreamConnectionState.CONNECTED);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        CountDownLatch entering = new CountDownLatch(1);
        Thread sender =
                new Thread(
                        () -> {
                            entering.countDown();
                            try {
                                invokeSendFrame(connector, "stale").toCompletableFuture().join();
                            } catch (Throwable ex) {
                                failure.set(ex);
                            }
                        });

        synchronized (admissionLock) {
            sender.start();
            assertTrue(entering.await(5, TimeUnit.SECONDS));
            long until = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
            while (sender.getState() != Thread.State.BLOCKED
                    && writes.get() == 0
                    && failure.get() == null
                    && System.nanoTime() < until) {
                Thread.onSpinWait();
            }
            assertEquals(0, writes.get(), "send must wait for lifecycle admission");
            setField(lifecycle, "connection", null);
            setField(lifecycle, "state", ZLinkStreamConnectionState.CLOSED);
        }
        sender.join(TimeUnit.SECONDS.toMillis(5));
        assertFalse(sender.isAlive());
        assertEquals(0, writes.get());
        assertTrue(failure.get() != null);
        ConnectorTestAwait.await(connector.close());
    }

    private static CompletionStage<Void> invokeSendFrame(
            DefaultZLinkStreamConnector connector, String name) throws Exception {
        Method method =
                DefaultZLinkStreamConnector.class.getDeclaredMethod(
                        "sendFrame", ZLinkStreamWireProtocol.Header.class, byte[].class);
        method.setAccessible(true);
        return (CompletionStage<Void>)
                method.invoke(
                        connector,
                        new ZLinkStreamWireProtocol.Header(
                                ZLinkStreamWireProtocol.KIND_SEND,
                                ZLinkStreamWireProtocol.CODEC_RAW,
                                0,
                                null,
                                name,
                                Map.of(),
                                null),
                        new byte[0]);
    }

    private static void setField(Object target, String name, Object value) throws Exception {
        Field field = target.getClass().getDeclaredField(name);
        field.setAccessible(true);
        field.set(target, value);
    }

    @Test
    void sendBulkMetadataReplacesExistingMetadata() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    createConnector(server.options(ZLinkStreamDispatchMode.MANUAL));
            ConnectorTestAwait.await(connector.connect());

            var frame = server.readFrameAsync();
            connector
                    .send(payload("Meta", "hello"))
                    .metadata("old", "ignored")
                    .metadata(Map.of("trace", "abc", "tenant", "sample"))
                    .submit();

            TcpStreamConnectorTestServer.ReceivedFrame sent = frame.join();
            assertEquals(ZLinkStreamWireProtocol.KIND_SEND, sent.header().kind());
            assertEquals("Meta", sent.header().name());
            assertEquals(Map.of("trace", "abc", "tenant", "sample"), sent.header().metadata());
        }
    }

    @Test
    void encodedPayloadCodecIsWrittenToWireHeaderLikeDotnet() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    createConnector(server.options(ZLinkStreamDispatchMode.MANUAL));
            ConnectorTestAwait.await(connector.connect());

            var frame = server.readFrameAsync();
            connector
                    .send(
                            new ZLinkStreamEncodedPayload(
                                    "JsonPayload",
                                    Message.from("{\"ok\":true}"),
                                    Map.of(),
                                    ZLinkStreamCodec.JSON))
                    .submit();

            TcpStreamConnectorTestServer.ReceivedFrame sent = frame.join();
            assertEquals(ZLinkStreamWireProtocol.KIND_SEND, sent.header().kind());
            assertEquals(ZLinkStreamWireProtocol.CODEC_JSON, sent.header().codec());
            assertEquals("JsonPayload", sent.header().name());
        }
    }

    @Test
    void typedCallsRejectRawEncodedPayloadHiddenAsObject() {
        ZLinkStreamConnector connector = createConnector(options(ZLinkStreamDispatchMode.MANUAL));
        Object rawPayload = payload("RawPayload", "raw");

        assertEquals(
                ZLinkStreamErrorCode.VALIDATION_FAILED,
                assertThrows(ZLinkStreamException.class, () -> connector.send(rawPayload))
                        .errorCode());
        assertEquals(
                ZLinkStreamErrorCode.VALIDATION_FAILED,
                assertThrows(ZLinkStreamException.class, () -> connector.request(rawPayload))
                        .errorCode());
        ((ZLinkStreamEncodedPayload) rawPayload).payload().close();
    }

    @Test
    void requestBulkMetadataReplacesExistingMetadata() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    createConnector(server.options(ZLinkStreamDispatchMode.MANUAL));
            ConnectorTestAwait.await(connector.connect());

            var requestFrame = server.readFrameAsync();
            var replyFuture =
                    connector
                            .request(payload("MetaRequest", "hello"))
                            .metadata("old", "ignored")
                            .metadata(Map.of("trace", "abc", "tenant", "sample"))
                            .timeout(Duration.ofMillis(500))
                            .submit()
                            .toCompletableFuture();

            TcpStreamConnectorTestServer.ReceivedFrame request = requestFrame.join();
            assertEquals(ZLinkStreamWireProtocol.KIND_REQUEST, request.header().kind());
            assertEquals("MetaRequest", request.header().name());
            assertEquals(Map.of("trace", "abc", "tenant", "sample"), request.header().metadata());

            server.sendAsync(
                            TcpStreamConnectorTestServer.responseTo(
                                    request, "MetaRequest", Map.of()),
                            TcpStreamConnectorTestServer.bytes("reply"))
                    .join();

            ZLinkStreamEncodedPayload reply = replyFuture.join();
            try {
                assertEquals("MetaRequest", reply.packetName());
            } finally {
                reply.payload().close();
            }
        }
    }

    @Test
    void lz4PicklerMatchesDotnetFixtures() {
        assertArrayEquals(
                hex("00636F6D70726573736564"),
                ZLinkStreamLz4Pickler.pickle(TcpStreamConnectorTestServer.bytes("compressed")));
        assertArrayEquals(
                TcpStreamConnectorTestServer.bytes("compressed"),
                ZLinkStreamLz4Pickler.unpickle(hex("00636F6D70726573736564")));
        assertArrayEquals(
                "A".repeat(1024).getBytes(StandardCharsets.UTF_8),
                ZLinkStreamLz4Pickler.unpickle(hex("80F2031F410100FFFFFFEA504141414141")));
    }

    @Test
    void sendCompressionIsExplicit() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    createConnector(
                            compressedOptions(server.endpoint(), ZLinkStreamDispatchMode.MANUAL));
            ConnectorTestAwait.await(connector.connect());

            var plainFrame = server.readFrameAsync();
            connector.send(payload("Plain", "A".repeat(1024))).submit();

            TcpStreamConnectorTestServer.ReceivedFrame plain = plainFrame.join();
            assertEquals(
                    0, plain.header().flags() & ZLinkStreamWireProtocol.FLAG_PAYLOAD_COMPRESSED);
            assertEquals("A".repeat(1024), new String(plain.payload(), StandardCharsets.UTF_8));

            var compressedFrame = server.readFrameAsync();
            connector.send(payload("Compressed", "A".repeat(1024))).compress().submit();

            TcpStreamConnectorTestServer.ReceivedFrame compressed = compressedFrame.join();
            assertTrue(
                    (compressed.header().flags() & ZLinkStreamWireProtocol.FLAG_PAYLOAD_COMPRESSED)
                            != 0);
            assertEquals(
                    "A".repeat(1024),
                    new String(
                            ZLinkStreamLz4Pickler.unpickle(compressed.payload()),
                            StandardCharsets.UTF_8));
        }
    }

    @Test
    void compressedSendUsesOriginalPayloadForMaxSizeValidation() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    createConnector(
                            compressedOptions(
                                    server.endpoint(), ZLinkStreamDispatchMode.MANUAL, 8));
            ConnectorTestAwait.await(connector.connect());

            CompletableFuture<Void> send =
                    connector
                            .send(payload("Compressed", "A".repeat(1024)))
                            .compress()
                            .submit()
                            .toCompletableFuture();
            CompletionException failure = assertThrows(CompletionException.class, send::join);
            assertEquals(
                    ZLinkStreamErrorCode.VALIDATION_FAILED,
                    ((ZLinkStreamException) failure.getCause()).errorCode());
        }
    }

    @Test
    void compressedResponseIsDecompressedBeforeCompletingRequest() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    createConnector(
                            compressedOptions(server.endpoint(), ZLinkStreamDispatchMode.MANUAL));
            ConnectorTestAwait.await(connector.connect());

            var requestFrame = server.readFrameAsync();
            var replyFuture =
                    connector
                            .request(payload("Echo", "hello"))
                            .timeout(Duration.ofMillis(500))
                            .submit()
                            .toCompletableFuture();

            TcpStreamConnectorTestServer.ReceivedFrame request = requestFrame.join();
            server.sendAsync(
                            new ZLinkStreamWireProtocol.Header(
                                    ZLinkStreamWireProtocol.KIND_RESPONSE,
                                    ZLinkStreamWireProtocol.CODEC_RAW,
                                    ZLinkStreamWireProtocol.FLAG_HAS_REQUEST_SEQ
                                            | ZLinkStreamWireProtocol.FLAG_PAYLOAD_COMPRESSED,
                                    request.header().requestSeq(),
                                    "Echo",
                                    Map.of(),
                                    null),
                            ZLinkStreamLz4Pickler.pickle(
                                    TcpStreamConnectorTestServer.bytes("compressed-reply")))
                    .join();

            ZLinkStreamEncodedPayload reply = replyFuture.join();
            try {
                assertEquals(
                        "compressed-reply",
                        new String(reply.payload().toByteArray(), StandardCharsets.UTF_8));
            } finally {
                reply.payload().close();
            }
        }
    }

    @Test
    void compressedResponseRejectsDecodedPayloadAboveReceiveLimit() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    createConnector(
                            options(
                                    server.endpoint(),
                                    ZLinkStreamDispatchMode.MANUAL,
                                    64 * 1024,
                                    2,
                                    false,
                                    false,
                                    ZLinkStreamCompression.LZ4));
            ConnectorTestAwait.await(connector.connect());

            var requestFrame = server.readFrameAsync();
            var replyFuture =
                    connector
                            .request(payload("Echo", "hello"))
                            .timeout(Duration.ofMillis(500))
                            .submit()
                            .toCompletableFuture();

            TcpStreamConnectorTestServer.ReceivedFrame request = requestFrame.join();
            server.sendAsync(
                            new ZLinkStreamWireProtocol.Header(
                                    ZLinkStreamWireProtocol.KIND_RESPONSE,
                                    ZLinkStreamWireProtocol.CODEC_RAW,
                                    ZLinkStreamWireProtocol.FLAG_HAS_REQUEST_SEQ
                                            | ZLinkStreamWireProtocol.FLAG_PAYLOAD_COMPRESSED,
                                    request.header().requestSeq(),
                                    "Echo",
                                    Map.of(),
                                    null),
                            new byte[] {0x40, 0x03})
                    .join();

            CompletionException ex = assertThrows(CompletionException.class, replyFuture::join);
            //  Common connector spec 32 4.7/9: a compressed inbound payload
            //  that decompresses over the receive limit is FrameTooLarge. It
            //  ends the connection as a protocol error and the pending
            //  request fails with Disconnected.
            assertTrue(ex.getCause() instanceof ZLinkStreamException);
            assertEquals(
                    ZLinkStreamErrorCode.DISCONNECTED,
                    ((ZLinkStreamException) ex.getCause()).errorCode());
            TcpStreamConnectorTestServer.awaitCondition(() -> connector.closeReason().isPresent());
            assertEquals(
                    Optional.of(ZLinkStreamCloseReason.PROTOCOL_ERROR), connector.closeReason());
        }
    }

    @Test
    void customCompressionCodecHandlesOutboundAndInboundPayloads() throws Exception {
        PrefixCompressionCodec codec = new PrefixCompressionCodec("java");
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    createConnector(
                            options(
                                    server.endpoint(),
                                    ZLinkStreamDispatchMode.IMMEDIATE,
                                    64 * 1024,
                                    64 * 1024,
                                    false,
                                    false,
                                    ZLinkStreamCompression.LZ4,
                                    codec));
            List<String> received = new ArrayList<>();
            connector.on(
                    "CustomInbound",
                    message -> {
                        received.add(
                                new String(
                                        message.payload().payload().toByteArray(),
                                        StandardCharsets.UTF_8));
                        message.payload().payload().close();
                        return CompletableFuture.completedFuture(null);
                    });
            ConnectorTestAwait.await(connector.connect());

            var sentFrame = server.readFrameAsync();
            connector.send(payload("CustomOutbound", "outbound")).compress().submit();

            TcpStreamConnectorTestServer.ReceivedFrame sent = sentFrame.join();
            assertTrue(
                    (sent.header().flags() & ZLinkStreamWireProtocol.FLAG_PAYLOAD_COMPRESSED) != 0);
            assertEquals("java:outbound", new String(sent.payload(), StandardCharsets.UTF_8));

            server.sendAsync(
                            new ZLinkStreamWireProtocol.Header(
                                    ZLinkStreamWireProtocol.KIND_SEND,
                                    ZLinkStreamWireProtocol.CODEC_RAW,
                                    ZLinkStreamWireProtocol.FLAG_PAYLOAD_COMPRESSED,
                                    null,
                                    "CustomInbound",
                                    Map.of(),
                                    null),
                            codec.compress(TcpStreamConnectorTestServer.bytes("inbound")))
                    .join();
            TcpStreamConnectorTestServer.awaitCondition(() -> received.size() == 1);

            assertEquals(List.of("inbound"), received);
        }
    }

    @Test
    void compressedSendLimitUsesCompressedPayloadSize() throws Exception {
        ZLinkStreamCompressionCodec codec =
                new ZLinkStreamCompressionCodec() {
                    @Override
                    public byte[] compress(byte[] payload) {
                        return new byte[] {1, 2};
                    }

                    @Override
                    public byte[] decompress(byte[] payload, int maxDecompressedSize) {
                        return payload;
                    }
                };
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    createConnector(
                            options(
                                    server.endpoint(),
                                    ZLinkStreamDispatchMode.IMMEDIATE,
                                    2,
                                    64 * 1024,
                                    false,
                                    false,
                                    ZLinkStreamCompression.LZ4,
                                    codec));
            ConnectorTestAwait.await(connector.connect());

            var sentFrame = server.readFrameAsync();
            connector
                    .send(payload("CompressedLimit", "larger-before-compression"))
                    .compress()
                    .submit();

            assertArrayEquals(new byte[] {1, 2}, sentFrame.join().payload());
        }
    }

    @Test
    void customDecompressionResultIsCheckedAgainstReceiveLimit() throws Exception {
        ZLinkStreamCompressionCodec codec =
                new ZLinkStreamCompressionCodec() {
                    @Override
                    public byte[] compress(byte[] payload) {
                        return payload;
                    }

                    @Override
                    public byte[] decompress(byte[] payload, int maxDecompressedSize) {
                        return new byte[maxDecompressedSize + 1];
                    }
                };
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    createConnector(
                            options(
                                    server.endpoint(),
                                    ZLinkStreamDispatchMode.MANUAL,
                                    64 * 1024,
                                    2,
                                    false,
                                    false,
                                    ZLinkStreamCompression.LZ4,
                                    codec));
            ConnectorTestAwait.await(connector.connect());

            var requestFrame = server.readFrameAsync();
            var replyFuture =
                    connector
                            .request(payload("Echo", "hello"))
                            .timeout(Duration.ofMillis(500))
                            .submit()
                            .toCompletableFuture();

            TcpStreamConnectorTestServer.ReceivedFrame request = requestFrame.join();
            server.sendAsync(
                            new ZLinkStreamWireProtocol.Header(
                                    ZLinkStreamWireProtocol.KIND_RESPONSE,
                                    ZLinkStreamWireProtocol.CODEC_RAW,
                                    ZLinkStreamWireProtocol.FLAG_HAS_REQUEST_SEQ
                                            | ZLinkStreamWireProtocol.FLAG_PAYLOAD_COMPRESSED,
                                    request.header().requestSeq(),
                                    "Echo",
                                    Map.of(),
                                    null),
                            new byte[] {1})
                    .join();

            CompletionException failedReply =
                    assertThrows(CompletionException.class, replyFuture::join);
            //  Spec 32 4.7/9: over the receive limit after decompression is
            //  FrameTooLarge, which ends the connection.
            assertEquals(
                    ZLinkStreamErrorCode.DISCONNECTED,
                    ((ZLinkStreamException) failedReply.getCause()).errorCode());
            TcpStreamConnectorTestServer.awaitCondition(() -> connector.closeReason().isPresent());
            assertEquals(
                    Optional.of(ZLinkStreamCloseReason.PROTOCOL_ERROR), connector.closeReason());
        }
    }

    @Test
    void lz4PicklerRejectsDecodedPayloadAboveReceiveLimit() {
        assertEquals(
                ZLinkStreamErrorCode.FRAME_TOO_LARGE,
                assertThrows(
                                ZLinkStreamException.class,
                                () -> ZLinkStreamLz4Pickler.unpickle(new byte[] {0x40, 0x03}, 2))
                        .errorCode());
    }

    @Test
    void webSocketRequestUsesBinaryFrameAndCorrelatesResponse() throws Exception {
        try (WebSocketStreamConnectorTestServer server = new WebSocketStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    createConnector(server.options(ZLinkStreamDispatchMode.MANUAL));
            ConnectorTestAwait.await(connector.connect());

            var requestFrame = server.readFrameAsync();
            var replyFuture =
                    connector
                            .request(payload("Echo", "hello"))
                            .timeout(Duration.ofMillis(500))
                            .submit()
                            .toCompletableFuture();

            TcpStreamConnectorTestServer.ReceivedFrame request = requestFrame.join();
            assertEquals(ZLinkStreamWireProtocol.KIND_REQUEST, request.header().kind());
            assertEquals("Echo", request.header().name());
            assertEquals("hello", new String(request.payload(), StandardCharsets.UTF_8));

            server.sendAsync(
                            TcpStreamConnectorTestServer.responseTo(request, "Echo", Map.of()),
                            TcpStreamConnectorTestServer.bytes("reply"))
                    .join();

            ZLinkStreamEncodedPayload reply = replyFuture.join();
            try {
                assertEquals("Echo", reply.packetName());
                assertEquals(
                        "reply", new String(reply.payload().toByteArray(), StandardCharsets.UTF_8));
            } finally {
                reply.payload().close();
            }
        }
    }

    @Test
    void tcpTransportRejectsOversizedInboundPayloadPrefix() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    createConnector(
                            options(
                                    server.endpoint(),
                                    ZLinkStreamDispatchMode.MANUAL,
                                    64 * 1024,
                                    1,
                                    false,
                                    false,
                                    ZLinkStreamCompression.NONE));
            ConnectorTestAwait.await(connector.connect());

            server.sendBytesAsync(framePrefix(0, 2)).join();

            TcpStreamConnectorTestServer.awaitCondition(
                    () -> connector.state() == ZLinkStreamConnectionState.DISCONNECTED);
        }
    }

    @Test
    void tlsRequestUsesEncryptedFrameAndSkippedCertificateValidation() throws Exception {
        try (TlsStreamConnectorTestServer server = new TlsStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    createConnector(server.options(ZLinkStreamDispatchMode.MANUAL));
            ConnectorTestAwait.await(connector.connect());

            var requestFrame = server.readFrameAsync();
            var replyFuture =
                    connector
                            .request(payload("Echo", "hello"))
                            .timeout(Duration.ofMillis(500))
                            .submit()
                            .toCompletableFuture();

            TcpStreamConnectorTestServer.ReceivedFrame request = requestFrame.join();
            assertEquals(ZLinkStreamWireProtocol.KIND_REQUEST, request.header().kind());
            assertEquals("Echo", request.header().name());
            assertEquals("hello", new String(request.payload(), StandardCharsets.UTF_8));

            server.sendAsync(
                            TcpStreamConnectorTestServer.responseTo(request, "Echo", Map.of()),
                            TcpStreamConnectorTestServer.bytes("reply"))
                    .join();

            ZLinkStreamEncodedPayload reply = replyFuture.join();
            try {
                assertEquals("Echo", reply.packetName());
                assertEquals(
                        "reply", new String(reply.payload().toByteArray(), StandardCharsets.UTF_8));
            } finally {
                reply.payload().close();
            }
        }
    }

    @Test
    void tlsConnectRejectsSelfSignedCertificateWhenValidationIsStrict() throws Exception {
        try (TlsStreamConnectorTestServer server = new TlsStreamConnectorTestServer()) {
            ZLinkStreamConnectorOptions strict =
                    new ZLinkStreamConnectorOptions(
                            server.endpoint(),
                            ZLinkStreamDispatchMode.IMMEDIATE,
                            Duration.ofSeconds(1),
                            1,
                            Duration.ofSeconds(1),
                            64 * 1024,
                            false,
                            Duration.ofMillis(25),
                            Duration.ofMillis(500),
                            false,
                            Duration.ofMillis(10),
                            Duration.ofMillis(250),
                            2.0,
                            false);
            ZLinkStreamConnector connector = createConnector(strict);

            assertThrows(
                    SSLHandshakeException.class,
                    () -> ConnectorTestAwait.await(connector.connect()));
            assertEquals(ZLinkStreamConnectionState.DISCONNECTED, connector.state());
        }
    }

    @Test
    void tlsTransportRejectsOversizedInboundPayloadPrefix() throws Exception {
        try (TlsStreamConnectorTestServer server = new TlsStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    createConnector(
                            options(
                                    server.endpoint(),
                                    ZLinkStreamDispatchMode.MANUAL,
                                    64 * 1024,
                                    1,
                                    false,
                                    true,
                                    ZLinkStreamCompression.NONE));
            ConnectorTestAwait.await(connector.connect());

            server.sendBytesAsync(framePrefix(0, 2)).join();

            TcpStreamConnectorTestServer.awaitCondition(
                    () -> connector.state() == ZLinkStreamConnectionState.DISCONNECTED);
        }
    }

    @Test
    void tlsHandlerEnablesHttpsEndpointIdentificationUnlessValidationIsSkipped() throws Exception {
        var sslContext =
                SslContextBuilder.forClient()
                        .trustManager(InsecureTrustManagerFactory.INSTANCE)
                        .build();

        SslHandler verified =
                ZLinkTlsTransportConnection.createSslHandler(
                        sslContext, UnpooledByteBufAllocator.DEFAULT, "localhost", 443, false);
        try {
            assertEquals(
                    "HTTPS",
                    verified.engine().getSSLParameters().getEndpointIdentificationAlgorithm());
        } finally {
            verified.engine().closeOutbound();
        }

        SslHandler skipped =
                ZLinkTlsTransportConnection.createSslHandler(
                        sslContext, UnpooledByteBufAllocator.DEFAULT, "localhost", 443, true);
        try {
            assertNull(skipped.engine().getSSLParameters().getEndpointIdentificationAlgorithm());
        } finally {
            skipped.engine().closeOutbound();
        }
    }

    @Test
    void wssRequestUsesBinaryFrameAndSkippedCertificateValidation() throws Exception {
        try (SecureWebSocketStreamConnectorTestServer server =
                new SecureWebSocketStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    createConnector(server.options(ZLinkStreamDispatchMode.MANUAL));
            ConnectorTestAwait.await(connector.connect());

            var requestFrame = server.readFrameAsync();
            var replyFuture =
                    connector
                            .request(payload("Echo", "hello"))
                            .timeout(Duration.ofMillis(500))
                            .submit()
                            .toCompletableFuture();

            TcpStreamConnectorTestServer.ReceivedFrame request = requestFrame.join();
            assertEquals(ZLinkStreamWireProtocol.KIND_REQUEST, request.header().kind());
            assertEquals("Echo", request.header().name());
            assertEquals("hello", new String(request.payload(), StandardCharsets.UTF_8));

            server.sendAsync(
                            TcpStreamConnectorTestServer.responseTo(request, "Echo", Map.of()),
                            TcpStreamConnectorTestServer.bytes("reply"))
                    .join();

            ZLinkStreamEncodedPayload reply = replyFuture.join();
            try {
                assertEquals("Echo", reply.packetName());
                assertEquals(
                        "reply", new String(reply.payload().toByteArray(), StandardCharsets.UTF_8));
            } finally {
                reply.payload().close();
            }
        }
    }

    @Test
    void webSocketTransportRejectsOversizedInboundPayloadPrefix() throws Exception {
        try (WebSocketStreamConnectorTestServer server = new WebSocketStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    createConnector(
                            options(
                                    server.endpoint(),
                                    ZLinkStreamDispatchMode.MANUAL,
                                    64 * 1024,
                                    1,
                                    false,
                                    false,
                                    ZLinkStreamCompression.NONE));
            ConnectorTestAwait.await(connector.connect());

            server.sendRawAsync(framePrefix(0, 2)).join();

            TcpStreamConnectorTestServer.awaitCondition(
                    () -> connector.state() == ZLinkStreamConnectionState.DISCONNECTED);
        }
    }

    @Test
    void requestWithoutReplyFailsWithTimeoutCause() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    createConnector(server.options(ZLinkStreamDispatchMode.MANUAL));
            ConnectorTestAwait.await(connector.connect());

            CompletionException ex =
                    assertThrows(
                            CompletionException.class,
                            () ->
                                    connector
                                            .request(payload("MissingReply", "hello"))
                                            .timeout(Duration.ofMillis(10))
                                            .submit()
                                            .toCompletableFuture()
                                            .join());

            assertTrue(ex.getCause() instanceof ZLinkStreamException);
            assertEquals(
                    ZLinkStreamErrorCode.REQUEST_TIMEOUT,
                    ((ZLinkStreamException) ex.getCause()).errorCode());
            assertTrue(ex.getCause().getCause() instanceof TimeoutException);
            assertEquals(0, connector.pendingDispatchCount());
        }
    }

    @Test
    void connectAndCloseUpdateState() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    createConnector(server.options(ZLinkStreamDispatchMode.IMMEDIATE));
            assertFalse(connector.isConnected());
            assertEquals(ZLinkStreamDispatchMode.IMMEDIATE, connector.options().dispatchMode());
            ConnectorTestAwait.await(connector.connect());
            assertTrue(connector.isConnected());
            ConnectorTestAwait.await(connector.close());
            assertEquals(ZLinkStreamConnectionState.CLOSED, connector.state());
        }
    }

    @Test
    void lifecycleHandlersObserveStateChangesAndDisconnect() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    createConnector(server.options(ZLinkStreamDispatchMode.IMMEDIATE));
            List<ZLinkStreamConnectionState> states = new ArrayList<>();
            AtomicInteger disconnected = new AtomicInteger();

            AutoCloseable stateRegistration =
                    connector.onConnectionStateChanged(
                            state -> {
                                states.add(state);
                                return CompletableFuture.completedFuture(null);
                            });
            AutoCloseable disconnectedRegistration =
                    connector.onDisconnected(
                            event -> {
                                assertEquals(
                                        ZLinkStreamCloseReason.CLIENT_CLOSE, event.closeReason());
                                disconnected.incrementAndGet();
                                return CompletableFuture.completedFuture(null);
                            });

            ConnectorTestAwait.await(connector.connect());
            ConnectorTestAwait.await(connector.close());

            assertEquals(
                    List.of(
                            ZLinkStreamConnectionState.CONNECTING,
                            ZLinkStreamConnectionState.CONNECTED,
                            ZLinkStreamConnectionState.CLOSED),
                    states);
            assertEquals(1, disconnected.get());

            stateRegistration.close();
            disconnectedRegistration.close();
            ConnectorTestAwait.await(connector.close());

            assertEquals(3, states.size());
            assertEquals(1, disconnected.get());
        }
    }

    @Test
    void sessionClosingPublishesServerDrainReasonBeforeDisconnect() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    createConnector(
                            server.options(
                                    ZLinkStreamDispatchMode.IMMEDIATE,
                                    Duration.ofSeconds(1),
                                    1,
                                    false,
                                    Duration.ofSeconds(1),
                                    Duration.ofSeconds(5),
                                    Duration.ofMillis(10)));
            AtomicReference<ZLinkStreamCloseReason> reason = new AtomicReference<>();
            CountDownLatch disconnected = new CountDownLatch(1);
            connector.onDisconnected(
                    event -> {
                        reason.set(event.closeReason());
                        disconnected.countDown();
                        return CompletableFuture.completedFuture(null);
                    });

            ConnectorTestAwait.await(connector.connect());
            server.sendAsync(
                            new ZLinkStreamWireProtocol.Header(
                                    ZLinkStreamWireProtocol.KIND_CONTROL,
                                    ZLinkStreamWireProtocol.CODEC_RAW,
                                    0,
                                    null,
                                    ZLinkSessionClosingControl.NAME,
                                    Map.of(),
                                    null),
                            new byte[] {1, 4, 0, 0})
                    .join();

            assertTrue(disconnected.await(5, TimeUnit.SECONDS));
            assertEquals(ZLinkStreamCloseReason.SERVER_DRAIN, reason.get());
        }
    }

    @Test
    void errorReceivedObservesInvalidHeaderAfterManualDispatch() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    createConnector(server.options(ZLinkStreamDispatchMode.MANUAL));
            AtomicReference<ZLinkStreamError> received = new AtomicReference<>();
            connector.onErrorReceived(
                    error -> {
                        received.set(error);
                        return CompletableFuture.completedFuture(null);
                    });

            ConnectorTestAwait.await(connector.connect());
            server.sendRawAsync(
                            "invalid-header".getBytes(StandardCharsets.UTF_8),
                            TcpStreamConnectorTestServer.bytes("payload"))
                    .join();

            TcpStreamConnectorTestServer.awaitCondition(
                    () -> connector.pendingDispatchCount() == 1);
            assertEquals(null, received.get());

            ConnectorTestAwait.await(connector.dispatch());

            assertEquals(ZLinkStreamErrorCode.FRAME_DECODE_FAILED, received.get().code());
        }
    }

    @Test
    void errorReceivedObservesRemoteErrorPacket() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    createConnector(server.options(ZLinkStreamDispatchMode.IMMEDIATE));
            AtomicReference<ZLinkStreamError> received = new AtomicReference<>();
            connector.onErrorReceived(
                    error -> {
                        received.set(error);
                        return CompletableFuture.completedFuture(null);
                    });

            ConnectorTestAwait.await(connector.connect());
            server.sendAsync(
                            new ZLinkStreamWireProtocol.Header(
                                    ZLinkStreamWireProtocol.KIND_ERROR,
                                    ZLinkStreamWireProtocol.CODEC_JSON,
                                    0,
                                    null,
                                    "RemoteError",
                                    Map.of(),
                                    null),
                            "{\"code\":\"quota_exceeded\",\"message\":\"remote failed\"}"
                                    .getBytes(StandardCharsets.UTF_8))
                    .join();

            TcpStreamConnectorTestServer.awaitCondition(() -> received.get() != null);

            assertEquals(ZLinkStreamErrorCode.REMOTE_ERROR, received.get().code());
            assertEquals("remote failed", received.get().message());
        }
    }

    @Test
    void errorPayloadRequiresAStringMessage() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    createConnector(server.options(ZLinkStreamDispatchMode.IMMEDIATE));
            AtomicReference<ZLinkStreamError> received = new AtomicReference<>();
            connector.onErrorReceived(
                    error -> {
                        received.set(error);
                        return CompletableFuture.completedFuture(null);
                    });

            ConnectorTestAwait.await(connector.connect());
            server.sendAsync(
                            new ZLinkStreamWireProtocol.Header(
                                    ZLinkStreamWireProtocol.KIND_ERROR,
                                    ZLinkStreamWireProtocol.CODEC_JSON,
                                    0,
                                    null,
                                    "",
                                    Map.of(),
                                    null),
                            "{\"code\":\"x\",\"message\":42}".getBytes(StandardCharsets.UTF_8))
                    .join();

            TcpStreamConnectorTestServer.awaitCondition(() -> received.get() != null);
            assertEquals(ZLinkStreamErrorCode.FRAME_DECODE_FAILED, received.get().code());
        }
    }

    @Test
    void correlatedRemoteErrorFailsOnlyItsPendingRequest() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    createConnector(server.options(ZLinkStreamDispatchMode.IMMEDIATE));
            AtomicInteger streamErrors = new AtomicInteger();
            connector.onErrorReceived(
                    error -> {
                        streamErrors.incrementAndGet();
                        return CompletableFuture.completedFuture(null);
                    });

            ConnectorTestAwait.await(connector.connect());
            var requestFrame = server.readFrameAsync();
            CompletableFuture<ZLinkStreamEncodedPayload> reply =
                    connector
                            .request(payload("RemoteFailure", "request"))
                            .submit()
                            .toCompletableFuture();
            TcpStreamConnectorTestServer.ReceivedFrame request = requestFrame.join();
            server.sendAsync(
                            new ZLinkStreamWireProtocol.Header(
                                    ZLinkStreamWireProtocol.KIND_ERROR,
                                    ZLinkStreamWireProtocol.CODEC_JSON,
                                    ZLinkStreamWireProtocol.FLAG_HAS_REQUEST_SEQ,
                                    request.header().requestSeq(),
                                    "",
                                    Map.of(),
                                    null),
                            "{\"code\":\"conflict\",\"message\":\"version conflict\"}"
                                    .getBytes(StandardCharsets.UTF_8))
                    .join();

            CompletionException failure = assertThrows(CompletionException.class, reply::join);
            assertEquals(
                    ZLinkStreamErrorCode.REMOTE_ERROR,
                    ((ZLinkStreamException) failure.getCause()).errorCode());
            assertEquals("version conflict", failure.getCause().getMessage());
            assertEquals(0, streamErrors.get());
        }
    }

    @Test
    void errorReceivedObservesUserCallbackFailure() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    createConnector(server.options(ZLinkStreamDispatchMode.IMMEDIATE));
            AtomicReference<ZLinkStreamError> received = new AtomicReference<>();
            connector.onErrorReceived(
                    error -> {
                        received.set(error);
                        return CompletableFuture.completedFuture(null);
                    });
            connector.on(
                    "Ping",
                    message -> {
                        throw new IllegalStateException("boom");
                    });

            ConnectorTestAwait.await(connector.connect());
            server.sendAsync(
                            new ZLinkStreamWireProtocol.Header(
                                    ZLinkStreamWireProtocol.KIND_SEND,
                                    ZLinkStreamWireProtocol.CODEC_RAW,
                                    0,
                                    null,
                                    "Ping",
                                    Map.of(),
                                    null),
                            TcpStreamConnectorTestServer.bytes("hello"))
                    .join();

            TcpStreamConnectorTestServer.awaitCondition(() -> received.get() != null);

            assertEquals(ZLinkStreamErrorCode.USER_CALLBACK_FAILED, received.get().code());
        }
    }

    @Test
    void errorCallbackFailureIsReportedToAnotherErrorHandler() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    createConnector(server.options(ZLinkStreamDispatchMode.IMMEDIATE));
            connector.onErrorReceived(
                    error -> {
                        throw new IllegalStateException("error handler failed");
                    });
            List<ZLinkStreamErrorCode> observed = Collections.synchronizedList(new ArrayList<>());
            connector.onErrorReceived(
                    error -> {
                        observed.add(error.code());
                        return CompletableFuture.completedFuture(null);
                    });

            ConnectorTestAwait.await(connector.connect());
            server.sendAsync(
                            new ZLinkStreamWireProtocol.Header(
                                    ZLinkStreamWireProtocol.KIND_ERROR,
                                    ZLinkStreamWireProtocol.CODEC_JSON,
                                    0,
                                    null,
                                    "",
                                    Map.of(),
                                    null),
                            "{\"code\":\"remote\",\"message\":\"failed\"}"
                                    .getBytes(StandardCharsets.UTF_8))
                    .join();

            TcpStreamConnectorTestServer.awaitCondition(
                    () ->
                            observed.contains(ZLinkStreamErrorCode.USER_CALLBACK_FAILED)
                                    && observed.contains(ZLinkStreamErrorCode.REMOTE_ERROR));
            assertTrue(observed.contains(ZLinkStreamErrorCode.REMOTE_ERROR));
        }
    }

    @Test
    void reservedPacketNamesAreRejectedForUserHandlers() throws Exception {
        ZLinkStreamConnector connector = createConnector(options(ZLinkStreamDispatchMode.MANUAL));
        try {
            assertEquals(
                    ZLinkStreamErrorCode.VALIDATION_FAILED,
                    assertThrows(
                                    ZLinkStreamException.class,
                                    () ->
                                            connector.on(
                                                    "$zlink.heartbeat",
                                                    message ->
                                                            CompletableFuture.completedFuture(
                                                                    null)))
                            .errorCode());
            assertEquals(
                    ZLinkStreamErrorCode.VALIDATION_FAILED,
                    assertThrows(
                                    ZLinkStreamException.class,
                                    () -> connector.send(payload("$zlink.send", "hello")))
                            .errorCode());
            assertEquals(
                    ZLinkStreamErrorCode.VALIDATION_FAILED,
                    assertThrows(
                                    ZLinkStreamException.class,
                                    () -> connector.request(payload("$zlink.request", "hello")))
                            .errorCode());
        } finally {
            ConnectorTestAwait.await(connector.close());
        }
    }

    @Test
    void packetNameLongerThanOneByteLengthIsRejected() throws Exception {
        ZLinkStreamConnector connector = createConnector(options(ZLinkStreamDispatchMode.MANUAL));
        try {
            String tooLong = "a".repeat(256);

            assertEquals(
                    ZLinkStreamErrorCode.VALIDATION_FAILED,
                    assertThrows(
                                    ZLinkStreamException.class,
                                    () ->
                                            connector.on(
                                                    tooLong,
                                                    message ->
                                                            CompletableFuture.completedFuture(
                                                                    null)))
                            .errorCode());
            assertEquals(
                    ZLinkStreamErrorCode.VALIDATION_FAILED,
                    assertThrows(
                                    ZLinkStreamException.class,
                                    () -> connector.send(payload(tooLong, "hello")))
                            .errorCode());
            assertEquals(
                    ZLinkStreamErrorCode.VALIDATION_FAILED,
                    assertThrows(
                                    ZLinkStreamException.class,
                                    () -> connector.request(payload(tooLong, "hello")))
                            .errorCode());
        } finally {
            ConnectorTestAwait.await(connector.close());
        }
    }

    @Test
    void uriSchemeAndTransportMismatchIsRejected() {
        assertEquals(
                ZLinkStreamErrorCode.CONFIGURATION_ERROR,
                assertThrows(
                                ZLinkStreamException.class,
                                () ->
                                        createConnector(
                                                new ZLinkStreamConnectorOptions(
                                                        URI.create("http://127.0.0.1:7000"),
                                                        ZLinkStreamDispatchMode.MANUAL,
                                                        Duration.ofSeconds(1),
                                                        1)))
                        .errorCode());
        assertEquals(
                ZLinkStreamErrorCode.CONFIGURATION_ERROR,
                assertThrows(
                                ZLinkStreamException.class,
                                () ->
                                        createConnector(
                                                //  No scheme at all: java.net.URI rejects
                                                // "127.0.0.1:7000"
                                                //  itself, so the authority-only form is what
                                                // reaches the
                                                //  connector's own scheme check.
                                                new ZLinkStreamConnectorOptions(
                                                        URI.create("//127.0.0.1:7000"),
                                                        ZLinkStreamDispatchMode.MANUAL,
                                                        Duration.ofSeconds(1),
                                                        1)))
                        .errorCode());
    }

    @Test
    void skipServerCertificateValidationDefaultsToFalseAndCanBeEnabled() {
        assertFalse(
                new ZLinkStreamConnectorOptions(
                                URI.create("wss://127.0.0.1:7000"),
                                ZLinkStreamDispatchMode.MANUAL,
                                Duration.ofSeconds(1),
                                1)
                        .skipServerCertificateValidation());

        assertTrue(
                new ZLinkStreamConnectorOptions(
                                URI.create("wss://127.0.0.1:7000"),
                                ZLinkStreamDispatchMode.MANUAL,
                                Duration.ofSeconds(1),
                                1,
                                Duration.ofSeconds(1),
                                64 * 1024,
                                false,
                                Duration.ofMillis(25),
                                Duration.ofMillis(500),
                                true,
                                Duration.ofMillis(10),
                                Duration.ofMillis(250),
                                2.0,
                                true)
                        .skipServerCertificateValidation());
    }

    @Test
    void defaultOptionsMatchDotnetConnectorDefaults() {
        URI endpoint = URI.create("tcp://127.0.0.1:7000");
        ZLinkStreamConnectorOptions options = ZLinkStreamConnectorOptions.createDefault(endpoint);

        assertEquals(endpoint, options.endpoint());
        assertEquals(ZLinkStreamDispatchMode.MANUAL, options.dispatchMode());
        assertEquals(Duration.ofSeconds(30), options.requestTimeout());
        assertEquals(3, options.maxReconnectAttempts());
        assertEquals(Duration.ofSeconds(5), options.connectTimeout());
        assertEquals(64 * 1024, options.maxSendPayloadSize());
        assertEquals(64 * 1024, options.maxReceivePayloadSize());
        assertTrue(options.heartbeatEnabled());
        assertEquals(Duration.ofSeconds(1), options.heartbeatInterval());
        assertEquals(Duration.ofSeconds(5), options.heartbeatTimeout());
        assertTrue(options.reconnectEnabled());
        assertEquals(Duration.ofMillis(250), options.reconnectInitialDelay());
        assertEquals(Duration.ofSeconds(5), options.reconnectMaxDelay());
        assertEquals(2.0, options.reconnectBackoffFactor());
        assertFalse(options.skipServerCertificateValidation());
        assertEquals(ZLinkStreamCompression.LZ4, options.compression());
        assertEquals(ZLinkStreamCompressionCodecs.lz4(), options.compressionCodec());
        assertEquals("custom.packet", options.nameResolver().resolve(NamedPayload.class));
    }

    @Test
    void configurationAcceptsUppercaseEndpointScheme() {
        // Endpoint notation policy §2.6 / common connector spec §3.1:
        // scheme resolution is case-insensitive. transportFor() used to
        // switch on the raw (unlowered) scheme string, so "TCP://" threw
        // IllegalArgumentException instead of resolving to TCP.
        ZLinkStreamConnectorOptions uppercase =
                ZLinkStreamConnectorOptions.createDefault(URI.create("TCP://127.0.0.1:7000"));
        ZLinkStreamConnectorConfiguration configuration =
                ZLinkStreamConnectorConfiguration.from(uppercase);
        assertEquals(ZLinkStreamTransport.TCP, configuration.transport().kind());

        ZLinkStreamConnectorOptions mixedCaseWss =
                ZLinkStreamConnectorOptions.createDefault(URI.create("Wss://127.0.0.1:7443"));
        assertEquals(
                ZLinkStreamTransport.WEB_SOCKET_SECURE,
                ZLinkStreamConnectorConfiguration.from(mixedCaseWss).transport().kind());
    }

    @Test
    void reconnectOptionsAcceptEveryPositiveFactorAndRequirePositiveAttemptCount() {
        URI endpoint = URI.create("tcp://127.0.0.1:7000");
        ZLinkStreamConnectorOptions subunitFactor =
                new ZLinkStreamConnectorOptions(
                        endpoint,
                        ZLinkStreamDispatchMode.MANUAL,
                        Duration.ofSeconds(1),
                        1,
                        Duration.ofSeconds(1),
                        64 * 1024,
                        false,
                        Duration.ofSeconds(1),
                        Duration.ofSeconds(5),
                        false,
                        Duration.ofMillis(10),
                        Duration.ofSeconds(1),
                        0.5);
        ZLinkStreamConnectorConfiguration.from(subunitFactor);

        for (double factor : new double[] {0.0, -1.0, Double.NaN, Double.POSITIVE_INFINITY}) {
            ZLinkStreamConnectorOptions invalidFactor =
                    new ZLinkStreamConnectorOptions(
                            endpoint,
                            ZLinkStreamDispatchMode.MANUAL,
                            Duration.ofSeconds(1),
                            1,
                            Duration.ofSeconds(1),
                            64 * 1024,
                            false,
                            Duration.ofSeconds(1),
                            Duration.ofSeconds(5),
                            false,
                            Duration.ofMillis(10),
                            Duration.ofSeconds(1),
                            factor);
            assertEquals(
                    ZLinkStreamErrorCode.VALIDATION_FAILED,
                    assertThrows(
                                    ZLinkStreamException.class,
                                    () -> ZLinkStreamConnectorConfiguration.from(invalidFactor))
                            .errorCode());
        }

        ZLinkStreamConnectorOptions zeroAttempts =
                new ZLinkStreamConnectorOptions(
                        endpoint,
                        ZLinkStreamDispatchMode.MANUAL,
                        Duration.ofSeconds(1),
                        0,
                        Duration.ofSeconds(1),
                        64 * 1024,
                        false,
                        Duration.ofSeconds(1),
                        Duration.ofSeconds(5),
                        false,
                        Duration.ofMillis(10),
                        Duration.ofSeconds(1),
                        2.0);
        assertEquals(
                ZLinkStreamErrorCode.VALIDATION_FAILED,
                assertThrows(
                                ZLinkStreamException.class,
                                () -> ZLinkStreamConnectorConfiguration.from(zeroAttempts))
                        .errorCode());
    }

    @Test
    void positiveHeartbeatAndReconnectValuesNeedNoOrdering() {
        ZLinkStreamConnectorOptions options =
                new ZLinkStreamConnectorOptions(
                        URI.create("tcp://127.0.0.1:7000"),
                        ZLinkStreamDispatchMode.MANUAL,
                        Duration.ofSeconds(1),
                        1,
                        Duration.ofSeconds(1),
                        64 * 1024,
                        true,
                        Duration.ofSeconds(5),
                        Duration.ofSeconds(1),
                        true,
                        Duration.ofMillis(1000),
                        Duration.ofMillis(100),
                        0.5);
        assertEquals(
                Duration.ofSeconds(1),
                ZLinkStreamConnectorConfiguration.from(options).heartbeat().timeout());
    }

    @Test
    void requestSequenceExhaustionDoesNotReuseAnEarlierValue() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    createConnector(server.options(ZLinkStreamDispatchMode.MANUAL));
            ConnectorTestAwait.await(connector.connect());
            Field sequenceField =
                    DefaultZLinkStreamConnector.class.getDeclaredField("nextRequestSeq");
            sequenceField.setAccessible(true);
            ((AtomicLong) sequenceField.get(connector)).set(-2L);

            var first = connector.request(payload("Echo", "first")).submit();
            assertEquals(-1L, server.readFrameAsync().join().header().requestSeq());
            CompletionException exhausted =
                    assertThrows(
                            CompletionException.class,
                            () ->
                                    connector
                                            .request(payload("Echo", "second"))
                                            .submit()
                                            .toCompletableFuture()
                                            .join());
            assertEquals(
                    ZLinkStreamErrorCode.SEND_FAILED,
                    ((ZLinkStreamException) exhausted.getCause()).errorCode());
            assertTrue(connector.isConnected());
            ConnectorTestAwait.await(connector.close());
            assertTrue(first.toCompletableFuture().isCompletedExceptionally());
        }
    }

    /**
     * Spec 32 5.2: the request timeout starts when the Request is accepted, after its payload is
     * compressed. The compression waits for a task that the connector's own timeout timer runs one
     * request timeout later; a timeout started before the compression would run on that timer
     * first. So the Request is still pending when submit returns only if its timer started at
     * acceptance, and it then ends with RequestTimeout since the server never replies.
     */
    @Test
    void requestTimeoutStartsAfterSlowCompressionAndAcceptance() throws Exception {
        Duration timeout = Duration.ofMillis(200);
        AtomicReference<java.util.concurrent.ScheduledExecutorService> timer =
                new AtomicReference<>();
        ZLinkStreamCompressionCodec codec =
                new ZLinkStreamCompressionCodec() {
                    @Override
                    public byte[] compress(byte[] payload) {
                        try {
                            timer.get()
                                    .schedule(() -> {}, timeout.toMillis(), TimeUnit.MILLISECONDS)
                                    .get();
                        } catch (Exception interrupted) {
                            throw new IllegalStateException(interrupted);
                        }
                        return payload;
                    }

                    @Override
                    public byte[] decompress(byte[] payload, int maxDecompressedSize) {
                        return payload;
                    }
                };
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            DefaultZLinkStreamConnector connector =
                    new DefaultZLinkStreamConnector(
                            options(
                                    server.endpoint(),
                                    ZLinkStreamDispatchMode.MANUAL,
                                    64 * 1024,
                                    64 * 1024,
                                    false,
                                    false,
                                    ZLinkStreamCompression.LZ4,
                                    codec));
            connectors.add(connector);
            timer.set(
                    (java.util.concurrent.ScheduledExecutorService)
                            fieldValue(connector, "timeouts"));
            ConnectorTestAwait.await(connector.connect());

            CompletableFuture<ZLinkStreamEncodedPayload> pending =
                    connector
                            .request(payload("SlowEncode", "request"))
                            .timeout(timeout)
                            .compress()
                            .submit()
                            .toCompletableFuture();

            assertFalse(pending.isDone(), "the timeout must not start before acceptance");
            assertEquals("SlowEncode", server.readFrameAsync().join().header().name());
            CompletionException timedOut = assertThrows(CompletionException.class, pending::join);
            assertEquals(
                    ZLinkStreamErrorCode.REQUEST_TIMEOUT,
                    ((ZLinkStreamException) timedOut.getCause()).errorCode());
        }
    }

    @Test
    void terminalResponseIsDiscardedBeforePayloadDecompression() throws Exception {
        AtomicInteger decompressions = new AtomicInteger();
        ZLinkStreamCompressionCodec codec =
                new ZLinkStreamCompressionCodec() {
                    @Override
                    public byte[] compress(byte[] payload) {
                        return payload;
                    }

                    @Override
                    public byte[] decompress(byte[] payload, int maxDecompressedSize) {
                        decompressions.incrementAndGet();
                        return payload;
                    }
                };
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    createConnector(
                            options(
                                    server.endpoint(),
                                    ZLinkStreamDispatchMode.IMMEDIATE,
                                    64 * 1024,
                                    64 * 1024,
                                    false,
                                    false,
                                    ZLinkStreamCompression.LZ4,
                                    codec));
            ConnectorTestAwait.await(connector.connect());
            CompletableFuture<ZLinkStreamEncodedPayload> pending =
                    connector
                            .request(payload("Echo", "request"))
                            .timeout(Duration.ofMillis(30))
                            .submit()
                            .toCompletableFuture();
            TcpStreamConnectorTestServer.ReceivedFrame request = server.readFrameAsync().join();
            assertThrows(CompletionException.class, pending::join);
            CountDownLatch afterLate = new CountDownLatch(1);
            connector.on(
                    "AfterLate",
                    message -> {
                        message.payload().payload().close();
                        afterLate.countDown();
                        return CompletableFuture.completedFuture(null);
                    });
            server.sendAsync(
                            new ZLinkStreamWireProtocol.Header(
                                    ZLinkStreamWireProtocol.KIND_RESPONSE,
                                    ZLinkStreamWireProtocol.CODEC_RAW,
                                    ZLinkStreamWireProtocol.FLAG_HAS_REQUEST_SEQ
                                            | ZLinkStreamWireProtocol.FLAG_PAYLOAD_COMPRESSED,
                                    request.header().requestSeq(),
                                    "Echo",
                                    Map.of(),
                                    null),
                            TcpStreamConnectorTestServer.bytes("late"))
                    .join();
            server.sendAsync(
                            new ZLinkStreamWireProtocol.Header(
                                    ZLinkStreamWireProtocol.KIND_SEND,
                                    ZLinkStreamWireProtocol.CODEC_RAW,
                                    0,
                                    null,
                                    "AfterLate",
                                    Map.of(),
                                    null),
                            TcpStreamConnectorTestServer.bytes("after"))
                    .join();
            assertTrue(afterLate.await(5, TimeUnit.SECONDS));
            assertEquals(0, decompressions.get());
            assertTrue(connector.isConnected());
            ConnectorTestAwait.await(connector.close());
        }
    }

    @Test
    void receivePayloadLimitMustBePositive() {
        assertThrows(
                ZLinkStreamException.class,
                () ->
                        createConnector(
                                options(
                                        URI.create("tcp://127.0.0.1:1"),
                                        ZLinkStreamDispatchMode.MANUAL,
                                        64 * 1024,
                                        0,
                                        false,
                                        false,
                                        ZLinkStreamCompression.NONE)));
    }

    private static ZLinkStreamEncodedPayload payload(String packetName, String body) {
        return new ZLinkStreamEncodedPayload(packetName, Message.from(body), Map.of());
    }

    private static ZLinkStreamConnectorOptions options(ZLinkStreamDispatchMode dispatchMode) {
        return new ZLinkStreamConnectorOptions(
                URI.create("tcp://127.0.0.1:1"), dispatchMode, Duration.ofSeconds(1), 1);
    }

    private static ZLinkStreamConnectorOptions compressedOptions(
            URI endpoint, ZLinkStreamDispatchMode dispatchMode) {
        return compressedOptions(endpoint, dispatchMode, 64 * 1024);
    }

    private static ZLinkStreamConnectorOptions compressedOptions(
            URI endpoint, ZLinkStreamDispatchMode dispatchMode, int maxSendPayloadSize) {
        return options(
                endpoint,
                dispatchMode,
                maxSendPayloadSize,
                64 * 1024,
                true,
                false,
                ZLinkStreamCompression.LZ4);
    }

    private static ZLinkStreamConnectorOptions options(
            URI endpoint,
            ZLinkStreamDispatchMode dispatchMode,
            int maxSendPayloadSize,
            int maxReceivePayloadSize,
            boolean reconnectEnabled,
            boolean skipServerCertificateValidation,
            ZLinkStreamCompression compression) {
        return options(
                endpoint,
                dispatchMode,
                maxSendPayloadSize,
                maxReceivePayloadSize,
                reconnectEnabled,
                skipServerCertificateValidation,
                compression,
                null);
    }

    private static ZLinkStreamConnectorOptions options(
            URI endpoint,
            ZLinkStreamDispatchMode dispatchMode,
            int maxSendPayloadSize,
            int maxReceivePayloadSize,
            boolean reconnectEnabled,
            boolean skipServerCertificateValidation,
            ZLinkStreamCompression compression,
            ZLinkStreamCompressionCodec compressionCodec) {
        return new ZLinkStreamConnectorOptions(
                endpoint,
                dispatchMode,
                Duration.ofSeconds(1),
                Duration.ofSeconds(5),
                1,
                Duration.ofSeconds(1),
                maxSendPayloadSize,
                maxReceivePayloadSize,
                false,
                Duration.ofMillis(25),
                Duration.ofMillis(500),
                reconnectEnabled,
                Duration.ofMillis(250),
                Duration.ofSeconds(5),
                2.0,
                skipServerCertificateValidation,
                compression,
                compressionCodec,
                ZLinkStreamPacketNameResolver.defaultResolver(),
                null);
    }

    private record PrefixCompressionCodec(String prefix) implements ZLinkStreamCompressionCodec {
        @Override
        public byte[] compress(byte[] payload) {
            return (prefix + ":" + new String(payload, StandardCharsets.UTF_8))
                    .getBytes(StandardCharsets.UTF_8);
        }

        @Override
        public byte[] decompress(byte[] payload, int maxDecompressedSize) {
            String value = new String(payload, StandardCharsets.UTF_8);
            String marker = prefix + ":";
            if (!value.startsWith(marker)) {
                throw new IllegalArgumentException("unexpected compression marker");
            }
            return value.substring(marker.length()).getBytes(StandardCharsets.UTF_8);
        }
    }

    private static byte[] framePrefix(int headerLength, int payloadLength) {
        return ByteBuffer.allocate(6).putShort((short) headerLength).putInt(payloadLength).array();
    }

    private static byte[] hex(String value) {
        byte[] result = new byte[value.length() / 2];
        for (int i = 0; i < result.length; i++) {
            result[i] = (byte) Integer.parseInt(value.substring(i * 2, i * 2 + 2), 16);
        }
        return result;
    }

    @ZLinkStreamPacketName("custom.packet")
    private record NamedPayload(String value) {}

    @ZLinkStreamPacketName("")
    private record BlankNamedPayload(String value) {}
}
