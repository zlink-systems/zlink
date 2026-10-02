package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;

import systems.zlink.contracts.messaging.Message;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.Map;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.logging.Handler;
import java.util.logging.LogRecord;
import java.util.logging.Logger;

final class ZLinkStreamCleanupDiagnosticsTest {
    @ParameterizedTest
    @CsvSource({
        "wait,false",
        "wait,true",
        "sequence,false",
        "sequence,true",
        "none,false",
        "none,true"
    })
    void publicTerminalSurvivesThrowingSubscriptionCleanup(String kind, boolean fails)
            throws Exception {
        RuntimeException cleanupFailure = new IllegalStateException("subscription cleanup failed");
        ZLinkStreamException operationFailure =
                ZLinkStreamException.validationFailed("predicate failed");
        var records = new CopyOnWriteArrayList<LogRecord>();
        var diagnostic = new java.util.concurrent.CompletableFuture<Throwable>();
        Handler handler =
                new Handler() {
                    @Override
                    public void publish(LogRecord record) {
                        records.add(record);
                        diagnostic.complete(record.getThrown());
                    }

                    @Override
                    public void flush() {}

                    @Override
                    public void close() {}
                };
        Logger logger = Logger.getLogger(ZLinkStreamCleanup.class.getName());
        logger.addHandler(handler);
        AtomicReference<ZLinkStreamMessageHandler<ZLinkStreamEncodedPayload>> receiver =
                new AtomicReference<>();
        ZLinkStreamConnector connector =
                (ZLinkStreamConnector)
                        Proxy.newProxyInstance(
                                ZLinkStreamConnector.class.getClassLoader(),
                                new Class<?>[] {ZLinkStreamConnector.class},
                                (proxy, method, arguments) -> {
                                    if (!method.getName().equals("on"))
                                        throw new UnsupportedOperationException(method.getName());
                                    @SuppressWarnings("unchecked")
                                    var callback =
                                            (ZLinkStreamMessageHandler<ZLinkStreamEncodedPayload>)
                                                    arguments[1];
                                    receiver.set(callback);
                                    return (AutoCloseable)
                                            () -> {
                                                throw cleanupFailure;
                                            };
                                });
        try {
            CompletionStage<?> terminal =
                    switch (kind) {
                        case "wait" ->
                                new DefaultZLinkStreamWaitCall(
                                                connector, "Push", Duration.ofSeconds(3), null)
                                        .where(
                                                message -> {
                                                    if (fails) throw operationFailure;
                                                    return true;
                                                })
                                        .submit();
                        case "sequence" ->
                                new DefaultZLinkStreamSequenceCall(
                                                connector, "Push", Duration.ofSeconds(3), null)
                                        .expect(
                                                message -> {
                                                    if (fails) throw operationFailure;
                                                    return true;
                                                })
                                        .submit();
                        case "none" ->
                                new DefaultZLinkStreamExpectNoneCall(connector, "Push")
                                        .within(fails ? Duration.ofSeconds(3) : Duration.ZERO)
                                        .submit();
                        default -> throw new AssertionError(kind);
                    };
            var message =
                    new ZLinkStreamMessage<>(
                            "Push",
                            new ZLinkStreamEncodedPayload(
                                    "Push", Message.from(new byte[] {1}), Map.of()),
                            Map.<String, String>of());
            if (!kind.equals("none") || fails) {
                receiver.get().handleAsync(message).toCompletableFuture().get(3, TimeUnit.SECONDS);
            }
            if (fails) {
                Throwable resultFailure =
                        assertThrows(
                                        CompletionException.class,
                                        () -> terminal.toCompletableFuture().join())
                                .getCause();
                if (kind.equals("none")) {
                    assertEquals(
                            ZLinkStreamErrorCode.VALIDATION_FAILED,
                            ((ZLinkStreamException) resultFailure).errorCode());
                } else {
                    assertSame(operationFailure, resultFailure);
                }
            } else {
                Object result = terminal.toCompletableFuture().get(3, TimeUnit.SECONDS);
                if (kind.equals("wait")) assertSame(message, result);
                else if (kind.equals("sequence")) assertEquals(java.util.List.of(message), result);
                else assertEquals(null, result);
                message.payload().payload().close();
            }
            assertSame(cleanupFailure, diagnostic.get(3, TimeUnit.SECONDS));
            assertEquals(1, records.size());
            assertSame(cleanupFailure, records.getFirst().getThrown());
        } finally {
            logger.removeHandler(handler);
        }
    }
}
