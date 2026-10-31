package systems.zlink.framework.runtime.handlers;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotSame;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

class ZLinkHandlerMethodInvokerTest {
    @Test
    void completionStageHandlerResultIsIsolatedFromCancellation() throws Exception {
        var reply = new CompletableFuture<String>();
        var method = EchoHandler.class.getMethod("handle");
        var prepared = ZLinkHandlerMethodInvoker.prepare(method, List.of());

        CompletionStage<Object> result = prepared.invoke(new EchoHandler(reply));

        assertNotSame(reply, result);
        assertTrue(result.toCompletableFuture().cancel(false));
        assertFalse(reply.isDone());

        CompletionStage<Object> second = prepared.invoke(new EchoHandler(reply));
        reply.complete("reply");
        assertEquals("reply", second.toCompletableFuture().join());
    }

    @Test
    void completedPlainCompletionStageDoesNotNeedAnIsolationStage() throws Exception {
        var reply = CompletableFuture.completedFuture("reply");
        var method = EchoHandler.class.getMethod("handle");
        var prepared = ZLinkHandlerMethodInvoker.prepare(method, List.of());

        CompletionStage<Object> result = prepared.invoke(new EchoHandler(reply));

        assertSame(reply, result);
    }

    @Test
    void completedCompletableFutureSubclassDoesNotNeedAnIsolationStage() throws Exception {
        var reply = new CompletableFuture<String>() {};
        reply.complete("reply");
        var method = EchoHandler.class.getMethod("handle");
        var prepared = ZLinkHandlerMethodInvoker.prepare(method, List.of());

        CompletionStage<Object> result = prepared.invoke(new EchoHandler(reply));

        assertSame(reply, result);
    }

    public static final class EchoHandler {
        private final CompletionStage<String> reply;

        public EchoHandler(CompletionStage<String> reply) {
            this.reply = reply;
        }

        public CompletionStage<String> handle() {
            return reply;
        }
    }
}
