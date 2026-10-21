package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ConnectResult;
import systems.zlink.contracts.errors.ZlinkConnectException;
import systems.zlink.contracts.sockets.RouterSocket;

import java.lang.reflect.Proxy;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicInteger;

/** Checks the binding result projection used when liveness ends a physical connection. */
public final class ZLinkJavaLivenessDisconnectResultTest {
    private static final RoutingId PEER = RoutingId.from("already-absent-liveness-peer");

    @Test
    void nativeAbsentRidRemainsNotFoundWhileLivenessClosureIsSatisfied() {
        try (var context = Zlink.createContext();
                var socket = context.createRouterSocket()) {
            socket.bind("inproc://liveness-disconnect-result-" + UUID.randomUUID());
            var missing =
                    assertThrows(ZlinkConnectException.class, () -> socket.disconnectRid(PEER));
            assertEquals(ConnectResult.NOT_FOUND, missing.getResult());
            assertDoesNotThrow(() -> ZLinkJavaSocketSupport.disconnectRidIfPresent(socket, PEER));
        }
    }

    @ParameterizedTest
    @ValueSource(ints = {0, 2, 107})
    void onlyTypedAbsenceIsConsumedRegardlessOfErrno(int errno) {
        var calls = new AtomicInteger();
        var socket = socket(new ZlinkConnectException(ConnectResult.NOT_FOUND, errno), calls);
        assertDoesNotThrow(() -> ZLinkJavaSocketSupport.disconnectRidIfPresent(socket, PEER));
        assertEquals(1, calls.get(), "one binding invocation, without route lookup or retry");
    }

    @ParameterizedTest
    @EnumSource(
            value = ConnectResult.class,
            names = {"OK", "NOT_FOUND"},
            mode = EnumSource.Mode.EXCLUDE)
    void otherTypedFailuresRetainTheirIdentity(ConnectResult result) {
        var calls = new AtomicInteger();
        var failure = new ZlinkConnectException(result, 2);
        var socket = socket(failure, calls);
        assertSame(
                failure,
                assertThrows(
                        ZlinkConnectException.class,
                        () -> ZLinkJavaSocketSupport.disconnectRidIfPresent(socket, PEER)));
        assertEquals(1, calls.get());
    }

    @Test
    void successfulClosureInvokesBindingOnce() {
        var calls = new AtomicInteger();
        var socket = socket(null, calls);
        assertDoesNotThrow(() -> ZLinkJavaSocketSupport.disconnectRidIfPresent(socket, PEER));
        assertEquals(1, calls.get());
    }

    @Test
    void unrelatedFailureIsNotConsumed() {
        var calls = new AtomicInteger();
        var failure = new IllegalStateException("socket owner failed");
        var socket = socket(failure, calls);
        assertSame(
                failure,
                assertThrows(
                        IllegalStateException.class,
                        () -> ZLinkJavaSocketSupport.disconnectRidIfPresent(socket, PEER)));
        assertEquals(1, calls.get());
    }

    private static RouterSocket socket(RuntimeException failure, AtomicInteger calls) {
        return (RouterSocket)
                Proxy.newProxyInstance(
                        RouterSocket.class.getClassLoader(),
                        new Class<?>[] {RouterSocket.class},
                        (proxy, method, args) -> {
                            assertEquals("disconnectRid", method.getName());
                            assertEquals(PEER, args[0]);
                            calls.incrementAndGet();
                            if (failure != null) throw failure;
                            return null;
                        });
    }

    public static void main(String[] args) {
        var suite = new ZLinkJavaLivenessDisconnectResultTest();
        suite.nativeAbsentRidRemainsNotFoundWhileLivenessClosureIsSatisfied();
        for (int errno : new int[] {0, 2, 107})
            suite.onlyTypedAbsenceIsConsumedRegardlessOfErrno(errno);
        int propagated = 0;
        for (var result : ConnectResult.values()) {
            if (result == ConnectResult.OK || result == ConnectResult.NOT_FOUND) continue;
            suite.otherTypedFailuresRetainTheirIdentity(result);
            propagated++;
        }
        suite.successfulClosureInvokesBindingOnce();
        suite.unrelatedFailureIsNotConsumed();
        System.out.printf(
                "nativeNotFoundControl=passed absentErrnoCases=3 typedErrorsPropagated=%d successCalls=1 unrelatedFailure=preserved%n",
                propagated);
    }
}
