package systems.zlink.framework.runtime.host;

import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;

import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;

import systems.zlink.framework.runtime.internal.backend.*;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.time.Duration;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletionException;
import java.util.concurrent.TimeoutException;
import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.CloseResult;
import systems.zlink.contracts.errors.ZlinkCloseException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.errors.ZLinkConfigurationException;

final class ZLinkAsyncSubmitterTest {
    @Test
    void oneWaySendWaitsUntilTheAdmissionDeadline() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addClientServerChannel("profile").client().connect("inproc://profile");

        try (ZLinkFrameworkRuntime runtime =
                 ZLinkFrameworkRuntimeTestAccess.start(options, new BackpressuredBackend())) {
            var result = runtime.client()
                .sendToChannel("profile", "hello")
                .submit();

            assertEquals(
                2,
                systems.zlink.framework.runtime.messaging.OneWayTestStatus.status(result));
        }
    }

    @Test
    void oneWaySubmitUsesOneNonBlockingAdmissionAttempt() {
        RecordingPublishBackend backend = new RecordingPublishBackend();
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        { var channel = options.addFanoutChannel("events").enablePublisher("inproc://events"); };

        try (ZLinkFrameworkRuntime runtime = ZLinkFrameworkRuntimeTestAccess.start(options, backend)) {
            var submission = runtime.fanout()
                .publish("events", "payload")
                .submit();

            assertEquals(
                0,
                systems.zlink.framework.runtime.messaging.OneWayTestStatus.status(submission));
            assertEquals(SendFlags.DONT_WAIT, backend.flags);
        }
    }

    @Test
    void submittingTheSameOneWayCallTwiceFailsWithoutASecondBackendAttempt() {
        RecordingPublishBackend backend = new RecordingPublishBackend();
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addFanoutChannel("events").enablePublisher("inproc://events");

        try (ZLinkFrameworkRuntime runtime = ZLinkFrameworkRuntimeTestAccess.start(options, backend)) {
            var call = runtime.fanout().publish("events", "payload");

            assertEquals(
                0,
                systems.zlink.framework.runtime.messaging.OneWayTestStatus.status(call.submit()));
            CompletionException duplicate = assertThrows(
                CompletionException.class,
                () -> call.submit().toCompletableFuture().join());

            var frameworkError = assertInstanceOf(
                systems.zlink.framework.errors.ZLinkFrameworkException.class,
                duplicate.getCause());
            assertEquals(
                systems.zlink.framework.errors.ZLinkFrameworkErrorKind.INVALID_OPERATION,
                frameworkError.kind());
            assertEquals(1, backend.submissions);
        }
    }

    @Test
    void submit_failsPendingItemWhenSendTimeoutExpires() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(20));
        options.addClientServerChannel("profile").client().connect("inproc://profile");

        try (ZLinkFrameworkRuntime runtime = ZLinkFrameworkRuntimeTestAccess.start(options, new NoReplyBackend())) {
            CompletionException error = org.junit.jupiter.api.Assertions.assertThrows(
                CompletionException.class,
                () -> runtime.client()
                    .requestToChannel("profile", "hello")
                    .submit(String.class)
                    .toCompletableFuture()
                    .join());

            assertInstanceOf(TimeoutException.class, error.getCause());
        }
    }

    @Test
    void close_failsPendingItems() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofSeconds(5));
        options.addClientServerChannel("profile").client().connect("inproc://profile");

        ZLinkFrameworkRuntime runtime = ZLinkFrameworkRuntimeTestAccess.start(options, new NoReplyBackend());
        var pending = runtime.client()
            .requestToChannel("profile", "hello")
            .submit(String.class)
            .toCompletableFuture();

        runtime.close();

        CompletionException error = org.junit.jupiter.api.Assertions.assertThrows(
            CompletionException.class,
            pending::join);
        assertInstanceOf(ZLinkConfigurationException.class, error.getCause());
    }

    @Test
    void requestUsesGlobalDefaultRequestTimeout() {
        RecordingRequestBackend backend = new RecordingRequestBackend();
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofSeconds(2));
        options.addClientServerChannel("profile").client().connect("inproc://profile");

        try (ZLinkFrameworkRuntime runtime = ZLinkFrameworkRuntimeTestAccess.start(options, backend)) {
            var pending = runtime.client()
                .requestToChannel("profile", "hello")
                .submit(String.class)
                .toCompletableFuture();

            assertEquals(Duration.ofSeconds(2), backend.timeout);
            runtime.close();
            pending.cancel(true);
        }
    }

    @Test
    void close_ignoresBackendCloseExceptionDuringRuntimeShutdown() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addClientServerChannel("profile").client().connect("inproc://profile");

        ZLinkFrameworkRuntime runtime = ZLinkFrameworkRuntimeTestAccess.start(
            options,
            new CloseFailureBackend());

        assertDoesNotThrow(runtime::close);
    }

    private static class NoReplyBackend implements ZLinkBackendAdapterProvider, ZLinkChannelBackendAdapter {
        @Override
        public ZLinkChannelBackendAdapter createChannelAdapter(ZLinkBackendAdapterOptions options) {
            return this;
        }

        @Override
        public ZLinkSpotBackendAdapter createSpotAdapter(ZLinkBackendAdapterOptions options) {
            throw new UnsupportedOperationException();
        }

        @Override
        public ZLinkMonitoringBackendAdapter createMonitoringAdapter(ZLinkBackendAdapterOptions options) {
            throw new UnsupportedOperationException();
        }

        @Override
        public ZLinkStreamBackendAdapter createStreamAdapter(ZLinkBackendAdapterOptions options) {
            throw new UnsupportedOperationException();
        }

        @Override
        public ZLinkBackendContext createContext() {
            return new ZLinkBackendContext() {
                @Override public String name() { return "context"; }
                @Override public void shutdown() { }
                @Override public void close() { }
            };
        }

        @Override
        public ZLinkBackendDealerSocket createDealerSocket(ZLinkBackendContext context) {
            return new NoReplyDealer();
        }

        @Override
        public ZLinkBackendRouterSocket createRouterSocket(ZLinkBackendContext context) {
            throw new UnsupportedOperationException();
        }

        @Override
        public ZLinkBackendPublisherSocket createPublisherSocket(ZLinkBackendContext context) {
            throw new UnsupportedOperationException();
        }

        @Override
        public ZLinkBackendSubscriberSocket createSubscriberSocket(ZLinkBackendContext context) {
            throw new UnsupportedOperationException();
        }
    }

    private static final class RecordingPublishBackend extends NoReplyBackend {
        private SendFlags flags;
        private int submissions;

        @Override
        public ZLinkBackendPublisherSocket createPublisherSocket(ZLinkBackendContext context) {
            return new RecordingPublisher(this);
        }
    }

    private static final class CloseFailureBackend extends NoReplyBackend {
        @Override
        public ZLinkBackendDealerSocket createDealerSocket(ZLinkBackendContext context) {
            return new CloseFailureDealer();
        }
    }

    private static final class BackpressuredBackend extends NoReplyBackend {
        @Override
        public ZLinkBackendDealerSocket createDealerSocket(ZLinkBackendContext context) {
            return new NoReplyDealer() {
                @Override public boolean send(List<Message> parts, SendFlags flags) {
                    return false;
                }
                public Duration admissionTimeout() {
                    return Duration.ofMillis(20);
                }
            };
        }
    }

    private static final class RecordingRequestBackend extends NoReplyBackend {
        private Duration timeout;

        @Override
        public ZLinkBackendDealerSocket createDealerSocket(ZLinkBackendContext context) {
            return new NoReplyDealer() {
                @Override public boolean request(
                    List<Message> parts,
                    ZLinkBackendRequestCallback callback,
                    SendFlags flags,
                    Duration timeout) {
                    RecordingRequestBackend.this.timeout = timeout;
                    if (isClientServerHello(parts)) {
                        return super.request(
                            parts, callback, flags, timeout);
                    }
                    return true;
                }
            };
        }
    }

    private static class NoReplyDealer implements ZLinkBackendDealerSocket {
        @Override public String name() { return "dealer"; }
        @Override public void bind(String endpoint) { }
        @Override public void connect(String endpoint) { }
        @Override public void disconnect(String endpoint) { }
        @Override public void setChannelName(String channelName) { }
        @Override public boolean send(List<Message> parts, SendFlags flags) { return true; }
        @Override public boolean request(
            List<Message> parts,
            ZLinkBackendRequestCallback callback,
            SendFlags flags,
            Duration timeout) {
            if (isClientServerHello(parts)) {
                Message response = Message.from(clientServerAdmit());
                callback.handle(new ZLinkBackendReceived(
                    ZLinkBackendRequestResult.OK,
                    Optional.empty(),
                    Optional.empty(),
                    Optional.empty(),
                    List.of(response)));
            }
            return true;
        }
        @Override public ZLinkBackendReceived recv(ZLinkBackendRecvMode mode) { return null; }
        @Override public boolean waitForReadable(Duration timeout) { return false; }
        @Override public void close() { }
    }

    private static boolean isClientServerHello(List<Message> parts) {
        if (parts.size() != 1) {
            return false;
        }
        byte[] value = parts.get(0).toByteArray();
        return value.length >= 5
            && value[0] == 0x5a
            && value[1] == 0x4d
            && value[2] == 1
            && value[3] == 1;
    }

    private static byte[] clientServerAdmit() {
        ByteArrayOutputStream body = new ByteArrayOutputStream();
        text8(body, "profile");
        body.write(1);
        bytes8(body, RoutingId.from("fake-server").toBytes());
        body.writeBytes(ByteBuffer.allocate(8)
            .order(ByteOrder.BIG_ENDIAN).putLong(1).array());
        body.writeBytes(ByteBuffer.allocate(8)
            .order(ByteOrder.BIG_ENDIAN).putLong(1).array());
        body.writeBytes(ByteBuffer.allocate(4)
            .order(ByteOrder.BIG_ENDIAN).putInt(100).array());
        body.write(1);
        text8(body, "default");
        body.writeBytes(ByteBuffer.allocate(4)
            .order(ByteOrder.BIG_ENDIAN)
            .putInt(Integer.MAX_VALUE).array());
        byte[] endpoint =
            "inproc://profile".getBytes(StandardCharsets.UTF_8);
        body.writeBytes(ByteBuffer.allocate(2)
            .order(ByteOrder.BIG_ENDIAN)
            .putShort((short) endpoint.length).array());
        body.writeBytes(endpoint);

        ByteArrayOutputStream admission = new ByteArrayOutputStream();
        admission.write(2);
        admission.writeBytes(ByteBuffer.allocate(2)
            .order(ByteOrder.BIG_ENDIAN)
            .putShort((short) body.size()).array());
        admission.writeBytes(body.toByteArray());

        ByteArrayOutputStream result = new ByteArrayOutputStream();
        result.writeBytes(new byte[] {0x5a, 0x4d, 1, 2, 0});
        result.write(2);
        result.writeBytes(ByteBuffer.allocate(4)
            .order(ByteOrder.BIG_ENDIAN)
            .putInt(admission.size()).array());
        result.writeBytes(admission.toByteArray());
        return result.toByteArray();
    }

    private static void text8(
        ByteArrayOutputStream output,
        String value) {
        bytes8(output, value.getBytes(StandardCharsets.UTF_8));
    }

    private static void bytes8(
        ByteArrayOutputStream output,
        byte[] value) {
        output.write(value.length);
        output.writeBytes(value);
    }

    private static final class CloseFailureDealer extends NoReplyDealer {
        @Override public void close() {
            throw new ZlinkCloseException(CloseResult.SHUTDOWN);
        }
    }

    private static final class RecordingPublisher implements ZLinkBackendPublisherSocket {
        private final RecordingPublishBackend owner;

        RecordingPublisher(RecordingPublishBackend owner) {
            this.owner = owner;
        }

        @Override public String name() { return "publisher"; }
        @Override public void bind(String endpoint) { }
        @Override public void setChannelName(String channelName) { }
        @Override public void setRoutingId(RoutingId routingId) { }
        @Override public boolean publish(String topic, List<Message> parts, SendFlags flags) {
            owner.flags = flags;
            owner.submissions++;
            return true;
        }
        @Override public void close() { }
    }

    @SuppressWarnings("unused")
    private static ZLinkBackendReceived received(List<Message> parts) {
        return new ZLinkBackendReceived(
            Optional.of(RoutingId.from("source")),
            Optional.empty(),
            Optional.of(1L),
            parts);
    }
}
