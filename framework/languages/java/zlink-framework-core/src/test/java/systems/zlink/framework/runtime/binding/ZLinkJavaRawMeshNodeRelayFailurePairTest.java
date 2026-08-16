package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;

import org.junit.jupiter.api.Test;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.sockets.RequestResult;

/**
 * Pins the relocation-forward relay failure classification
 * (spec 32-framework-error-model:83-92 + the schema
 * terminal-failure-integrity rule): the received (terminal, failureCode)
 * pair relays unchanged, and every synthesized pair is schema-valid.
 */
final class ZLinkJavaRawMeshNodeRelayFailurePairTest {

    @Test
    void relayedReplyTerminalPairIsPreservedUnchanged() {
        assertArrayEquals(
            new int[] {106, 18},
            ZLinkJavaRawMeshNode.relayedFailurePair(
                new ZLinkJavaRawMeshNode
                    .ZLinkRelayedReplyTerminalException(106, 18)));
        assertArrayEquals(
            new int[] {107, 33},
            ZLinkJavaRawMeshNode.relayedFailurePair(
                new ZLinkJavaRawMeshNode
                    .ZLinkRelayedReplyTerminalException(107, 33)));
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
}
