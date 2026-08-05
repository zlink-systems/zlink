package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertSame;

import java.lang.reflect.Method;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.spots.ZLinkSpotContext;

final class ZLinkSpotRuntimeActorArgumentsTest {
    @Test
    void bindsSpotActorRequestArguments() throws Exception {
        Method method = DotnetShapeHandler.class.getMethod(
            "request",
            TestSpot.class,
            TestActor.class,
            ZLinkMessageContext.class,
            Request.class);
        TestSpot spot = new TestSpot();
        TestActor actor = new TestActor();
        Request request = new Request();
        ZLinkMessageContext context = requestContext("PlaceMarkReq");

        Object[] args = ZLinkSpotHandlerInvoker.actorPacketArguments(
            method,
            spot,
            actor,
            context,
            request);

        assertSame(spot, args[0]);
        assertSame(actor, args[1]);
        assertSame(context, args[2]);
        assertSame(request, args[3]);
        assertEquals(4, args.length);
    }

    private static ZLinkMessageContext requestContext(String packetName) {
        return new ZLinkMessageContext() {
            @Override
            public Optional<String> meshName() {
                return Optional.empty();
            }

            @Override
            public Optional<String> channelName() {
                return Optional.empty();
            }

            @Override
            public String packetName() {
                return packetName;
            }

            @Override
            public Optional<String> contentType() {
                return Optional.empty();
            }

            @Override
            public java.util.Map<String, String> metadata() {
                return java.util.Map.of();
            }

            @Override
            public Optional<String> correlationId() {
                return Optional.empty();
            }
        };
    }

    public static final class DotnetShapeHandler {
        public CompletionStage<Reply> request(
            TestSpot spot,
            TestActor actor,
            ZLinkMessageContext context,
            Request request) {
            return CompletableFuture.completedFuture(new Reply());
        }
    }

    public static final class TestSpot implements ZLinkSpot<ZLinkActor> {
        @Override
        public ZLinkSpotContext context() {
            throw new UnsupportedOperationException();
        }

        @Override public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
        @Override public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class TestActor implements ZLinkActor {
        @Override
        public ZLinkActorContext context() {
            throw new UnsupportedOperationException();
        }
    }

    public record Request() {
    }

    public record Reply() {
    }
}
