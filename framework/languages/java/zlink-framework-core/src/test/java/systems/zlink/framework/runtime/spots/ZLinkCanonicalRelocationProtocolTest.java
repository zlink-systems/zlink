package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.HexFormat;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

final class ZLinkCanonicalRelocationProtocolTest {
    @Test
    void semanticProjectionPreservesEveryCanonicalGoldenRecord()
        throws Exception {
        JsonNode fixture = new ObjectMapper().readTree(
            Files.readString(fixture()));
        for (JsonNode item : fixture.path("canonical")) {
            byte[] encoded = HexFormat.of().parseHex(
                item.path("hex").asText());
            byte[] projected = switch (item.path("command").asInt()) {
                case ServiceWireConstants.COMMAND_RELOCATION_READY ->
                    ZLinkCanonicalRelocationProtocol.encodeReady(
                        ZLinkCanonicalRelocationProtocol.decodeReady(encoded));
                case ServiceWireConstants.COMMAND_RELOCATION_DATA ->
                    ZLinkCanonicalRelocationProtocol.encodeControlData(
                        ZLinkCanonicalRelocationProtocol.decodeControlData(
                            encoded));
                case ServiceWireConstants.COMMAND_RELOCATION_ACK ->
                    ZLinkCanonicalRelocationProtocol.encodeAck(
                        ZLinkCanonicalRelocationProtocol.decodeAck(encoded));
                case ServiceWireConstants.COMMAND_RELOCATION_SEAL ->
                    ZLinkCanonicalRelocationProtocol.encodeSeal(
                        ZLinkCanonicalRelocationProtocol.decodeSeal(encoded));
                case ServiceWireConstants.COMMAND_RELOCATION_COMPLETE ->
                    ZLinkCanonicalRelocationProtocol.encodeComplete(
                        ZLinkCanonicalRelocationProtocol.decodeComplete(
                            encoded));
                case ServiceWireConstants.COMMAND_RELOCATION_PREPARE ->
                    ZLinkCanonicalRelocationProtocol.encodePrepare(
                        ZLinkCanonicalRelocationProtocol.decodePrepare(
                            encoded));
                case ServiceWireConstants.COMMAND_RELOCATION_RESERVED ->
                    ZLinkCanonicalRelocationProtocol.encodeReserved(
                        ZLinkCanonicalRelocationProtocol.decodeReserved(
                            encoded));
                default -> throw new AssertionError("unexpected command");
            };
            assertArrayEquals(
                encoded,
                projected,
                item.path("name").asText());
        }
    }

    private static Path fixture() {
        Path current = Path.of(System.getProperty("user.dir")).toAbsolutePath();
        while (current != null) {
            Path candidate = current.resolve(
                "runtime/protocol/golden/relocation-control-v1.json");
            if (Files.isRegularFile(candidate)) {
                return candidate;
            }
            current = current.getParent();
        }
        throw new IllegalStateException(
            "shared relocation fixture was not found");
    }
}
