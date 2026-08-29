package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.ReceiveFlowState;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRecvMode;

final class ZLinkJavaSocketReceiveOwnerTest {
    private static final Duration OPERATION_TIMEOUT = Duration.ofSeconds(2);

    @Test
    void routerTopologyEntryDoesNotWaitForTheReceiveOwner() throws Exception {
        String endpoint = "inproc://receive-owner-topology-" + System.nanoTime();
        String additionalEndpoint = endpoint + "-additional";
        try (var context = Zlink.createContext();
             RouterSocket nativeRouter = context.createRouterSocket();
             DealerSocket nativeDealer = context.createDealerSocket();
             ExecutorService executor = Executors.newFixedThreadPool(2)) {
            nativeRouter.options().recvTimeout(Duration.ofSeconds(5));
            ZLinkJavaRouterSocket router = new ZLinkJavaRouterSocket(nativeRouter);
            router.bind(endpoint);
            nativeDealer.connect(endpoint);
            AtomicReference<Thread> receiveOwner = new AtomicReference<>();
            Future<?> receive = executor.submit(() -> {
                receiveOwner.set(Thread.currentThread());
                assertNotNull(router.recv(ZLinkBackendRecvMode.BLOCK));
            });
            awaitReceiveEntry(receiveOwner, ZLinkJavaRouterSocket.class);
            Future<?> topology = executor.submit(() ->
                router.connect(additionalEndpoint));

            try {
                assertDoesNotThrow(() -> topology.get(1, TimeUnit.SECONDS));
            } finally {
                send(nativeDealer, "release-topology");
                receive.get(OPERATION_TIMEOUT.toMillis(), TimeUnit.MILLISECONDS);
                router.close();
            }
        }
    }

    @Test
    void dealerReceiveFlowEntryDoesNotWaitForTheReceiveOwner() throws Exception {
        RoutingId dealerRid = RoutingId.from("receive-owner-dealer");
        String endpoint = "inproc://receive-owner-dealer-" + System.nanoTime();
        try (var context = Zlink.createContext();
             DealerSocket nativeDealer = context.createDealerSocket();
             RouterSocket nativeRouter = context.createRouterSocket();
             ExecutorService executor = Executors.newFixedThreadPool(2)) {
            nativeDealer.options().recvTimeout(Duration.ofSeconds(5));
            nativeDealer.setRoutingId(dealerRid);
            nativeRouter.bind(endpoint);
            nativeDealer.connect(endpoint);
            RoutingId admittedRid = admitDealer(nativeDealer, nativeRouter);
            ZLinkJavaDealerSocket dealer = new ZLinkJavaDealerSocket(nativeDealer);
            AtomicReference<Thread> receiveOwner = new AtomicReference<>();
            Future<?> receive = executor.submit(() -> {
                receiveOwner.set(Thread.currentThread());
                assertNotNull(dealer.recv(ZLinkBackendRecvMode.BLOCK));
            });
            awaitReceiveEntry(receiveOwner, ZLinkJavaDealerSocket.class);
            Future<?> flow = executor.submit(() ->
                dealer.setReceiveFlowState(ReceiveFlowState.RUNNING));

            try {
                assertDoesNotThrow(() -> flow.get(1, TimeUnit.SECONDS));
            } finally {
                send(nativeRouter, admittedRid, "release-dealer");
                receive.get(OPERATION_TIMEOUT.toMillis(), TimeUnit.MILLISECONDS);
                dealer.close();
            }
        }
    }

    @Test
    void routerReceiveFlowEntryDoesNotWaitForTheReceiveOwner() throws Exception {
        String endpoint = "inproc://receive-owner-router-" + System.nanoTime();
        try (var context = Zlink.createContext();
             RouterSocket nativeRouter = context.createRouterSocket();
             DealerSocket nativeDealer = context.createDealerSocket();
             ExecutorService executor = Executors.newFixedThreadPool(2)) {
            nativeRouter.options().recvTimeout(Duration.ofSeconds(5));
            nativeRouter.bind(endpoint);
            nativeDealer.connect(endpoint);
            ZLinkJavaRouterSocket router = new ZLinkJavaRouterSocket(nativeRouter);
            AtomicReference<Thread> receiveOwner = new AtomicReference<>();
            Future<?> receive = executor.submit(() -> {
                receiveOwner.set(Thread.currentThread());
                assertNotNull(router.recv(ZLinkBackendRecvMode.BLOCK));
            });
            awaitReceiveEntry(receiveOwner, ZLinkJavaRouterSocket.class);
            Future<?> flow = executor.submit(() ->
                router.setReceiveFlowState(ReceiveFlowState.RUNNING));

            try {
                assertDoesNotThrow(() -> flow.get(1, TimeUnit.SECONDS));
            } finally {
                send(nativeDealer, "release-router");
                receive.get(OPERATION_TIMEOUT.toMillis(), TimeUnit.MILLISECONDS);
                router.close();
            }
        }
    }

    private static void awaitReceiveEntry(
        AtomicReference<Thread> owner,
        Class<?> wrapperType) throws Exception {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(1);
        while (true) {
            Thread thread = owner.get();
            if (thread != null) {
                for (StackTraceElement frame : thread.getStackTrace()) {
                    if (frame.getClassName().equals(wrapperType.getName())
                        && frame.getMethodName().equals("recv")) {
                        return;
                    }
                }
            }
            if (System.nanoTime() >= deadline) {
                throw new AssertionError(
                    "receive owner did not enter " + wrapperType.getSimpleName());
            }
            Thread.sleep(1);
        }
    }

    private static RoutingId admitDealer(
        DealerSocket dealer,
        RouterSocket router) throws Exception {
        send(dealer, "admit");
        try (Received received = new Received()) {
            long deadline = System.nanoTime() + OPERATION_TIMEOUT.toNanos();
            while (!router.recv(received, RecvFlags.DONT_WAIT)) {
                if (System.nanoTime() >= deadline) {
                    throw new AssertionError("dealer admission timed out");
                }
                Thread.sleep(1);
            }
            return received.getRoutingId().orElseThrow();
        }
    }

    private static void send(DealerSocket socket, String value) throws Exception {
        try (Message message = Message.from(value)) {
            socket.send().message(message).submit()
                .toCompletableFuture()
                .get(OPERATION_TIMEOUT.toMillis(), TimeUnit.MILLISECONDS);
        }
    }

    private static void send(
        RouterSocket socket,
        RoutingId target,
        String value) throws Exception {
        try (Message message = Message.from(value)) {
            socket.send(target).message(message).submit()
                .toCompletableFuture()
                .get(OPERATION_TIMEOUT.toMillis(), TimeUnit.MILLISECONDS);
        }
    }
}
