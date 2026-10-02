package systems.zlink.httpclient.internal;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.io.UncheckedIOException;
import java.time.Duration;
import java.util.Map;

final class HttpApplicationFailureR3Test {
    @Test
    void providerIoFailureIsApplicationFailure() {
        UncheckedIOException failure =
                new UncheckedIOException(new IOException("provider failure"));
        ProviderInputStream stream =
                new ProviderInputStream(
                        () -> {
                            throw failure;
                        });
        ZLinkFrameworkException observed =
                assertThrows(ZLinkFrameworkException.class, stream::read);
        assertEquals(ZLinkFrameworkErrorKind.INTERNAL_FAILURE, observed.kind());
        assertSame(failure, observed.getCause());
    }

    @Test
    void sinkIoFailureIsApplicationFailure() {
        UncheckedIOException failure = new UncheckedIOException(new IOException("sink failure"));
        HttpClientOptions options =
                new HttpClientOptions(
                        "http://localhost",
                        Duration.ofSeconds(3),
                        1024,
                        Map.of(),
                        null,
                        null,
                        0,
                        0,
                        false,
                        null,
                        null,
                        false);
        ResponseBodyReader reader = new ResponseBodyReader(options);
        ZLinkFrameworkException observed =
                assertThrows(
                        ZLinkFrameworkException.class,
                        () ->
                                reader.streamToSink(
                                        new ByteArrayInputStream(new byte[] {1}),
                                        chunk -> {
                                            throw failure;
                                        }));
        assertEquals(ZLinkFrameworkErrorKind.INTERNAL_FAILURE, observed.kind());
        assertSame(failure, observed.getCause());
    }

    @Test
    void applicationNestedCodedFailureIsNotReclassified() {
        var coded = HttpClientErrors.unavailable(new IOException("coded"));
        var wrapped = new IllegalStateException("provider", coded);
        var observed =
                assertThrows(
                        ZLinkFrameworkException.class,
                        () ->
                                new ProviderInputStream(
                                                () -> {
                                                    throw wrapped;
                                                })
                                        .read());
        assertEquals(ZLinkFrameworkErrorKind.INTERNAL_FAILURE, observed.kind());
        assertSame(wrapped, observed.getCause());
        var options =
                new HttpClientOptions(
                        "http://localhost",
                        Duration.ofSeconds(3),
                        1024,
                        Map.of(),
                        null,
                        null,
                        0,
                        0,
                        false,
                        null,
                        null,
                        false);
        var sinkObserved =
                assertThrows(
                        ZLinkFrameworkException.class,
                        () ->
                                new ResponseBodyReader(options)
                                        .streamToSink(
                                                new ByteArrayInputStream(new byte[] {1}),
                                                chunk -> {
                                                    throw wrapped;
                                                }));
        assertEquals(ZLinkFrameworkErrorKind.INTERNAL_FAILURE, sinkObserved.kind());
        assertSame(wrapped, sinkObserved.getCause());
    }

    @Test
    void applicationDirectCodedFailurePreservesIdentity() {
        var coded = HttpClientErrors.unavailable(new IOException("coded"));
        assertSame(
                coded,
                assertThrows(
                        ZLinkFrameworkException.class,
                        () ->
                                new ProviderInputStream(
                                                () -> {
                                                    throw coded;
                                                })
                                        .read()));
        var options =
                new HttpClientOptions(
                        "http://localhost",
                        Duration.ofSeconds(3),
                        1024,
                        Map.of(),
                        null,
                        null,
                        0,
                        0,
                        false,
                        null,
                        null,
                        false);
        assertSame(
                coded,
                assertThrows(
                        ZLinkFrameworkException.class,
                        () ->
                                new ResponseBodyReader(options)
                                        .streamToSink(
                                                new ByteArrayInputStream(new byte[] {1}),
                                                chunk -> {
                                                    throw coded;
                                                })));
    }
}
