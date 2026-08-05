package systems.zlink.framework.runtime.streams;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.EnumSet;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

final class ZLinkFlowHeaderContractTest {
    @Test
    void markerAndFlowPairRoundTrip() {
        ZLinkFlowContext.State flow = ZLinkFlowContext.create(ZLinkFlowOrigin.APPLICATION);
        ZLinkStreamHeader header = new ZLinkStreamHeader(
            ZLinkStreamMessageKind.SEND, ZLinkStreamCodec.JSON,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class), Optional.empty(),
            "Move", Map.of(), Optional.of("corr-1"), Optional.of(flow.flowId()),
            Optional.of(flow.origin()));

        byte[] encoded = ZLinkStreamHeaderCodec.encode(header);
        assertEquals(0xF2, Byte.toUnsignedInt(encoded[0]));
        assertEquals(header, ZLinkStreamHeaderCodec.decodeOrPlain(encoded));
        assertTrue(flow.flowId().matches(
            "[0-9a-f]{8}-[0-9a-f]{4}-7[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}"));
    }

    @Test
    void markerAndOptionalPairAreMandatory() {
        assertThrows(IllegalArgumentException.class,
            () -> ZLinkStreamHeaderCodec.decodeOrPlain(new byte[] {1, 0, 0, 1, 'x'}));
        assertThrows(IllegalArgumentException.class, () -> new ZLinkStreamHeader(
            ZLinkStreamMessageKind.SEND, ZLinkStreamCodec.JSON,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class), Optional.empty(),
            "Move", Map.of(), Optional.empty(), Optional.of(
                "018f2f1d-5d52-7b70-8f08-13fecf6f6abc"), Optional.empty()));
    }

    @Test
    void scopeRestoresPreviousFlow() {
        ZLinkFlowContext.State outer = ZLinkFlowContext.create(ZLinkFlowOrigin.INBOUND);
        ZLinkFlowContext.State inner = ZLinkFlowContext.create(ZLinkFlowOrigin.TIMER);
        try (ZLinkFlowContext.Scope ignored = ZLinkFlowContext.enter(outer)) {
            try (ZLinkFlowContext.Scope nested = ZLinkFlowContext.enter(inner)) {
                assertEquals(inner, ZLinkFlowContext.current());
            }
            assertEquals(outer, ZLinkFlowContext.current());
        }
        assertEquals(null, ZLinkFlowContext.current());
    }

    @Test
    void propagatedStageCompletesDependentsInsideCapturedFlowOnly() {
        ZLinkFlowContext.State first = ZLinkFlowContext.create(ZLinkFlowOrigin.INBOUND);
        CompletableFuture<String> source = new CompletableFuture<>();
        java.util.concurrent.CompletionStage<String> propagated;
        try (ZLinkFlowContext.Scope ignored = ZLinkFlowContext.enter(first)) {
            propagated = ZLinkFlowContext.propagate(source);
        }
        var observed = propagated.thenApply(value -> ZLinkFlowContext.current());
        source.complete("ok");
        assertEquals(first, observed.toCompletableFuture().join());
        assertEquals(null, ZLinkFlowContext.current());

        CompletableFuture<String> noFlow = new CompletableFuture<>();
        var noFlowObserved = ZLinkFlowContext.propagate(noFlow)
            .thenApply(value -> ZLinkFlowContext.current());
        noFlow.complete("ok");
        assertEquals(null, noFlowObserved.toCompletableFuture().join());

        CompletableFuture<String> failed = new CompletableFuture<>();
        try (ZLinkFlowContext.Scope ignored = ZLinkFlowContext.enter(first)) {
            var propagatedFailure = ZLinkFlowContext.propagate(failed);
            failed.completeExceptionally(new IllegalStateException("expected"));
            assertThrows(java.util.concurrent.CompletionException.class,
                () -> propagatedFailure.toCompletableFuture().join());
        }
    }
}
