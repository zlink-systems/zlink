package systems.zlink.stream.connector;

import io.netty.buffer.ByteBuf;
import io.netty.buffer.ByteBufAllocator;
import io.netty.channel.embedded.EmbeddedChannel;
import io.netty.handler.ssl.SslContext;
import io.netty.handler.ssl.SslContextBuilder;
import io.netty.handler.ssl.SslHandler;
import io.netty.handler.ssl.util.InsecureTrustManagerFactory;

import java.io.File;
import java.net.URISyntaxException;
import java.net.URL;

final class TlsTestCertificates {
    private TlsTestCertificates() {}

    static File certificate() {
        return resourceFile("tls-test-cert.pem");
    }

    static File privateKey() {
        return resourceFile("tls-test-key.pem");
    }

    /**
     * The server context of the TLS test servers, returned once it has completed a handshake.
     *
     * <p>The first handshake in a JVM also loads and compiles the TLS stack, which takes most of a
     * second of CPU and longer on a loaded machine. A connector's connect timeout bounds a connect,
     * so that one-time cost is paid here: the server is ready when the handshake has finished, not
     * after a time budget, and the connects the tests make afterwards do only the work of a
     * connect.
     */
    static SslContext serverContext() throws Exception {
        SslContext server = SslContextBuilder.forServer(certificate(), privateKey()).build();
        SslContext client =
                SslContextBuilder.forClient()
                        .trustManager(InsecureTrustManagerFactory.INSTANCE)
                        .build();
        completeHandshake(
                server.newHandler(ByteBufAllocator.DEFAULT),
                client.newHandler(ByteBufAllocator.DEFAULT));
        return server;
    }

    /**
     * Runs a handshake in memory, passing the bytes each side writes to the other until it ends.
     */
    private static void completeHandshake(SslHandler serverHandler, SslHandler clientHandler)
            throws Exception {
        serverHandler.setHandshakeTimeoutMillis(0);
        clientHandler.setHandshakeTimeoutMillis(0);
        EmbeddedChannel serverSide = new EmbeddedChannel(serverHandler);
        EmbeddedChannel clientSide = new EmbeddedChannel(clientHandler);
        try {
            while (!serverHandler.handshakeFuture().isDone()
                    || !clientHandler.handshakeFuture().isDone()) {
                boolean moved = pass(clientSide, serverSide) | pass(serverSide, clientSide);
                if (!moved) {
                    throw new IllegalStateException("TLS handshake stopped without finishing");
                }
            }
            clientHandler.handshakeFuture().sync();
            serverHandler.handshakeFuture().sync();
        } finally {
            clientSide.finishAndReleaseAll();
            serverSide.finishAndReleaseAll();
        }
    }

    private static boolean pass(EmbeddedChannel from, EmbeddedChannel to) {
        from.runPendingTasks();
        boolean moved = false;
        for (ByteBuf bytes = from.readOutbound(); bytes != null; bytes = from.readOutbound()) {
            to.writeInbound(bytes);
            moved = true;
        }
        return moved;
    }

    private static File resourceFile(String name) {
        URL resource = TlsTestCertificates.class.getClassLoader().getResource(name);
        if (resource == null) {
            throw new IllegalStateException("missing test TLS resource: " + name);
        }
        try {
            return new File(resource.toURI());
        } catch (URISyntaxException ex) {
            throw new IllegalStateException("invalid test TLS resource URI: " + name, ex);
        }
    }
}
