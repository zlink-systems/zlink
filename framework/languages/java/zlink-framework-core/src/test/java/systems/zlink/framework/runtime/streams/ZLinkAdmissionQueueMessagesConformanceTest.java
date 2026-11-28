package systems.zlink.framework.runtime.streams;

import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.Arguments;
import org.junit.jupiter.params.provider.MethodSource;

import systems.zlink.framework.runtime.internal.json.ZLinkFrameworkJsonProfile;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.AdmitPermit;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.AdmitPermitReply;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.Enqueue;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.EnqueueReply;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.NodeReport;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.NodeReportReply;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.ReleasePermit;
import systems.zlink.framework.testsupport.ZLinkConformanceFixtures;

import java.util.Map;
import java.util.stream.Stream;
import java.util.stream.StreamSupport;

/** Runs admission-queue-messages-v1.json against the node-to-Spot message types (§3.5.8). */
final class ZLinkAdmissionQueueMessagesConformanceTest {
    private static final ObjectMapper MAPPER = ZLinkFrameworkJsonProfile.mapper();
    private static final Map<String, Class<?>> REQUESTS =
            Map.of(
                    "$zlink.admission-queue.enqueue", Enqueue.class,
                    "$zlink.admission-queue.report", NodeReport.class,
                    "$zlink.admission-queue.admit-permit", AdmitPermit.class,
                    "$zlink.admission-queue.release-permit", ReleasePermit.class);
    private static final Map<String, Class<?>> RESPONSES =
            Map.of(
                    "$zlink.admission-queue.enqueue", EnqueueReply.class,
                    "$zlink.admission-queue.report", NodeReportReply.class,
                    "$zlink.admission-queue.admit-permit", AdmitPermitReply.class);

    private static JsonNode fixture() throws Exception {
        var fixture = ZLinkConformanceFixtures.file("admission-queue-messages-v1.json");
        return MAPPER.readTree(fixture.toFile());
    }

    static Stream<Arguments> messages() throws Exception {
        var messages = fixture().required("messages");
        assertEquals(8, messages.size());
        return StreamSupport.stream(messages.spliterator(), false)
                .map(message -> Arguments.of(message.required("name").asText(), message));
    }

    static Stream<Arguments> invalid() throws Exception {
        var invalid = fixture().required("invalid");
        assertEquals(9, invalid.size());
        return StreamSupport.stream(invalid.spliterator(), false)
                .map(entry -> Arguments.of(entry.required("name").asText(), entry));
    }

    @Test
    void spotTypeMatchesTheFixture() throws Exception {
        assertEquals(
                fixture().required("spotType").asText(), ZLinkAdmissionQueueMessages.SPOT_TYPE);
    }

    @ParameterizedTest(name = "{0}")
    @MethodSource("messages")
    void validMessageEncodesAndDecodesToTheGoldenJson(String name, JsonNode message)
            throws Exception {
        String packet = message.required("packetName").asText();
        for (String side : new String[] {"request", "send"}) {
            if (message.has(side)) {
                roundTrip(REQUESTS.get(packet), message.get(side));
            }
        }
        if (message.has("replyPacketName")) {
            assertEquals(
                    message.get("replyPacketName").asText(),
                    systems.zlink.framework.runtime.messaging.ZLinkPacketNames.resolve(
                            RESPONSES.get(packet)));
        } else {
            assertEquals(false, RESPONSES.containsKey(packet));
        }
        if (message.has("response")) {
            roundTrip(RESPONSES.get(packet), message.get("response"));
        }
    }

    @ParameterizedTest(name = "{0}")
    @MethodSource("invalid")
    void invalidMessageIsAProtocolErrorOrIgnoredUnknownField(String name, JsonNode entry) {
        String packet = entry.required("packetName").asText();
        boolean response = entry.has("responsePayload");
        JsonNode payload = response ? entry.get("responsePayload") : entry.required("payload");
        Class<?> type = (response ? RESPONSES : REQUESTS).get(packet);
        if (entry.required("expect").asText().equals("accepted")) {
            assertDoesNotThrow(() -> MAPPER.treeToValue(payload, type));
        } else {
            assertThrows(Exception.class, () -> MAPPER.treeToValue(payload, type));
        }
    }

    private static void roundTrip(Class<?> type, JsonNode golden) throws Exception {
        Object decoded = MAPPER.treeToValue(golden, type);
        assertEquals(golden, MAPPER.valueToTree(decoded));
    }
}
