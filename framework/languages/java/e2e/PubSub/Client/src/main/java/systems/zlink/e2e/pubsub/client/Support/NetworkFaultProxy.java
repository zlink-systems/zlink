package systems.zlink.e2e.pubsub.client.Support;

import java.io.IOException;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

public final class NetworkFaultProxy implements AutoCloseable {
    private final String upstreamHost;
    private final int port;
    private final AtomicBoolean blocked = new AtomicBoolean();
    private final AtomicBoolean running = new AtomicBoolean(true);
    private final Set<Socket> sockets = ConcurrentHashMap.newKeySet();
    private final ExecutorService executor = Executors.newVirtualThreadPerTaskExecutor();
    private final ServerSocket listener;

    private NetworkFaultProxy(String upstreamHost, int port) throws IOException {
        this.upstreamHost = upstreamHost;
        this.port = port;
        listener = new ServerSocket();
        listener.setReuseAddress(true);
        listener.bind(new InetSocketAddress(InetAddress.getByName("127.0.0.1"), port));
        executor.submit(this::acceptLoop);
    }

    public static NetworkFaultProxy start(String upstreamHost, int port) {
        try {
            return new NetworkFaultProxy(upstreamHost, port);
        } catch (IOException error) {
            throw new IllegalStateException("failed to start network fault proxy", error);
        }
    }

    public void block() {
        blocked.set(true);
        for (Socket socket : sockets.toArray(Socket[]::new)) {
            closeSocket(socket);
        }
    }

    public void unblock() {
        blocked.set(false);
    }

    private void acceptLoop() {
        while (running.get()) {
            try {
                accept(listener.accept());
            } catch (IOException error) {
                if (running.get()) {
                    throw new IllegalStateException("network fault proxy accept failed", error);
                }
            }
        }
    }

    private void accept(Socket client) throws IOException {
        if (blocked.get()) {
            client.close();
            return;
        }
        Socket upstream = new Socket(upstreamHost, port);
        sockets.add(client);
        sockets.add(upstream);
        executor.submit(() -> copy(client, upstream));
        executor.submit(() -> copy(upstream, client));
    }

    private void copy(Socket source, Socket target) {
        try {
            source.getInputStream().transferTo(target.getOutputStream());
        } catch (IOException ignored) {
        } finally {
            closeSocket(source);
            closeSocket(target);
        }
    }

    private void closeSocket(Socket socket) {
        sockets.remove(socket);
        try {
            socket.close();
        } catch (IOException ignored) {
        }
    }

    @Override
    public void close() {
        running.set(false);
        block();
        try {
            listener.close();
        } catch (IOException ignored) {
        }
        executor.close();
    }
}
