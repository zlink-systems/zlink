package systems.zlink.framework.runtime.internal.backend;

import static org.junit.jupiter.api.Assertions.*;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;

import java.nio.file.Files;
import java.nio.file.Path;

final class ZLinkRequestFailureMappingTest {
    private static JsonNode fixture() throws Exception {
        Path current = Path.of("").toAbsolutePath();
        while (current != null) {
            Path candidate =
                    current.resolve(
                            "framework/runtime/conformance/framework-error-mapping-v1.json");
            if (Files.isRegularFile(candidate))
                return new ObjectMapper().readTree(candidate.toFile());
            current = current.getParent();
        }
        throw new IllegalStateException("Framework error mapping fixture not found");
    }

    private static ZLinkFrameworkErrorKind kind(JsonNode row) {
        String value = row.get("kind").asText().replaceAll("([a-z])([A-Z])", "$1_$2");
        return ZLinkFrameworkErrorKind.valueOf(value.toUpperCase(java.util.Locale.ROOT));
    }

    @Test
    void allFailureCodesUseTheSpecifiedKind() throws Exception {
        JsonNode receive = fixture().get("receive");
        assertEquals(25, receive.size());
        for (JsonNode row : receive) {
            int code = row.get("failureCode").asInt();
            assertEquals(
                    kind(row), ZLinkRequestFailureMapping.incoming(code), row.get("code").asText());
            assertEquals(
                    kind(row),
                    ZLinkBackendRequestResult.fromWireTerminal(row.get("terminalResult").asInt())
                            .toFrameworkErrorKind(code));
        }
    }

    @Test
    void everyKindUsesItsSpecifiedRepresentative() throws Exception {
        JsonNode send = fixture().get("send");
        assertEquals(12, send.size());
        for (JsonNode row : send) {

            assertArrayEquals(
                    new int[] {row.get("terminalResult").asInt(), row.get("failureCode").asInt()},
                    ZLinkRequestFailureMapping.outgoing(kind(row)),
                    row.get("kind").asText());
            assertEquals(
                    row.path("codeOnlyFailureCode").asInt(row.get("failureCode").asInt()),
                    ZLinkRequestFailureMapping.outgoingCode(kind(row), 0),
                    row.get("kind").asText());
        }
    }

    @Test
    void matchingCauseCodesArePreservedAndOtherKindsUseTheirRepresentative() throws Exception {
        JsonNode fixture = fixture();
        for (JsonNode row : fixture.get("receive")) {
            int code = row.get("failureCode").asInt();
            assertArrayEquals(
                    new int[] {row.get("terminalResult").asInt(), code},
                    ZLinkRequestFailureMapping.outgoing(kind(row), code));
            assertEquals(code, ZLinkRequestFailureMapping.outgoingCode(kind(row), code));
            for (JsonNode send : fixture.get("send")) {
                if (kind(send) == kind(row)) continue;
                assertArrayEquals(
                        new int[] {
                            send.get("terminalResult").asInt(), send.get("failureCode").asInt()
                        },
                        ZLinkRequestFailureMapping.outgoing(kind(send), code));
                assertEquals(
                        send.path("codeOnlyFailureCode").asInt(send.get("failureCode").asInt()),
                        ZLinkRequestFailureMapping.outgoingCode(kind(send), code));
            }
            assertTrue(
                    systems.zlink.framework.runtime.protocol.ServiceWireConstants
                            .validTerminalFailure(row.get("terminalResult").asInt(), code));
            assertFalse(
                    systems.zlink.framework.runtime.protocol.ServiceWireConstants
                            .validTerminalFailure(0, code));
        }
        assertNull(ZLinkRequestFailureMapping.incoming(0));
        assertNull(ZLinkRequestFailureMapping.incoming(999));
    }

    @Test
    void receivedFailureRetainsItsWireCauseWhenRelayed() throws Exception {
        for (JsonNode row : fixture().get("receive")) {
            int code = row.get("failureCode").asInt();
            var failure =
                    ZLinkRequestFailureMapping.receivedFailure(
                            kind(row), "remote", code, java.util.Map.of());
            assertEquals(code, ZLinkRequestFailureMapping.causeCode(failure));
            assertArrayEquals(
                    new int[] {row.get("terminalResult").asInt(), code},
                    ZLinkRequestFailureMapping.outgoing(
                            failure.kind(), ZLinkRequestFailureMapping.causeCode(failure)));
        }
    }
}
