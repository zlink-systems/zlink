package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.TopicMessage;
import systems.zlink.contracts.sockets.PubSocket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.SubSocket;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.configuration.FanoutChannelBuilder;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendDealerSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendPublisherSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSubscriberSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkChannelBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkMonitoringBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkSpotBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkStreamBackendAdapter;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.function.Function;

final class ZLinkFanoutNoDropTest {
    private static final String CHANNEL = "events";
    private static final String TOPIC = "topic";
    private static final long HWM_BYTES = 4_096L;
    private static final Duration SEND_TIMEOUT = Duration.ofSeconds(10);
    private static final Duration DEADLINE_TIMEOUT = Duration.ofMillis(500);
    private static final Duration RECEIVE_TIMEOUT = Duration.ofSeconds(3);
    private static final int MAX_FILL_ATTEMPTS = 256;
    private static final int STABLE_BACKPRESSURE_ATTEMPTS = 50;

    @Test
    void a_defaultNoDropLetsFastSubscriberReceiveAfterSlowSubscriberFills() throws Exception {
        try (Scenario scenario = Scenario.start(false, false, SEND_TIMEOUT)) {
            int published = 128;
            for (int index = 0; index < published; index++) {
                scenario.publish(payload(index));
                assertNotNull(receive(scenario.fast, RECEIVE_TIMEOUT));
            }

            int slowReceived = drainCount(scenario.slow);
            assertTrue(slowReceived > 0, "the slow subscriber never received a record");
            assertTrue(
                    slowReceived < published, "the slow subscriber did not exceed its receive HWM");
        }
    }

    @Test
    void b_noDropWaitsWithoutPartialDeliveryThenAllSubscribersReceive() throws Exception {
        try (Scenario scenario = Scenario.start(true, true, SEND_TIMEOUT);
                ExecutorService publisher = Executors.newSingleThreadExecutor()) {
            fillToBackpressure(scenario);
            CountDownLatch publishEntered = new CountDownLatch(1);
            CompletableFuture<Void> completion =
                    CompletableFuture.runAsync(
                            () -> {
                                publishEntered.countDown();
                                scenario.publish(payload(MAX_FILL_ATTEMPTS));
                            },
                            publisher);
            assertTrue(publishEntered.await(1, TimeUnit.SECONDS));

            assertNull(
                    receiveNow(scenario.fast),
                    "the blocked record was partially delivered to the ready subscriber");
            assertNotNull(
                    receive(scenario.slow, RECEIVE_TIMEOUT),
                    "the slow subscriber had no queued record to release");

            completion.get(5, TimeUnit.SECONDS);
            byte[] fastRecord = receive(scenario.fast, RECEIVE_TIMEOUT);
            assertNotNull(fastRecord);
            assertArrayEquals(
                    fastRecord, receiveMatching(scenario.slow, fastRecord, RECEIVE_TIMEOUT));
        }
    }

    @Test
    void c_noDropMapsSendTimeoutToDeadlineExceeded() throws Exception {
        try (Scenario scenario = Scenario.start(true, true, DEADLINE_TIMEOUT)) {
            fillToBackpressure(scenario);

            CompletionException failure =
                    assertThrows(
                            CompletionException.class,
                            () -> scenario.publish(payload(MAX_FILL_ATTEMPTS)));
            ZLinkFrameworkException frameworkFailure =
                    org.junit.jupiter.api.Assertions.assertInstanceOf(
                            ZLinkFrameworkException.class, unwrap(failure));
            assertEquals(ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED, frameworkFailure.kind());
            assertNull(receiveNow(scenario.fast));
        }
    }

    @Test
    void d_noDropWithoutPublisherRoleFailsStartupValidation() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addFanoutChannel(CHANNEL).setNoDrop(true).connect("inproc://subscriber-only");

        ZLinkConfigurationException failure =
                assertThrows(ZLinkConfigurationException.class, options::validate);
        assertTrue(failure.getMessage().contains("NoDrop requires the publisher role"));
    }

    @Test
    void e_builderValueIsAppliedToPublisherSocketOption() {
        LowHwmBackendProvider backend = new LowHwmBackendProvider(SEND_TIMEOUT);
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addFanoutChannel(CHANNEL).setNoDrop(true).enablePublisher("inproc://fanout-option");

        try (ZLinkFrameworkRuntime ignored =
                ZLinkFrameworkRuntimeTestAccess.start(options, backend)) {
            assertTrue(backend.publisher().options().noDrop());
        }
    }

    private static void fillToBackpressure(Scenario scenario) throws Exception {
        int consecutiveBackpressure = 0;
        for (int index = 0; index < MAX_FILL_ATTEMPTS * 20; index++) {
            if (!scenario.backend.tryPublishNoWait(payload(index))) {
                assertNull(receiveNow(scenario.fast));
                consecutiveBackpressure++;
                if (consecutiveBackpressure == STABLE_BACKPRESSURE_ATTEMPTS) {
                    return;
                }
                Thread.sleep(2);
                continue;
            }
            consecutiveBackpressure = 0;
            assertNotNull(receive(scenario.fast, RECEIVE_TIMEOUT));
        }
        throw new AssertionError("the slow subscriber did not reach its receive HWM");
    }

    private static String payload(int index) {
        return "record-" + index + "-" + "x".repeat(1_024);
    }

    private static byte[] receive(SubSocket subscriber, Duration timeout)
            throws InterruptedException {
        long deadline = System.nanoTime() + timeout.toNanos();
        byte[] received;
        while ((received = receiveNow(subscriber)) == null && System.nanoTime() < deadline) {
            Thread.sleep(1);
        }
        return received;
    }

    private static byte[] receiveMatching(SubSocket subscriber, byte[] expected, Duration timeout)
            throws InterruptedException {
        long deadline = System.nanoTime() + timeout.toNanos();
        while (System.nanoTime() < deadline) {
            byte[] received = receiveNow(subscriber);
            if (received == null) {
                Thread.sleep(1);
            } else if (java.util.Arrays.equals(expected, received)) {
                return received;
            }
        }
        return null;
    }

    private static byte[] receiveNow(SubSocket subscriber) {
        try (TopicMessage received = new TopicMessage()) {
            if (!subscriber.subscribe(received, RecvFlags.DONT_WAIT)) {
                return null;
            }
            return received.firstPart().toByteArray();
        }
    }

    private static int drainCount(SubSocket subscriber) {
        int count = 0;
        while (receiveNow(subscriber) != null) {
            count++;
        }
        return count;
    }

    private static Throwable unwrap(Throwable failure) {
        Throwable current = failure;
        while (current instanceof CompletionException || current instanceof ExecutionException) {
            current = current.getCause();
        }
        return current;
    }

    private static final class Scenario implements AutoCloseable {
        private final LowHwmBackendProvider backend;
        private final ZLinkFrameworkRuntime runtime;
        private final SubSocket slow;
        private final SubSocket fast;

        private Scenario(
                LowHwmBackendProvider backend,
                ZLinkFrameworkRuntime runtime,
                SubSocket slow,
                SubSocket fast) {
            this.backend = backend;
            this.runtime = runtime;
            this.slow = slow;
            this.fast = fast;
        }

        private static Scenario start(boolean configureNoDrop, boolean noDrop, Duration sendTimeout)
                throws Exception {
            LowHwmBackendProvider backend = new LowHwmBackendProvider(sendTimeout);
            DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
            FanoutChannelBuilder channel = options.addFanoutChannel(CHANNEL);
            if (configureNoDrop) {
                channel.setNoDrop(noDrop);
            }
            String endpoint = "inproc://fanout-nodrop-" + System.nanoTime();
            channel.enablePublisher(endpoint);
            ZLinkFrameworkRuntime runtime = ZLinkFrameworkRuntimeTestAccess.start(options, backend);
            SubSocket slow = null;
            SubSocket fast = null;
            try {
                slow = backend.connectSubscriber(endpoint, TOPIC);
                fast = backend.connectSubscriber(endpoint, "top");
                Scenario scenario = new Scenario(backend, runtime, slow, fast);
                scenario.awaitSubscribers();
                return scenario;
            } catch (Throwable failure) {
                if (fast != null) fast.close();
                if (slow != null) slow.close();
                runtime.close();
                throw failure;
            }
        }

        private void publish(String payload) {
            runtime.fanout().publish(CHANNEL, TOPIC, payload).submit().toCompletableFuture().join();
        }

        private void awaitSubscribers() throws InterruptedException {
            boolean slowReady = false;
            boolean fastReady = false;
            long deadline = System.nanoTime() + RECEIVE_TIMEOUT.toNanos();
            int attempt = 0;
            while ((!slowReady || !fastReady) && System.nanoTime() < deadline) {
                publish("warmup-" + attempt++);
                slowReady |= receiveNow(slow) != null;
                fastReady |= receiveNow(fast) != null;
                if (!slowReady || !fastReady) {
                    Thread.sleep(1);
                }
            }
            assertTrue(slowReady, "the slow subscriber did not connect");
            assertTrue(fastReady, "the fast subscriber did not connect");
            drainCount(slow);
            drainCount(fast);
        }

        @Override
        public void close() {
            RuntimeException failure = null;
            try {
                fast.close();
            } catch (RuntimeException closeFailure) {
                failure = closeFailure;
            }
            try {
                slow.close();
            } catch (RuntimeException closeFailure) {
                if (failure == null) failure = closeFailure;
                else failure.addSuppressed(closeFailure);
            }
            try {
                runtime.close();
            } catch (RuntimeException closeFailure) {
                if (failure == null) failure = closeFailure;
                else failure.addSuppressed(closeFailure);
            }
            if (failure != null) throw failure;
        }
    }

    private static final class LowHwmBackendProvider implements ZLinkBackendAdapterProvider {
        private final ZLinkJavaBackendAdapterFactory delegate =
                new ZLinkJavaBackendAdapterFactory();
        private final LowHwmChannelAdapter channels;

        private LowHwmBackendProvider(Duration sendTimeout) {
            channels = new LowHwmChannelAdapter(sendTimeout);
        }

        @Override
        public ZLinkChannelBackendAdapter createChannelAdapter(ZLinkBackendAdapterOptions options) {
            return channels;
        }

        @Override
        public ZLinkSpotBackendAdapter createSpotAdapter(ZLinkBackendAdapterOptions options) {
            return delegate.createSpotAdapter(options);
        }

        @Override
        public ZLinkMeshBackendAdapter createMeshAdapter(ZLinkBackendAdapterOptions options) {
            return delegate.createMeshAdapter(options);
        }

        @Override
        public ZLinkStreamBackendAdapter createStreamAdapter(ZLinkBackendAdapterOptions options) {
            return delegate.createStreamAdapter(options);
        }

        @Override
        public ZLinkMonitoringBackendAdapter createMonitoringAdapter(
                ZLinkBackendAdapterOptions options) {
            return delegate.createMonitoringAdapter(options);
        }

        @Override
        public Function<ZLinkBackendObject, Duration> admissionTimeout() {
            return delegate.admissionTimeout();
        }

        private PubSocket publisher() {
            return channels.publisher();
        }

        private boolean tryPublishNoWait(String payload) {
            return channels.tryPublishNoWait(payload);
        }

        private SubSocket connectSubscriber(String endpoint, String subscription) {
            return channels.connectSubscriber(endpoint, subscription);
        }
    }

    private static final class LowHwmChannelAdapter implements ZLinkChannelBackendAdapter {
        private Context context;
        private PubSocket publisher;
        private final Duration sendTimeout;

        private LowHwmChannelAdapter(Duration sendTimeout) {
            this.sendTimeout = sendTimeout;
        }

        @Override
        public ZLinkBackendContext createContext() {
            context = Zlink.createContext();
            return new ZLinkJavaContext(context);
        }

        @Override
        public ZLinkBackendDealerSocket createDealerSocket(ZLinkBackendContext ignored) {
            throw new UnsupportedOperationException();
        }

        @Override
        public ZLinkBackendRouterSocket createRouterSocket(ZLinkBackendContext ignored) {
            throw new UnsupportedOperationException();
        }

        @Override
        public ZLinkBackendPublisherSocket createPublisherSocket(ZLinkBackendContext ignored) {
            publisher = ZLinkJavaSocketOptions.configureFrameworkSocket(context.createPubSocket());
            publisher.options().sendHwm(HWM_BYTES);
            publisher.options().sendTimeout(sendTimeout);
            return new ZLinkJavaPublisherSocket(publisher);
        }

        @Override
        public ZLinkBackendSubscriberSocket createSubscriberSocket(ZLinkBackendContext ignored) {
            throw new UnsupportedOperationException();
        }

        private PubSocket publisher() {
            return java.util.Objects.requireNonNull(publisher);
        }

        private boolean tryPublishNoWait(String payload) {
            try (Message message = Message.from(payload)) {
                publisher().publish(TOPIC).message(message).flags(SendFlags.DONT_WAIT).submit();
                return true;
            } catch (ZlinkSubmitException failure) {
                if (failure.getResult() == SubmitResult.BACKPRESSURED) {
                    return false;
                }
                throw failure;
            }
        }

        private SubSocket connectSubscriber(String endpoint, String subscription) {
            SubSocket subscriber = context.createSubSocket();
            subscriber.options().linger(Duration.ZERO);
            subscriber.options().recvHwm(HWM_BYTES);
            subscriber.setSubscription(subscription);
            subscriber.connect(endpoint);
            return subscriber;
        }
    }
}
