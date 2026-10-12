package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertSame;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.channels.ZLinkRouteMessageContext;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

final class ZLinkHandlerPreparationTest {
    @Test
    void declaredPrimitivePayloadBindsItsBoxedInvocationValue() {
        var registration =
                new ChannelRequestHandlerRegistration(
                        PrimitiveHandler.class, int.class, int.class, "number");
        assertEquals(
                7,
                registration
                        .invocation()
                        .invoke(new PrimitiveHandler(), 7, null)
                        .toCompletableFuture()
                        .join());
    }

    public static final class PrimitiveHandler {
        public int handle(int message, systems.zlink.framework.ZLinkMessageContext context) {
            return message;
        }
    }

    @Test
    void annotatedMethodReceivesOnlyItsDeclaredArguments() throws Exception {
        var registration =
                new ChannelRequestHandlerRegistration(
                        SingleParameterHandler.class,
                        SingleParameterHandler.class.getMethod("handle", String.class),
                        String.class,
                        String.class,
                        "message");
        assertEquals(
                "message",
                registration
                        .invocation()
                        .invoke(new SingleParameterHandler(), "message", null)
                        .toCompletableFuture()
                        .join());
    }

    public static final class SingleParameterHandler {
        public String handle(String message) {
            return message;
        }
    }

    @Test
    void interfaceMethodIsSelectedBeforeDispatchAndRetainedAcrossPacketNaming() {
        var registration =
                new ChannelRouteSendHandlerRegistration(Handler.class, String.class, null);
        assertNotNull(registration.handlerMethod());
        assertSame(
                registration.handlerMethod(),
                registration.withPacketName("message").handlerMethod());
    }

    public static final class Handler {
        public CompletionStage<Void> handle(String message, ZLinkRouteMessageContext context) {
            return CompletableFuture.completedFuture(null);
        }
    }
}
