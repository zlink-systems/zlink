package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.assertFalse;

import org.junit.jupiter.api.Test;

import java.util.concurrent.atomic.AtomicBoolean;

final class DefaultZLinkStreamConnectorTraceTest {
    @Test
    void disabledTraceDoesNotRegisterCompletionCallback() throws Exception {
        var connector =
                new DefaultZLinkStreamConnector(
                        ZLinkStreamConnectorOptions.createDefault(
                                java.net.URI.create("tcp://127.0.0.1:1")));
        var method =
                DefaultZLinkStreamConnector.class.getDeclaredMethod(
                        "traceWrite",
                        ZLinkStreamWireProtocol.Header.class,
                        java.util.concurrent.CompletionStage.class);
        method.setAccessible(true);
        var callbacks = new java.util.concurrent.atomic.AtomicInteger();
        var publication =
                new java.util.concurrent.CompletableFuture<Void>() {
                    @Override
                    public java.util.concurrent.CompletableFuture<Void> whenComplete(
                            java.util.function.BiConsumer<? super Void, ? super Throwable> action) {
                        callbacks.incrementAndGet();
                        return super.whenComplete(action);
                    }
                };
        org.junit.jupiter.api.Assertions.assertSame(
                publication, method.invoke(connector, null, publication));
        org.junit.jupiter.api.Assertions.assertEquals(0, callbacks.get());
    }

    @Test
    void disabledTraceDoesNotBuildMessage() {
        AtomicBoolean messageBuilt = new AtomicBoolean();

        DefaultZLinkStreamConnector.trace(() -> buildTraceMessage(messageBuilt));

        assertFalse(messageBuilt.get());
    }

    private static String buildTraceMessage(AtomicBoolean messageBuilt) {
        messageBuilt.set(true);
        return "test trace";
    }
}
