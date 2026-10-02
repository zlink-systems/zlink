package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

/**
 * Pins the relocation-forward relay failure classification (spec 32-framework-error-model:83-92 +
 * the schema terminal-failure-integrity rule): the received (terminal, failureCode) pair relays
 * unchanged, and every synthesized pair is schema-valid.
 */
final class ZLinkJavaRawMeshNodeRelayFailurePairTest {
    @Test
    void everyCppIncomingAliasRetainsItsGeneralMeaning() {
        int[] codes = {4, 8, 9, 12, 18, 20, 21, 33, 34};
        ZLinkFrameworkErrorKind[] kinds = {
            ZLinkFrameworkErrorKind.TYPE_MISMATCH,
            ZLinkFrameworkErrorKind.INVALID_OPERATION,
            ZLinkFrameworkErrorKind.NOT_FOUND,
            ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
            ZLinkFrameworkErrorKind.UNAVAILABLE,
            ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
            ZLinkFrameworkErrorKind.UNAVAILABLE,
            ZLinkFrameworkErrorKind.INVALID_OPERATION,
            ZLinkFrameworkErrorKind.UNAVAILABLE
        };
        for (int i = 0; i < codes.length; i++) {
            assertEquals(
                    kinds[i],
                    ZLinkBackendRequestResult.INTERNAL_ERROR.toFrameworkErrorKind(codes[i]),
                    "cpp alias " + codes[i]);
        }
    }

    @Test
    void everyCppRepresentativeRoundTripsIncludingCoarseNoneTerminals() {
        ZLinkFrameworkErrorKind[] kinds = {
            ZLinkFrameworkErrorKind.NOT_FOUND, ZLinkFrameworkErrorKind.ALREADY_EXISTS,
            ZLinkFrameworkErrorKind.TYPE_MISMATCH, ZLinkFrameworkErrorKind.REJECTED,
            ZLinkFrameworkErrorKind.UNAVAILABLE, ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
            ZLinkFrameworkErrorKind.SHUTTING_DOWN, ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
            ZLinkFrameworkErrorKind.INVALID_OPERATION, ZLinkFrameworkErrorKind.DATA_LOST,
            ZLinkFrameworkErrorKind.INTERNAL_FAILURE
        };
        int[][] cppPairs = {
            {102, 14}, {107, 3}, {107, 7}, {106, 15}, {105, 13}, {105, 19}, {103, 0}, {104, 16},
            {111, 0}, {105, 35}, {105, 17}
        };
        for (int i = 0; i < kinds.length; i++) {
            int[] actual =
                    ZLinkJavaRawMeshNode.relayedFailurePair(
                            new ZLinkFrameworkException(kinds[i], "cpp representative"));
            assertArrayEquals(cppPairs[i], actual, kinds[i].name());
            assertTrue(ServiceWireConstants.validTerminalFailure(actual[0], actual[1]));
            assertEquals(
                    kinds[i],
                    ZLinkBackendRequestResult.fromWireTerminal(actual[0])
                            .toFrameworkErrorKind(actual[1]),
                    kinds[i].name());
        }
        assertEquals(ZLinkFrameworkErrorKind.values().length - 1, kinds.length);
        assertArrayEquals(
                new int[] {105, 17},
                ZLinkJavaRawMeshNode.relayedFailurePair(
                        new ZLinkFrameworkException(
                                ZLinkFrameworkErrorKind.NOT_CONFIGURED, "fallback")));
    }

    @Test
    void unavailableActorJoinPreservesPublicErrorKind() {
        int[] pair =
                ZLinkJavaRawMeshNode.canonicalActorJoinFailurePair(
                        new ZLinkFrameworkException(
                                ZLinkFrameworkErrorKind.UNAVAILABLE, "unavailable"));
        assertArrayEquals(
                new int[] {105, (int) ServiceWireConstants.FRAMEWORK_ERROR_ROUTE_NOT_CONNECTED},
                pair);
        assertTrue(ServiceWireConstants.validTerminalFailure(pair[0], pair[1]));
        assertEquals(
                ZLinkFrameworkErrorKind.UNAVAILABLE,
                ZLinkBackendRequestResult.INTERNAL_ERROR.toFrameworkErrorKind(pair[1]));
    }

    @Test
    void relayedReplyTerminalPairIsPreservedUnchanged() {
        assertArrayEquals(
                new int[] {106, 18},
                ZLinkJavaRawMeshNode.relayedFailurePair(
                        new ZLinkJavaRawMeshNode.ZLinkRelayedReplyTerminalException(106, 18)));
        assertArrayEquals(
                new int[] {107, 33},
                ZLinkJavaRawMeshNode.relayedFailurePair(
                        new ZLinkJavaRawMeshNode.ZLinkRelayedReplyTerminalException(107, 33)));
    }

    @Test
    void transportAndMalformedFailuresRelaySchemaValidPairs() {
        //  A boundary transport terminal relays its value with none.
        assertArrayEquals(
                new int[] {101, 0},
                ZLinkJavaRawMeshNode.relayedFailurePair(
                        new ZlinkRequestException(RequestResult.TIMED_OUT)));
        //  A typed transport terminal cannot carry none, so it falls back to
        //  the generic internalError+requestFailed pair.
        assertArrayEquals(
                new int[] {105, 17},
                ZLinkJavaRawMeshNode.relayedFailurePair(
                        new ZlinkRequestException(RequestResult.NOT_FOUND)));
        //  A malformed forwarded reply relays
        //  protocolError+requestProtocolError (a bare 104+0 would itself
        //  violate the schema integrity rule).
        assertArrayEquals(
                new int[] {104, 16},
                ZLinkJavaRawMeshNode.relayedFailurePair(
                        new IllegalArgumentException("malformed forwarded reply")));
        //  Anything else is an unexpressible Framework failure
        //  (spec 32:119-120).
        assertArrayEquals(
                new int[] {105, 17},
                ZLinkJavaRawMeshNode.relayedFailurePair(
                        new IllegalStateException("unexpected relay failure")));
    }

    @Test
    void actorDispatchUnavailableAndRejectionKeepTheirFrameworkKinds() {
        assertArrayEquals(
                new int[] {105, 13},
                ZLinkJavaRawMeshNode.relayedFailurePair(
                        new ZLinkFrameworkException(
                                ZLinkFrameworkErrorKind.UNAVAILABLE,
                                "actor placement unavailable")));
        assertArrayEquals(
                new int[] {106, 15},
                ZLinkJavaRawMeshNode.relayedFailurePair(
                        new ZLinkFrameworkException(
                                ZLinkFrameworkErrorKind.REJECTED,
                                "actor dispatch admission is closed")));
    }

    @Test
    void canonicalActorJoinKeepsStoreFailureTerminalKinds() {
        assertArrayEquals(
                new int[] {102, 14},
                ZLinkJavaRawMeshNode.canonicalActorJoinFailurePair(
                        new ZLinkFrameworkException(ZLinkFrameworkErrorKind.NOT_FOUND, "missing")));
        assertArrayEquals(
                new int[] {104, 16},
                ZLinkJavaRawMeshNode.canonicalActorJoinFailurePair(
                        new ZLinkFrameworkException(
                                ZLinkFrameworkErrorKind.PROTOCOL_ERROR, "fence")));
        assertArrayEquals(
                new int[] {107, 4},
                ZLinkJavaRawMeshNode.canonicalActorJoinFailurePair(
                        new ZLinkFrameworkException(
                                ZLinkFrameworkErrorKind.TYPE_MISMATCH, "type")));
        assertArrayEquals(
                new int[] {106, 15},
                ZLinkJavaRawMeshNode.canonicalActorJoinFailurePair(
                        new ZLinkFrameworkException(ZLinkFrameworkErrorKind.REJECTED, "rejected")));
    }
}
