package systems.zlink.framework.runtime.streams;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import com.fasterxml.jackson.databind.JsonNode;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.Arguments;
import org.junit.jupiter.params.provider.MethodSource;

import systems.zlink.framework.errors.ZLinkConfigurationException;

import java.util.Base64;
import java.util.stream.Stream;

final class ZLinkAdmissionTicketConformanceTest {
    static Stream<Arguments> ticketVectors() throws Exception {
        return AdmissionQueueFixture.scenarios("ticketVectors", 12);
    }

    @ParameterizedTest(name = "{0}")
    @MethodSource("ticketVectors")
    void conformsToSharedTicketVector(String name, JsonNode vector) {
        var given = vector.required("given");
        var expect = vector.required("expect");
        String key = given.required("keyBase64Url").asText();
        String previousKey = given.required("previousKeyBase64Url").asText(null);
        if (expect.required("reason").asText().equals("keyTooShort")) {
            assertThrows(
                    ZLinkConfigurationException.class,
                    () -> new ZLinkAdmissionTickets(key, previousKey));
            return;
        }

        var tickets = new ZLinkAdmissionTickets(key, previousKey);
        var decoder = Base64.getUrlDecoder();
        long expiresAt = given.required("expiresAtUnixMs").longValue();
        long epoch = given.required("epoch").longValue();
        var issued =
                new ZLinkAdmissionTickets.TicketConnection(
                        decoder.decode(given.required("nodeRidBase64Url").asText()),
                        given.required("nodeLifecycleGeneration").longValue(),
                        given.required("streamName").asText(),
                        decoder.decode(given.required("sessionRidBase64Url").asText()));
        var verified =
                new ZLinkAdmissionTickets.TicketConnection(
                        decoder.decode(given.required("verificationNodeRidBase64Url").asText()),
                        given.required("verificationNodeLifecycleGeneration").longValue(),
                        given.required("verificationStreamName").asText(),
                        decoder.decode(given.required("verificationSessionRidBase64Url").asText()));
        String ticket = vector.required("ticket").asText();
        if (name.equals("current-key") || name.equals("previous-key")) {
            var signer =
                    previousKey == null ? tickets : new ZLinkAdmissionTickets(previousKey, null);
            assertEquals(ticket, signer.issue(issued, expiresAt, epoch));
        }
        if (name.equals("previous-key")) {
            assertEquals(
                    new ZLinkAdmissionTickets(key, null).issue(issued, expiresAt, epoch),
                    tickets.issue(issued, expiresAt, epoch));
        }

        assertEquals(
                expect.required("accept").asBoolean(),
                tickets.verifies(
                        ticket,
                        verified,
                        given.required("verificationEpoch").longValue(),
                        given.required("nowUnixMs").longValue()));
    }
}
