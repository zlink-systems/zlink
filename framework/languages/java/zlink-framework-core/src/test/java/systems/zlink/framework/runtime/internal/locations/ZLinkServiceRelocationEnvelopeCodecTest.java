package systems.zlink.framework.runtime.internal.locations;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.HexFormat;
import java.util.List;
import java.util.regex.Pattern;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.runtime.protocol.ServiceWirePilotCodec;

final class ZLinkServiceRelocationEnvelopeCodecTest {
    private static final Pattern LOGICAL_HEX = Pattern.compile(
        "\\\"logicalHex\\\"\\s*:\\s*\\\"([0-9a-f]+)\\\"");

    @Test
    void sharedGoldenRoundTripsByteIdentically() throws IOException {
        byte[] encoded = golden();
        var envelope = ZLinkServiceRelocationEnvelopeCodec.decode(encoded);
        var generated = ServiceWirePilotCodec.decodeRelocationEnvelopeV1(
            List.of(encoded));

        assertEquals(0, envelope.relocationHigh());
        assertEquals(9, envelope.relocationLow());
        assertEquals(1, envelope.applicationVersion());
        assertEquals(3, envelope.applicationStates().size());
        assertEquals(1, envelope.applicationStates().get(0).participantId());
        assertEquals(2, envelope.applicationStates().get(1).participantId());
        assertEquals(3, envelope.applicationStates().get(2).participantId());
        assertEquals(1, envelope.savedWork().size());
        assertEquals(2, envelope.savedWork().getFirst().participantId());
        assertEquals(1, envelope.savedWork().getFirst().sequence());
        assertEquals(1, envelope.timerRegistrations().size());
        assertEquals(3, envelope.timerRegistrations().getFirst().participantId());
        assertEquals(1, envelope.pendingTimerTicks().size());
        assertEquals(3, envelope.pendingTimerTicks().getFirst().participantId());
        assertEquals(2, envelope.pendingTimerTicks().getFirst().sequence());
        assertArrayEquals(encoded, envelope.canonicalBytes());
        assertArrayEquals(encoded,
            ServiceWirePilotCodec.encodeRelocationEnvelopeV1(generated));
    }

    @Test
    void handAndGeneratedDecodersRejectCrossSectionReferenceMutations()
        throws IOException {
        byte[] membershipMutation = golden();
        membershipMutation[132] = 4;
        assertRejectedByBoth(membershipMutation);

        byte[] timerNameMutation = golden();
        timerNameMutation[402] = 'x';
        assertRejectedByBoth(timerNameMutation);
    }

    @Test
    void trailingBytesAreRejected() throws IOException {
        byte[] encoded = golden();
        byte[] trailing = Arrays.copyOf(encoded, encoded.length + 1);
        assertThrows(IllegalArgumentException.class,
            () -> ZLinkServiceRelocationEnvelopeCodec.decode(trailing));
    }

    private static void assertRejectedByBoth(byte[] encoded) {
        assertThrows(IllegalArgumentException.class,
            () -> ZLinkServiceRelocationEnvelopeCodec.decode(encoded));
        assertThrows(IOException.class,
            () -> ServiceWirePilotCodec.decodeRelocationEnvelopeV1(
                List.of(encoded)));
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
}
