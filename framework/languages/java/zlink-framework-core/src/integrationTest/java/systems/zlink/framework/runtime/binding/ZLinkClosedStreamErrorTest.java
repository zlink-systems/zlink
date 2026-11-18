package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;
import systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorOrigin;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

import java.net.Socket;
import java.net.URI;
import java.util.EnumSet;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;

final class ZLinkClosedStreamErrorTest {
    @Test
    void disconnectedStreamOneWayFailureShowsKindAndBindingSubmitResult() throws Exception {
        var ready = new CompletableFuture<RoutingId>();
        var disconnected = new CompletableFuture<RoutingId>();
        try (var context = Zlink.createContext();
                var stream = new ZLinkJavaStreamSocket(context.createStreamSocket(), null);
                var payload = Message.from(new byte[] {1})) {
            stream.onTransportError(
                    (rid, event, nativeCode, message) -> {
                        if (event == MonitorEventType.CONNECTION_READY) {
                            ready.complete(rid);
                        } else if (event == MonitorEventType.DISCONNECTED) {
                            disconnected.complete(rid);
                        }
                    });
            stream.bind("tcp://127.0.0.1:0");
            var endpoint = URI.create(stream.lastEndpoint());
            RoutingId rid;
            try (var client = new Socket(endpoint.getHost(), endpoint.getPort())) {
                rid = ready.get(5, TimeUnit.SECONDS);
            }
            assertEquals(rid, disconnected.get(5, TimeUnit.SECONDS));
            var header =
                    new ZLinkStreamHeader(
                            ZLinkStreamMessageKind.SEND,
                            ZLinkStreamCodec.RAW,
                            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
                            Optional.empty(),
                            "push",
                            Map.of());
            var send =
                    ZLinkOneWayCalls.adaptOneWay(stream.sendAsync(rid, header, List.of(payload)))
                            .toCompletableFuture();
            var error =
                    assertInstanceOf(
                            ZLinkFrameworkException.class,
                            assertThrows(
                                            ExecutionException.class,
                                            () -> send.get(5, TimeUnit.SECONDS))
                                    .getCause());
            var submit = assertInstanceOf(ZlinkSubmitException.class, error.getCause());
            assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE, error.kind());
            assertTrue(error.toString().contains("[UNAVAILABLE]"), error.toString());
            assertTrue(error.toString().contains(submit.getResult().name()), error.toString());
            assertEquals(ZLinkFrameworkErrorOrigin.frameworkMetadata(), error.metadata());
        }
    }
}
