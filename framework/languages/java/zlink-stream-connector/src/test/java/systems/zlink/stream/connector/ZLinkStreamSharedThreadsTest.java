package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.*;

import io.netty.bootstrap.ServerBootstrap;
import io.netty.buffer.Unpooled;
import io.netty.channel.Channel;
import io.netty.channel.ChannelHandlerContext;
import io.netty.channel.ChannelInitializer;
import io.netty.channel.SimpleChannelInboundHandler;
import io.netty.channel.group.ChannelGroup;
import io.netty.channel.group.DefaultChannelGroup;
import io.netty.channel.nio.NioEventLoopGroup;
import io.netty.channel.socket.SocketChannel;
import io.netty.channel.socket.nio.NioServerSocketChannel;
import io.netty.handler.codec.http.HttpObjectAggregator;
import io.netty.handler.codec.http.HttpServerCodec;
import io.netty.handler.codec.http.websocketx.BinaryWebSocketFrame;
import io.netty.handler.codec.http.websocketx.WebSocketServerProtocolHandler;
import io.netty.handler.ssl.SslContext;
import io.netty.util.concurrent.ImmediateEventExecutor;

import org.junit.jupiter.api.Test;

import java.lang.management.ManagementFactory;
import java.lang.management.ThreadInfo;
import java.net.InetSocketAddress;
import java.net.URI;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

class ZLinkStreamSharedThreadsTest {
    @Test
    void twoHundredConnectionsShareThreads() throws Exception {
        try (MultiConnectionServer server = new MultiConnectionServer(false)) {
            List<ZLinkStreamConnector> connectors = new ArrayList<>();
            try {
                ThreadSnapshot before = ThreadSnapshot.capture();
                connect(connectors, server, 10);
                ThreadSnapshot ten = ThreadSnapshot.capture();
                connect(connectors, server, 190);
                ThreadSnapshot twoHundred = ThreadSnapshot.capture();
                System.out.printf(
                        "connections=0 %s%nconnections=10 %s%nconnections=200 %s%n",
                        before, ten, twoHundred);
                assertTrue(
                        twoHundred.total() - ten.total() <= 16,
                        "190 more connections must share execution resources: "
                                + ten
                                + " -> "
                                + twoHundred);
                assertTrue(twoHundred.selectors() - before.selectors() <= 1);
                assertTrue(twoHundred.timers() - before.timers() <= 1);
                assertTrue(connectors.stream().allMatch(ZLinkStreamConnector::isConnected));
            } finally {
                close(connectors);
            }
        }
    }

    @Test
    void serverRestartsReuseThreadsAndClosingOneConnectorKeepsOthersAlive() throws Exception {
        try (MultiConnectionServer server = new MultiConnectionServer(false)) {
            List<ZLinkStreamConnector> connectors = new ArrayList<>();
            AtomicReference<CountDownLatch> connected =
                    new AtomicReference<>(new CountDownLatch(12));
            try {
                for (int index = 0; index < 12; index++) {
                    ZLinkStreamConnector connector =
                            ZLinkStreamConnectorFactory.create(options(server.endpoint(), false));
                    connectors.add(connector);
                    connector.onConnectionStateChanged(
                            state -> {
                                if (state == ZLinkStreamConnectionState.CONNECTED)
                                    connected.get().countDown();
                                return CompletableFuture.completedFuture(null);
                            });
                    ConnectorTestAwait.await(connector.connect());
                }
                assertTrue(connected.get().await(5, TimeUnit.SECONDS));
                ThreadSnapshot initial = ThreadSnapshot.capture();
                for (int restart = 1; restart <= 4; restart++) {
                    connected.set(new CountDownLatch(connectors.size()));
                    server.restart();
                    assertTrue(connected.get().await(5, TimeUnit.SECONDS), "restart " + restart);
                    assertTrue(connectors.stream().allMatch(ZLinkStreamConnector::isConnected));
                    ThreadSnapshot current = ThreadSnapshot.capture();
                    System.out.printf(
                            "restart=%d initial=%s current=%s%n", restart, initial, current);
                    assertEquals(
                            initial.selectors(),
                            current.selectors(),
                            "reconnect must reuse HttpClient");
                    assertEquals(
                            initial.timers(), current.timers(), "reconnect must reuse scheduler");
                    assertTrue(current.total() - initial.total() <= 16);
                }
                ConnectorTestAwait.await(connectors.removeFirst().close());
                connected.set(new CountDownLatch(connectors.size()));
                server.restart();
                assertTrue(connected.get().await(5, TimeUnit.SECONDS));
                assertTrue(connectors.stream().allMatch(ZLinkStreamConnector::isConnected));
            } finally {
                close(connectors);
            }
        }
    }

    @Test
    void certificateValidationPoliciesRemainIsolated() throws Exception {
        try (MultiConnectionServer server = new MultiConnectionServer(true)) {
            ZLinkStreamConnector insecure =
                    ZLinkStreamConnectorFactory.create(options(server.endpoint(), true));
            ZLinkStreamConnector verified =
                    ZLinkStreamConnectorFactory.create(options(server.endpoint(), false));
            try {
                ConnectorTestAwait.await(insecure.connect());
                ZLinkStreamException failure =
                        assertThrows(
                                ZLinkStreamException.class,
                                () -> ConnectorTestAwait.await(verified.connect()));
                assertEquals(ZLinkStreamErrorCode.TLS_VALIDATION_FAILED, failure.errorCode());
                assertTrue(insecure.isConnected());
                ConnectorTestAwait.await(verified.close());
                // Closing the verified connector must not close either shared client.
                ZLinkStreamConnector another =
                        ZLinkStreamConnectorFactory.create(options(server.endpoint(), true));
                try {
                    ConnectorTestAwait.await(another.connect());
                    assertTrue(another.isConnected());
                    assertTrue(insecure.isConnected());
                } finally {
                    ConnectorTestAwait.await(another.close());
                }
            } finally {
                ConnectorTestAwait.await(insecure.close());
                ConnectorTestAwait.await(verified.close());
            }
        }
    }

    private static void connect(
            List<ZLinkStreamConnector> connectors, MultiConnectionServer server, int count)
            throws Exception {
        for (int index = 0; index < count; index++) {
            ZLinkStreamConnector connector =
                    ZLinkStreamConnectorFactory.create(options(server.endpoint(), false));
            connectors.add(connector);
            ConnectorTestAwait.await(connector.connect());
        }
    }

    private static void close(List<ZLinkStreamConnector> connectors) throws Exception {
        for (ZLinkStreamConnector connector : connectors)
            ConnectorTestAwait.await(connector.close());
    }

    private static ZLinkStreamConnectorOptions options(URI endpoint, boolean skipValidation) {
        return new ZLinkStreamConnectorOptions(
                endpoint,
                ZLinkStreamDispatchMode.IMMEDIATE,
                Duration.ofSeconds(1),
                3,
                Duration.ofSeconds(5),
                64 * 1024,
                true,
                Duration.ofSeconds(1),
                Duration.ofSeconds(5),
                true,
                Duration.ofMillis(25),
                Duration.ofMillis(250),
                2.0,
                skipValidation);
    }

    private record ThreadSnapshot(
            int total, int selectors, int timers, Map<String, Integer> names) {
        static ThreadSnapshot capture() {
            Map<String, Integer> names = new TreeMap<>();
            int total = 0;
            int selectors = 0;
            int timers = 0;
            for (ThreadInfo thread :
                    ManagementFactory.getThreadMXBean().dumpAllThreads(false, false)) {
                total++;
                String name = thread.getThreadName();
                if (name.startsWith("HttpClient-") && name.endsWith("-SelectorManager"))
                    selectors++;
                if (name.equals("zlink-stream-connector-timeouts")) timers++;
                names.merge(name.replaceAll("[0-9]+", "#"), 1, Integer::sum);
            }
            return new ThreadSnapshot(total, selectors, timers, names);
        }
    }

    // Two server threads serve all clients, keeping server threads out of the connection slope.
    private static final class MultiConnectionServer implements AutoCloseable {
        private final NioEventLoopGroup boss = new NioEventLoopGroup(1);
        private final NioEventLoopGroup worker = new NioEventLoopGroup(1);
        private final ChannelGroup clients =
                new DefaultChannelGroup(ImmediateEventExecutor.INSTANCE);
        private final SslContext ssl;
        private Channel listening;

        MultiConnectionServer(boolean secure) throws Exception {
            ssl = secure ? TlsTestCertificates.serverContext() : null;
            bind(0);
        }

        URI endpoint() {
            return URI.create(
                    (ssl == null ? "ws" : "wss")
                            + "://127.0.0.1:"
                            + ((InetSocketAddress) listening.localAddress()).getPort()
                            + "/stream");
        }

        void restart() throws InterruptedException {
            int port = ((InetSocketAddress) listening.localAddress()).getPort();
            listening.close().sync();
            clients.close().await();
            bind(port);
        }

        private void bind(int port) throws InterruptedException {
            listening =
                    new ServerBootstrap()
                            .group(boss, worker)
                            .channel(NioServerSocketChannel.class)
                            .childHandler(
                                    new ChannelInitializer<SocketChannel>() {
                                        @Override
                                        protected void initChannel(SocketChannel channel) {
                                            clients.add(channel);
                                            if (ssl != null)
                                                channel.pipeline()
                                                        .addLast(ssl.newHandler(channel.alloc()));
                                            channel.pipeline()
                                                    .addLast(
                                                            new HttpServerCodec(),
                                                            new HttpObjectAggregator(8192),
                                                            new WebSocketServerProtocolHandler(
                                                                    "/stream", null, true),
                                                            new SimpleChannelInboundHandler<
                                                                    BinaryWebSocketFrame>() {
                                                                @Override
                                                                protected void channelRead0(
                                                                        ChannelHandlerContext
                                                                                context,
                                                                        BinaryWebSocketFrame
                                                                                message) {
                                                                    byte[] frame =
                                                                            new byte
                                                                                    [message.content()
                                                                                            .readableBytes()];
                                                                    message.content()
                                                                            .readBytes(frame);
                                                                    ZLinkStreamWireProtocol.Frame
                                                                            decoded =
                                                                                    ZLinkStreamWireProtocol
                                                                                            .decodeFrame(
                                                                                                    frame);
                                                                    ZLinkStreamWireProtocol.Header
                                                                            header =
                                                                                    ZLinkStreamWireProtocol
                                                                                            .decodeHeader(
                                                                                                    decoded
                                                                                                            .header());
                                                                    if (header.name()
                                                                            .equals(
                                                                                    ZLinkStreamReceiveDispatcher
                                                                                            .HEARTBEAT_PING_NAME)) {
                                                                        byte[] pongHeader =
                                                                                ZLinkStreamWireProtocol
                                                                                        .encodeHeader(
                                                                                                new ZLinkStreamWireProtocol
                                                                                                        .Header(
                                                                                                        ZLinkStreamWireProtocol
                                                                                                                .KIND_CONTROL,
                                                                                                        ZLinkStreamWireProtocol
                                                                                                                .CODEC_RAW,
                                                                                                        0,
                                                                                                        null,
                                                                                                        ZLinkStreamReceiveDispatcher
                                                                                                                .HEARTBEAT_PONG_NAME,
                                                                                                        Map
                                                                                                                .of(),
                                                                                                        null));
                                                                        context.writeAndFlush(
                                                                                new BinaryWebSocketFrame(
                                                                                        Unpooled
                                                                                                .wrappedBuffer(
                                                                                                        ZLinkStreamWireProtocol
                                                                                                                .encodeFrame(
                                                                                                                        pongHeader,
                                                                                                                        new byte
                                                                                                                                [0],
                                                                                                                        64
                                                                                                                                * 1024))));
                                                                    }
                                                                }
                                                            });
                                        }
                                    })
                            .bind("127.0.0.1", port)
                            .sync()
                            .channel();
        }

        @Override
        public void close() throws InterruptedException {
            listening.close().sync();
            clients.close().await();
            boss.shutdownGracefully(0, 5, TimeUnit.SECONDS).sync();
            worker.shutdownGracefully(0, 5, TimeUnit.SECONDS).sync();
        }
    }
}
