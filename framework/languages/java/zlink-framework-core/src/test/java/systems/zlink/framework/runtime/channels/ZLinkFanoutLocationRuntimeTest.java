package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.reflect.Method;
import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.testing.ZLinkLocationStoreTestAdapter;
import systems.zlink.framework.runtime.internal.locations.ZLinkFanoutPublisherDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.locations.ZLinkLocationPage;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteStatus;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendDealerSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendPublisherSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRecvMode;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitor;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitorEvent;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitorHandler;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSubscriberSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendTopicMessage;
import systems.zlink.framework.runtime.internal.backend.ZLinkChannelBackendAdapter;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;

final class ZLinkFanoutLocationRuntimeTest {
    @Test
    void lateMonitorEventCannotRemoveSuccessorConnection() throws Exception {
        Fixture fixture = new Fixture();
        ZLinkFanoutPublisherDescriptor descriptor = descriptor();
        fixture.runtime.start(List.of()).toCompletableFuture().join();

        reconcile(fixture.runtime, descriptor);
        ControlledSubscriber first = fixture.subscribers.get(0);
        first.monitor.emit("DISCONNECTED");
        assertTrue(first.closed);

        reconcile(fixture.runtime, descriptor);
        ControlledSubscriber successor = fixture.subscribers.get(1);
        first.monitor.emit("DISCONNECTED");

        assertFalse(successor.closed);
        assertEquals(2, fixture.subscribers.size());
        fixture.runtime.close();
    }

    @Test
    void completedReconcileCannotOpenConnectionAfterStop() throws Exception {
        Fixture fixture = new Fixture();
        fixture.runtime.start(List.of()).toCompletableFuture().join();
        fixture.runtime.stop().toCompletableFuture().join();

        reconcile(fixture.runtime, descriptor());

        assertTrue(fixture.subscribers.isEmpty());
        fixture.runtime.close();
    }

    @Test
    void subscriberReceiveRequiresSocketReadiness() throws Exception {
        Fixture fixture = new Fixture();
        fixture.runtime.start(List.of()).toCompletableFuture().join();
        reconcile(fixture.runtime, descriptor());

        Method receiveAvailable = ZLinkFanoutLocationRuntime.class
            .getDeclaredMethod("receiveAvailable", long.class);
        receiveAvailable.setAccessible(true);
        receiveAvailable.invoke(fixture.runtime, System.nanoTime());

        ControlledSubscriber subscriber = fixture.subscribers.getFirst();
        assertTrue(subscriber.readinessWaits > 0);
        assertEquals(0, subscriber.subscribeCalls);
        fixture.runtime.close();
    }

    private static void reconcile(
        ZLinkFanoutLocationRuntime runtime,
        ZLinkFanoutPublisherDescriptor descriptor) throws Exception {
        Method method = ZLinkFanoutLocationRuntime.class.getDeclaredMethod(
            "reconcileChannel", String.class, List.class);
        method.setAccessible(true);
        method.invoke(runtime, descriptor.channelName(), List.of(descriptor));
    }

    private static ZLinkFanoutPublisherDescriptor descriptor() {
        return new ZLinkFanoutPublisherDescriptor(
            "events",
            RoutingId.from("publisher"),
            7,
            1,
            "tcp://127.0.0.1:7001",
            ZLinkFrameworkRuntimeState.SERVING,
            "default",
            "owner",
            3,
            Instant.now());
    }

    private static final class Fixture {
        private final List<ControlledSubscriber> subscribers =
            new ArrayList<>();
        private final ZLinkFanoutLocationRuntime runtime =
            new ZLinkFanoutLocationRuntime(
                new EmptyStore(),
                () -> new ZLinkLocationOwnerToken("owner", 3),
                new Backend(subscribers),
                socket -> ((ControlledSubscriber) socket).monitor,
                new Context(),
                new ZLinkChannelSocketRegistry(),
                Duration.ofHours(1),
                100,
                (channel, message) ->
                    message.parts().forEach(
                        systems.zlink.contracts.messaging.Message::close));
    }

    private static final class EmptyStore
        extends ZLinkLocationStoreTestAdapter {
        @Override
        public java.util.concurrent.CompletionStage<ZLinkLocationWriteResult>
            updateFanoutPublisher(
                ZLinkFanoutPublisherDescriptor descriptor,
                systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteIntent
                    intent) {
            return CompletableFuture.completedFuture(
                ZLinkLocationWriteResult.stored(1, Instant.now()));
        }

        @Override
        public java.util.concurrent.CompletionStage<ZLinkLocationWriteStatus>
            removeFanoutPublisher(
                systems.zlink.framework.runtime.internal.locations.ZLinkFanoutPublisherDescriptorKey key,
                ZLinkLocationOwnerToken owner) {
            return CompletableFuture.completedFuture(
                ZLinkLocationWriteStatus.STORED);
        }

        @Override
        public java.util.concurrent.CompletionStage<
            ZLinkLocationPage<ZLinkFanoutPublisherDescriptor>>
            listFanoutPublishers(
                String channelName,
                systems.zlink.framework.locations.ZLinkPageRequest page) {
            return CompletableFuture.completedFuture(
                new ZLinkLocationPage<>(List.of(), null));
        }
    }

    private static final class Backend implements ZLinkChannelBackendAdapter {
        private final List<ControlledSubscriber> subscribers;

        private Backend(List<ControlledSubscriber> subscribers) {
            this.subscribers = subscribers;
        }

        @Override
        public ZLinkBackendSubscriberSocket createSubscriberSocket(
            ZLinkBackendContext context) {
            ControlledSubscriber subscriber = new ControlledSubscriber();
            subscribers.add(subscriber);
            return subscriber;
        }

        @Override
        public ZLinkBackendContext createContext() {
            throw new UnsupportedOperationException();
        }

        @Override
        public ZLinkBackendDealerSocket createDealerSocket(
            ZLinkBackendContext context) {
            throw new UnsupportedOperationException();
        }

        @Override
        public ZLinkBackendRouterSocket createRouterSocket(
            ZLinkBackendContext context) {
            throw new UnsupportedOperationException();
        }

        @Override
        public ZLinkBackendPublisherSocket createPublisherSocket(
            ZLinkBackendContext context) {
            throw new UnsupportedOperationException();
        }
    }

    private static final class ControlledSubscriber
        implements ZLinkBackendSubscriberSocket {
        private final Monitor monitor = new Monitor();
        private boolean closed;
        private int readinessWaits;
        private int subscribeCalls;

        @Override
        public void setChannelName(String channelName) {
        }

        @Override
        public void setSubscription(String topic) {
        }

        @Override
        public ZLinkBackendTopicMessage subscribe(
            ZLinkBackendRecvMode mode) {
            subscribeCalls++;
            throw new AssertionError("subscriber recv was called without readiness");
        }

        @Override
        public boolean waitForReadable(Duration timeout) {
            readinessWaits++;
            return false;
        }

        @Override
        public void connect(String endpoint) {
        }

        @Override
        public void disconnect(String endpoint) {
        }

        @Override
        public void bind(String endpoint) {
            throw new UnsupportedOperationException();
        }

        @Override
        public String name() {
            return "subscriber";
        }

        @Override
        public void close() {
            closed = true;
        }
    }

    private static final class Monitor implements ZLinkBackendSocketMonitor {
        private ZLinkBackendSocketMonitorHandler handler;

        private void emit(String event) {
            handler.handle(new ZLinkBackendSocketMonitorEvent(
                event, Optional.empty(), "", ""));
        }

        @Override
        public void onEvent(ZLinkBackendSocketMonitorHandler value) {
            handler = value;
        }

        @Override
        public ZLinkBackendSocketMonitorEvent recv() {
            return null;
        }

        @Override
        public String name() {
            return "monitor";
        }

        @Override
        public void close() {
        }
    }

    private static final class Context implements ZLinkBackendContext {
        @Override
        public void shutdown() {
        }

        @Override
        public String name() {
            return "context";
        }

        @Override
        public void close() {
        }
    }
}
