package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertSame;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.channels.ZLinkRouteMessageContext;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

final class ZLinkHandlerPreparationTest {
    @Test
    void configuredDeclarationDoesNotSelectAnInvocationBeforeCatalogConfiguration() {
        var declaration =
                new ChannelRouteSendHandlerRegistration(Handler.class, String.class, "message");
        assertNull(declaration.invocation());
    }

    @Test
    void declaredPrimitivePayloadBindsItsBoxedInvocationValue() {
        var registration =
                new ChannelRequestHandlerRegistration(
                                PrimitiveHandler.class, int.class, int.class, "number")
                        .prepare(java.util.List.of());
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
                                "message")
                        .prepare(java.util.List.of());
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
                new ChannelRouteSendHandlerRegistration(Handler.class, String.class, null)
                        .prepare(java.util.List.of());
        assertNotNull(registration.invocation().method());
        assertSame(
                registration.invocation().method(),
                registration.withPacketName("message").invocation().method());
    }

    public static final class Handler {
        public CompletionStage<Void> handle(String message, ZLinkRouteMessageContext context) {
            return CompletableFuture.completedFuture(null);
        }
    }
}
