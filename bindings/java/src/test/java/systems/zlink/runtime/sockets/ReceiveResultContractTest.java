/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.runtime.sockets;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.function.Executable;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.messaging.SubscriptionEvent;
import systems.zlink.contracts.messaging.TopicMessage;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.PairSocket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RecvResult;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.SubSocket;
import systems.zlink.contracts.sockets.XPubSocket;
import systems.zlink.runtime.nativeapi.NativeErrno;

/**
 * Receive paths branch on the result Core returns, never on errno: a result with errno EINTR is
 * reported, not retried, and NO_DATA is no data whatever errno describes it
 * (core/doc/spec/core/03-errors.ko.md "Result와 errno 대응" §4).
 */
class ReceiveResultContractTest {
    private static final int EINTR = 4;
    private static final int ETIMEDOUT = 110;

    /** Scripted Core: every receive path reports the result Core returns. */
    @Test
    void receivePathsReportCoreResult() throws Exception {
        CompletionNativeFixture.runProbe(ReceiveResultProbe.class);
    }

    static final class ReceiveResultProbe {
        public static void main(String[] args) throws Throwable {
            CompletionNativeFixture core = new CompletionNativeFixture();
            core.passthroughSends = true;
            try (Context context = Zlink.createContext()) {
                pair(core, context);
                router(core, context);
                topics(core, context);
            }
            System.exit(0);
        }

        private static void pair(CompletionNativeFixture core, Context context) {
            try (PairSocket receiver = context.createPairSocket();
                 PairSocket sender = context.createPairSocket();
                 Received received = new Received()) {
                String endpoint = TestSupport.inprocEndpoint("result-pair");
                receiver.bind(endpoint);
                sender.connect(endpoint);
                try (Message ready = Message.from("ready")) {
                    sender.send().message(ready).submit_sync();
                }
                core.receiveFault("zlink_recv", RecvResult.INTERNAL_ERROR, EINTR);
                assertInternalEintr(() -> receiver.recv(received, RecvFlags.NONE));
                assertTrue(receiver.recv(received, RecvFlags.NONE));
            }
        }

        private static void router(CompletionNativeFixture core, Context context) {
            try (RouterSocket router = context.createRouterSocket();
                 DealerSocket dealer = context.createDealerSocket();
                 Received received = new Received()) {
                String endpoint = TestSupport.inprocEndpoint("result-router");
                router.bind(endpoint);
                dealer.connect(endpoint);
                try (Message ready = Message.from("ready")) {
                    dealer.send().message(ready).submit_sync();
                }
                core.receiveFault("zlink_router_recv", RecvResult.INTERNAL_ERROR, EINTR);
                assertInternalEintr(() -> router.recv(received, RecvFlags.NONE));
                assertTrue(router.recv(received, RecvFlags.NONE));
            }
        }

        private static void topics(CompletionNativeFixture core, Context context)
                throws Exception {
            try (XPubSocket pub = context.createXPubSocket();
                 SubSocket sub = context.createSubSocket();
                 var subMonitor = sub.monitorOpen(MonitorEventType.CONNECTION_READY)) {
                String endpoint = TestSupport.inprocEndpoint("result-topics");
                pub.bind(endpoint);
                sub.setSubscription("topic");
                sub.connect(endpoint);
                TestSupport.awaitMonitorEvent(subMonitor, MonitorEventType.CONNECTION_READY);

                SubscriptionEvent event = new SubscriptionEvent();
                core.receiveFault("zlink_xpub_recv", RecvResult.NO_DATA, ETIMEDOUT);
                assertFalse(pub.receiveSubscriptionEvent(event, RecvFlags.DONT_WAIT));
                core.receiveFault("zlink_xpub_recv", RecvResult.INTERNAL_ERROR, EINTR);
                assertInternalEintr(() -> pub.receiveSubscriptionEvent(event, RecvFlags.NONE));
                assertTrue(pub.receiveSubscriptionEvent(event, RecvFlags.NONE));

                try (Message part = Message.from("payload")) {
                    pub.publish("topic").message(part).submit();
                }
                try (TopicMessage received = new TopicMessage()) {
                    core.receiveFault("zlink_subscribe", RecvResult.INTERNAL_ERROR, EINTR);
                    assertInternalEintr(() -> sub.subscribe(received, RecvFlags.NONE));
                    assertTrue(sub.subscribe(received, RecvFlags.NONE));
                }
            }
        }

        private static void assertInternalEintr(Executable call) {
            ZlinkRecvException error = assertThrows(ZlinkRecvException.class, call);
            assertEquals(RecvResult.INTERNAL_ERROR, error.getResult());
            assertEquals(NativeErrno.EINTR, error.getNativeErrno());
        }
    }
}
