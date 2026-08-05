package systems.zlink.framework.runtime.internal.locations;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.HexFormat;
import java.util.regex.Pattern;
import org.junit.jupiter.api.Test;

final class ZLinkServiceRelocationEnvelopeCodecTest {
    private static final Pattern LOGICAL_HEX = Pattern.compile(
        "\\\"logicalHex\\\"\\s*:\\s*\\\"([0-9a-f]+)\\\"");

    @Test
    void sharedGoldenRoundTripsByteIdentically() throws IOException {
        byte[] encoded = golden();
        var envelope = ZLinkServiceRelocationEnvelopeCodec.decode(encoded);

        assertEquals(0, envelope.relocationHigh());
        assertEquals(9, envelope.relocationLow());
        assertEquals(1, envelope.applicationVersion());
        assertEquals(2, envelope.participantProgress().size());
        assertEquals(2, envelope.participantProgress().getFirst()
            .acceptedBoundary());
        assertEquals(0, envelope.participantProgress().getFirst()
            .replayCursor());
        assertEquals(1, envelope.terminalCompletions().size());
        assertEquals(1, envelope.terminalCompletions().getFirst()
            .deliveryState());
        assertFalse(envelope.recoveryReleaseEligible(),
            "terminal receipt alone does not release recovery data");
        assertArrayEquals(encoded,
            ZLinkServiceRelocationEnvelopeCodec.encodeSuccessor(
                envelope,
                envelope.participantProgress(),
                envelope.terminalCompletions()));
    }

    @Test
    void recoveryReleaseRequiresRelayAckOrSourceLeaseExpiry()
        throws IOException {
        var envelope = ZLinkServiceRelocationEnvelopeCodec.decode(golden());

        for (int evidence : new int[] {2, 3}) {
            var completions = envelope.terminalCompletions().stream()
                .map(value -> new ZLinkServiceRelocationEnvelopeCodec.Completion(
                    value.operationHigh(),
                    value.operationLow(),
                    value.sourceOwnerId(),
                    value.sourceOwnerLeaseGeneration(),
                    value.sourceNodeRid(),
                    value.sourceNodeGeneration(),
                    value.participantId(),
                    value.sequence(),
                    value.terminalResult(),
                    value.failureCode(),
                    evidence,
                    value.payload()))
                .toList();
            var successor = ZLinkServiceRelocationEnvelopeCodec.decode(
                ZLinkServiceRelocationEnvelopeCodec.encodeSuccessor(
                    envelope,
                    envelope.participantProgress(),
                    completions));

            assertTrue(successor.recoveryReleaseEligible());
        }
    }

    @Test
    void successorDurablyAdvancesCursorAndRelayState() throws IOException {
        var envelope = ZLinkServiceRelocationEnvelopeCodec.decode(golden());
        var progress = envelope.participantProgress().stream()
            .map(value -> value.participantId() == 1
                ? new ZLinkServiceRelocationEnvelopeCodec.Progress(
                    value.participantId(), value.acceptedBoundary(), 1)
                : value)
            .toList();
        var completions = envelope.terminalCompletions().stream()
            .map(value -> new ZLinkServiceRelocationEnvelopeCodec.Completion(
                value.operationHigh(),
                value.operationLow(),
                value.sourceOwnerId(),
                value.sourceOwnerLeaseGeneration(),
                value.sourceNodeRid(),
                value.sourceNodeGeneration(),
                value.participantId(),
                value.sequence(),
                value.terminalResult(),
                value.failureCode(),
                0,
                value.payload()))
            .toList();

        byte[] successor = ZLinkServiceRelocationEnvelopeCodec.encodeSuccessor(
            envelope, progress, completions);
        var decoded = ZLinkServiceRelocationEnvelopeCodec.decode(successor);

        assertNotEquals(HexFormat.of().formatHex(golden()),
            HexFormat.of().formatHex(successor));
        assertEquals(1, decoded.participantProgress().getFirst()
            .replayCursor());
        assertEquals(1, decoded.pendingRelayCount());
        assertEquals(0, decoded.terminalCompletions().getFirst()
            .deliveryState());
    }

    @Test
    void malformedProgressAndTrailingBytesAreRejected() throws IOException {
        byte[] encoded = golden();
        byte[] trailing = Arrays.copyOf(encoded, encoded.length + 1);
        assertThrows(IllegalArgumentException.class,
            () -> ZLinkServiceRelocationEnvelopeCodec.decode(trailing));

        byte[] replayOverflow = encoded.clone();
        byte[] progressPrefix = HexFormat.of().parseHex(
            "00000002000000000000000100000000000000020000000000000000");
        int offset = indexOf(replayOverflow, progressPrefix);
        replayOverflow[offset + progressPrefix.length - 1] = 3;
        assertThrows(IllegalArgumentException.class,
            () -> ZLinkServiceRelocationEnvelopeCodec.decode(replayOverflow));
    }

    private static byte[] golden() throws IOException {
        String json = Files.readString(sharedFixturePath());
        var match = LOGICAL_HEX.matcher(json);
        if (!match.find()) {
            throw new IllegalStateException("shared relocation logicalHex is missing");
        }
        return HexFormat.of().parseHex(match.group(1));
    }

    private static Path sharedFixturePath() {
        Path current = Path.of(System.getProperty("user.dir")).toAbsolutePath();
        while (current != null) {
            Path candidate = current.resolve(
                "runtime/protocol/golden/relocation-envelope-v1.json");
            if (Files.isRegularFile(candidate)) {
                return candidate;
            }
            current = current.getParent();
        }
        throw new IllegalStateException(
            "shared relocation envelope fixture was not found");
    }

    private static int indexOf(byte[] source, byte[] value) {
        for (int index = 0; index <= source.length - value.length; index++) {
            if (Arrays.equals(
                Arrays.copyOfRange(source, index, index + value.length),
                value)) {
                return index;
            }
        }
        throw new IllegalStateException("fixture progress prefix was not found");
    }
}
