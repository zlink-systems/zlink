package systems.zlink.framework.runtime.handlers;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertSame;

import org.junit.jupiter.api.Test;

import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

class ZLinkHandlerMethodInvokerTest {
    @Test
    void completionStageHandlerResultDoesNotCreateADependentStage() throws Exception {
        var reply = new CompletableFuture<String>();
        var method = EchoHandler.class.getMethod("handle");
        var prepared = ZLinkHandlerMethodInvoker.prepare(method, List.of());

        CompletionStage<Object> result = prepared.invoke(new EchoHandler(reply));

        assertSame(reply, result);
        assertEquals(0, reply.getNumberOfDependents());
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
