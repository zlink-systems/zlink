package systems.zlink.framework.messaging;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

final class ZLinkMessageOwnershipTest {
    @Test
    void encodedMessageReusesItsImmutablePayloadAcrossDecodeAndEncode() {
        ZLinkEncodedPayload payload = ZLinkEncodedPayload.from(
            "payload".getBytes(StandardCharsets.UTF_8));
        RecordingSerializer serializer = new RecordingSerializer();
        ZLinkMessage message = ZLinkMessage.fromEncoded(payload, serializer);

        message.decode(String.class);

        assertSame(payload, serializer.decoded);
        assertSame(payload, message.toEncodedPayload(serializer));
        assertEquals(ZLinkMessage.class, message.declaredType());
    }

    @Test
    void explicitlyEmptyEncodedMessageRemainsEmpty() {
        ZLinkMessage message = ZLinkMessage.fromEncoded(
            ZLinkEncodedPayload.from(new byte[0]),
            new RecordingSerializer());

        assertTrue(message.isEmpty());
        assertEquals(ZLinkMessage.class, message.declaredType());
    }

    @Test
    void objectBackedMessagePreservesItsDeclaredTypeForEncoding() {
        RecordingSerializer serializer = new RecordingSerializer();
        ZLinkMessage message = ZLinkMessage.of(new DerivedValue(), BaseValue.class);

        message.toEncodedPayload(serializer);

        assertEquals(BaseValue.class, message.declaredType());
        assertEquals(BaseValue.class, serializer.encodedType);
        assertThrows(
            IllegalArgumentException.class,
            () -> ZLinkMessage.of("not-a-number", Number.class));
    }

    @Test
    void encodedMessageSharesOneTypedDecodeAcrossConcurrentAccessors()
        throws Exception {
        Probe decoded = new Probe("decoded");
        CountDownLatch entered = new CountDownLatch(1);
        CountDownLatch release = new CountDownLatch(1);
        CountingSerializer serializer = new CountingSerializer(
            decoded, null, entered, release);
        ZLinkMessage message = ZLinkMessage.fromEncoded(
            ZLinkEncodedPayload.from(new byte[] {1}), serializer);
        var executor = Executors.newFixedThreadPool(8);
        try {
            List<java.util.concurrent.Future<Probe>> results =
                java.util.stream.IntStream.range(0, 16)
                    .mapToObj(ignored -> executor.submit(
                        () -> message.decode(Probe.class)))
                    .toList();
            assertTrue(entered.await(5, TimeUnit.SECONDS));
            release.countDown();
            for (var result : results) {
                assertSame(decoded, result.get(5, TimeUnit.SECONDS));
            }
        } finally {
            release.countDown();
            executor.shutdownNow();
        }

        ZLinkFrameworkException mismatch = assertThrows(
            ZLinkFrameworkException.class,
            () -> message.decode(OtherProbe.class));
        assertEquals(ZLinkFrameworkErrorKind.TYPE_MISMATCH, mismatch.kind());
        assertEquals(1, serializer.calls.get());
    }

    @Test
    void encodedMessageRetainsFirstDecodeFailureForEveryTargetType()
        throws Exception {
        ZLinkFrameworkException malformed = new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.PROTOCOL_ERROR, "malformed");
        CountingSerializer serializer = new CountingSerializer(null, malformed);
        ZLinkMessage message = ZLinkMessage.fromEncoded(
            ZLinkEncodedPayload.from(new byte[] {1}), serializer);
        var executor = Executors.newFixedThreadPool(8);
        try {
            List<? extends java.util.concurrent.Future<?>> results =
                java.util.stream.IntStream.range(0, 16)
                    .mapToObj(index -> executor.submit(() -> index % 2 == 0
                        ? message.decode(Probe.class)
                        : message.decode(OtherProbe.class)))
                    .toList();
            for (var result : results) {
                ExecutionException failure = assertThrows(
                    ExecutionException.class,
                    () -> result.get(5, TimeUnit.SECONDS));
                assertSame(malformed, failure.getCause());
            }
        } finally {
            executor.shutdownNow();
        }
        assertSame(
            malformed,
            assertThrows(
                ZLinkFrameworkException.class,
                () -> message.decode(OtherProbe.class)));
        assertEquals(1, serializer.calls.get());
    }

    private static class BaseValue {
    }

    private static final class DerivedValue extends BaseValue {
    }

    private record Probe(String value) {
    }

    private record OtherProbe(String value) {
    }

    private static final class RecordingSerializer implements ZLinkMessageSerializer {
        private ZLinkEncodedPayload decoded;
        private Class<?> encodedType;

        @Override
        public <T> ZLinkEncodedPayload serialize(T value) {
            return ZLinkEncodedPayload.from(
                String.valueOf(value).getBytes(StandardCharsets.UTF_8));
        }

        @Override
        public <T> ZLinkEncodedPayload serialize(T value, Class<?> declaredType) {
            encodedType = declaredType;
            return serialize(value);
        }

        @Override
        public <T> T deserialize(ZLinkEncodedPayload payload, Class<T> type) {
            decoded = payload;
            return type.cast(new String(payload.bytes(), StandardCharsets.UTF_8));
        }
    }

    private static final class CountingSerializer implements ZLinkMessageSerializer {
        private final Object decoded;
        private final RuntimeException failure;
        private final CountDownLatch entered;
        private final CountDownLatch release;
        private final AtomicInteger calls = new AtomicInteger();

        private CountingSerializer(Object decoded, RuntimeException failure) {
            this(decoded, failure, null, null);
        }

        private CountingSerializer(
            Object decoded,
            RuntimeException failure,
            CountDownLatch entered,
            CountDownLatch release) {
            this.decoded = decoded;
            this.failure = failure;
            this.entered = entered;
            this.release = release;
        }

        @Override
        public <T> ZLinkEncodedPayload serialize(T value) {
            throw new UnsupportedOperationException();
        }

        @Override
        public <T> T deserialize(ZLinkEncodedPayload payload, Class<T> type) {
            calls.incrementAndGet();
            if (entered != null) {
                entered.countDown();
            }
            if (release != null) {
                try {
                    if (!release.await(5, TimeUnit.SECONDS)) {
                        throw new AssertionError("decode release timed out");
                    }
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                    throw new AssertionError(interrupted);
                }
            }
            if (failure != null) {
                throw failure;
            }
            return type.cast(decoded);
        }
    }
}
