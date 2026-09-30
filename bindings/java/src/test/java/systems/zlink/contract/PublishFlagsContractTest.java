package systems.zlink.contract;

import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.eventing.SocketMonitor;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.PublishOperation;
import systems.zlink.contracts.sockets.PubSocket;
import systems.zlink.contracts.sockets.PubSocketOptions;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.SubSocket;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.contracts.sockets.XPubSocket;
import systems.zlink.internal.NativeErrorCodes;
import java.time.Duration;
import java.util.function.Consumer;
import java.util.function.Function;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

public class PublishFlagsContractTest {
    private static final String TOPIC = "blocking-publish";
    private static final byte[] PAYLOAD = new byte[8 * 1024];

    @Test
    public void pubPublishWithNoneWaitsForSendTimeoutWhenNoDropQueueIsFull()
        throws Exception {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext();
             PubSocket pub = context.createPubSocket();
             SubSocket sub = context.createSubSocket();
             SocketMonitor pubMonitor = pub.monitorOpen(
                 MonitorEventType.CONNECTION_READY);
             SocketMonitor subMonitor = sub.monitorOpen(
                 MonitorEventType.CONNECTION_READY)) {
            verifyNoneWaitsForSendTimeout(pub.options(), pub::bind,
                pub::publish, pubMonitor, subMonitor, sub, "pub-flags-hwm");
        }
    }

    @Test
    public void xpubPublishWithNoneWaitsForSendTimeoutWhenNoDropQueueIsFull()
        throws Exception {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext();
             XPubSocket pub = context.createXPubSocket();
             SubSocket sub = context.createSubSocket();
             SocketMonitor pubMonitor = pub.monitorOpen(
                 MonitorEventType.CONNECTION_READY);
             SocketMonitor subMonitor = sub.monitorOpen(
                 MonitorEventType.CONNECTION_READY)) {
            verifyNoneWaitsForSendTimeout(pub.options(), pub::bind,
                pub::publish, pubMonitor, subMonitor, sub, "xpub-flags-hwm");
        }
    }

    private static void verifyNoneWaitsForSendTimeout(
        PubSocketOptions options,
        Consumer<String> bind,
        Function<String, PublishOperation> publish,
        SocketMonitor pubMonitor,
        SocketMonitor subMonitor,
        SubSocket sub,
        String endpointPrefix) throws Exception {
        options.noDrop(true);
        options.sendHwm(64 * 1024);
        options.sendTimeout(Duration.ofSeconds(1));
        String endpoint = TestSupport.inprocEndpoint(endpointPrefix);
        bind.accept(endpoint);
        sub.setSubscription(TOPIC);
        sub.connect(endpoint);
        TestSupport.awaitMonitorEvent(subMonitor,
            MonitorEventType.CONNECTION_READY);
        TestSupport.awaitMonitorEvent(pubMonitor,
            MonitorEventType.CONNECTION_READY);
        TestSupport.awaitCondition(() -> options.topicsCount() == 1);

        assertTrue(fillUntilBackpressured(publish),
            "DONT_WAIT must fill the NODROP publisher queue");

        long start = System.nanoTime();
        ZlinkSubmitException failure;
        try (Message part = Message.from(PAYLOAD)) {
            failure = assertThrows(ZlinkSubmitException.class,
                () -> publish.apply(TOPIC).message(part)
                    .flags(SendFlags.NONE).submit());
        }
        long elapsedNanos = System.nanoTime() - start;

        assertEquals(SubmitResult.BACKPRESSURED, failure.getResult());
        assertEquals(NativeErrorCodes.EAGAIN, failure.getNativeErrno());
        assertTrue(elapsedNanos >= Duration.ofMillis(750).toNanos(),
            "flags NONE returned before its 1-second send timeout: "
                + Duration.ofNanos(elapsedNanos).toMillis() + "ms");
    }

    private static boolean fillUntilBackpressured(
        Function<String, PublishOperation> publish) {
        for (int attempt = 0; attempt < 32; attempt++) {
            try (Message part = Message.from(PAYLOAD)) {
                try {
                    publish.apply(TOPIC).message(part)
                        .flags(SendFlags.DONT_WAIT).submit();
                } catch (ZlinkSubmitException failure) {
                    if (failure.getResult() != SubmitResult.BACKPRESSURED) {
                        throw failure;
                    }
                    return true;
                }
            }
        }
        return false;
    }
}
