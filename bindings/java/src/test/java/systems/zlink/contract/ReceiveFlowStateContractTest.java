/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contract;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assertions.fail;

import java.lang.reflect.Method;
import java.time.Duration;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ConfigResult;
import systems.zlink.contracts.errors.ZlinkConfigException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.PairSocket;
import systems.zlink.contracts.sockets.PubSocket;
import systems.zlink.contracts.sockets.ReceiveFlowState;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.Socket;
import systems.zlink.contracts.sockets.StreamSocket;
import systems.zlink.contracts.sockets.SubSocket;
import systems.zlink.contracts.sockets.XPubSocket;
import systems.zlink.contracts.sockets.XSubSocket;

/**
 * Plan §5.1/§7.3/§8.1.1 java-binding parity coverage for
 * {@code zlink_socket_set_receive_flow_state}.
 */
final class ReceiveFlowStateContractTest {

    @Test
    void enumValuesMatchCAbi() {
        assertEquals(0, ReceiveFlowState.RUNNING.value());
        assertEquals(1, ReceiveFlowState.PAUSED.value());
        assertEquals(ReceiveFlowState.RUNNING, ReceiveFlowState.fromValue(0));
        assertEquals(ReceiveFlowState.PAUSED, ReceiveFlowState.fromValue(1));
    }

    @Test
    void dealerAndRouterAcceptTheSettingAndRepeatIsIdempotent() {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext();
             DealerSocket dealer = context.createDealerSocket();
             RouterSocket router = context.createRouterSocket()) {
            dealer.options().receiveFlowState(ReceiveFlowState.PAUSED);
            dealer.options().receiveFlowState(ReceiveFlowState.PAUSED);
            dealer.options().receiveFlowState(ReceiveFlowState.RUNNING);
            dealer.options().receiveFlowState(ReceiveFlowState.RUNNING);

            router.options().receiveFlowState(ReceiveFlowState.PAUSED);
            router.options().receiveFlowState(ReceiveFlowState.PAUSED);
            router.options().receiveFlowState(ReceiveFlowState.RUNNING);
            router.options().receiveFlowState(ReceiveFlowState.RUNNING);
        }
    }

    @Test
    void unsupportedSocketTypesReportNotSupported() {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext();
             PairSocket pair = context.createPairSocket();
             PubSocket pub = context.createPubSocket();
             SubSocket sub = context.createSubSocket();
             XPubSocket xpub = context.createXPubSocket();
             XSubSocket xsub = context.createXSubSocket();
             StreamSocket stream = context.createStreamSocket()) {
            List<Socket> unsupported = List.of(pair, pub, sub, xpub, xsub, stream);
            for (Socket socket : unsupported) {
                ZlinkConfigException ex = assertThrows(ZlinkConfigException.class,
                    () -> socket.options().receiveFlowState(ReceiveFlowState.PAUSED));
                assertEquals(ConfigResult.NOT_SUPPORTED, ex.getResult());
            }
        }
    }

    @Test
    void unsupportedSocketSendRecvIsUnaffected() {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext();
             PairSocket left = context.createPairSocket();
             PairSocket right = context.createPairSocket()) {
            String endpoint = TestSupport.inprocEndpoint("java-flow-pair-smoke");
            left.bind(endpoint);
            right.connect(endpoint);

            assertThrows(ZlinkConfigException.class,
                () -> left.options().receiveFlowState(ReceiveFlowState.PAUSED));

            try (Message outbound = Message.from("still-works")) {
                left.send().message(outbound).submit()
                    .toCompletableFuture().join();
            }
            Received received = new Received();
            TestSupport.awaitCondition(() -> {
                try {
                    return right.recv(received, RecvFlags.DONT_WAIT);
                } catch (RuntimeException ex) {
                    return false;
                }
            });
            assertEquals("still-works",
                received.singlePartOrThrow().toUtf8String());
            received.close();
        }
    }

    @Test
    void invalidArgumentOutsideEnumRangeIsRejected() {
        // ReceiveFlowState only has two variants (RUNNING=0, PAUSED=1), so
        // the java type system is this binding's mapping of the C layer's
        // ZLINK_CONFIG_INVALID_ARGUMENT: there is no public-API path that
        // can pass a third state to receiveFlowState(...) at all. This
        // mirrors the rust binding's documented approach for the same case
        // (worklog/stage8-bindings.md "rust" section).
        assertThrows(IllegalArgumentException.class,
            () -> ReceiveFlowState.fromValue(2));
        assertThrows(IllegalArgumentException.class,
            () -> ReceiveFlowState.fromValue(-1));
        assertThrows(IllegalArgumentException.class,
            () -> ReceiveFlowState.fromValue(999));
    }

    @Test
    void closedHandleReportsInvalidHandleOrLocalClosedState() {
        TestSupport.assumeNative();

        Context context = Zlink.createContext();
        DealerSocket dealer = context.createDealerSocket();
        dealer.close();

        try {
            dealer.options().receiveFlowState(ReceiveFlowState.PAUSED);
            fail("expected an exception for a closed socket");
        } catch (ZlinkConfigException ex) {
            assertEquals(ConfigResult.INVALID_HANDLE, ex.getResult());
        } catch (IllegalStateException expectedLocalClosedState) {
            // The java binding may reject an already-closed local handle
            // before reaching native code; both outcomes are the two
            // observable "close wins" shapes documented in
            // worklog/stage7-c-api.md §1.2.
        } finally {
            context.close();
        }
    }

    @Test
    void closeRaceObservesOnlyOneOutcome() throws Exception {
        TestSupport.assumeNative();

        for (int i = 0; i < 20; i++) {
            try (Context context = Zlink.createContext()) {
                DealerSocket dealer = context.createDealerSocket();
                CountDownLatch ready = new CountDownLatch(1);
                AtomicReference<Throwable> observed = new AtomicReference<>();
                Thread setter = new Thread(() -> {
                    ready.countDown();
                    try {
                        dealer.options().receiveFlowState(ReceiveFlowState.PAUSED);
                    } catch (Throwable t) {
                        observed.set(t);
                    }
                });
                setter.start();
                ready.await();
                dealer.close();
                setter.join(TimeUnit.SECONDS.toMillis(5));
                assertTrue(!setter.isAlive(), "setter thread did not finish");

                Throwable outcome = observed.get();
                if (outcome == null) {
                    continue; // the setter's call was admitted first: success
                }
                assertTrue(
                    outcome instanceof IllegalStateException
                        || (outcome instanceof ZlinkConfigException zce
                            && (zce.getResult() == ConfigResult.INVALID_STATE
                                || zce.getResult() == ConfigResult.INVALID_HANDLE)),
                    "unexpected close-race outcome: " + outcome);
            }
        }
    }

    @Test
    void publicSurfaceHasNoFlowFrameOrPauseBypassApi() {
        for (Method method : systems.zlink.contracts.sockets.Socket.class.getMethods()) {
            String name = method.getName().toLowerCase(java.util.Locale.ROOT);
            assertTrue(!name.contains("flowframe") && !name.contains("flow_frame"),
                "unexpected flow-frame method: " + method);
            assertTrue(!name.contains("bypasspause") && !name.contains("pausebypass"),
                "unexpected pause-bypass method: " + method);
        }
        for (Method method : systems.zlink.contracts.sockets.CommonSocketOptions
            .class.getMethods()) {
            String name = method.getName().toLowerCase(java.util.Locale.ROOT);
            assertTrue(!name.contains("flowframe") && !name.contains("flow_frame"),
                "unexpected flow-frame method: " + method);
        }
    }

    @Test
    void existingHwmAndSendTimeoutBehaviorIsUnchanged() {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext();
             DealerSocket dealer = context.createDealerSocket();
             RouterSocket router = context.createRouterSocket()) {
            context.options().autoHwmEnabled(false);
            dealer.options().sendHwm(1024L);
            dealer.options().sendTimeout(Duration.ofMillis(50));
            dealer.options().receiveFlowState(ReceiveFlowState.RUNNING);

            assertEquals(1024L, dealer.options().sendHwm());
            assertEquals(Duration.ofMillis(50), dealer.options().sendTimeout());
        }
    }

}
