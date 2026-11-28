package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope;
import systems.zlink.framework.runtime.messaging.ZLinkStringMessageSerializer;

import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Map;
import java.util.Optional;

final class ZLinkAdmissionQueueSpotReplyEnvelopeTest {
    private static final String ENQUEUE = "$zlink.admission-queue.enqueue";
    private final ZLinkSpotRouteMessages messages =
            new ZLinkSpotRouteMessages(new ZLinkStringMessageSerializer());

    @ParameterizedTest
    @CsvSource({
        "$zlink.admission-queue.enqueue, $zlink.admission-queue.enqueue.reply",
        "$zlink.admission-queue.report, $zlink.admission-queue.report.reply",
        "$zlink.admission-queue.admit-permit, $zlink.admission-queue.admit-permit.reply",
        "ApplicationRequest, ApplicationRequest",
        "$zlink.admission-queue.release-permit, $zlink.admission-queue.release-permit"
    })
    void writesSpotResponseName(String packetName, String replyName) {
        var request = requestHeader(packetName);
        try (Message frame = ZLinkSpotRouteMessages.encodeReplyHeader(request);
                Message body = Message.from("response".getBytes(StandardCharsets.UTF_8))) {
            var reply = ZLinkChannelEnvelope.decodeHeader(frame, false);
            assertEquals(replyName, reply.messageName());
            assertEquals(ZLinkChannelEnvelope.KIND_RESPONSE, reply.kind());
            assertEquals(request.correlationId(), reply.correlationId());
            assertEquals(
                    "response",
                    messages.decodeReply(
                            List.of(frame, body), String.class, Optional.of(packetName)));
        }
    }

    @ParameterizedTest
    @ValueSource(strings = {ENQUEUE, "$zlink.admission-queue.report.reply", "ApplicationResponse"})
    void rejectsWrongAdmissionQueueResponseName(String wrongName) {
        try (Message frame =
                        ZLinkChannelEnvelope.encodeHeader(
                                ZLinkChannelEnvelope.reply(requestHeader(ENQUEUE), wrongName));
                Message body = Message.from(new byte[0])) {
            var error =
                    assertThrows(
                            ZLinkFrameworkException.class,
                            () ->
                                    messages.decodeReply(
                                            List.of(frame, body),
                                            String.class,
                                            Optional.of(ENQUEUE)));
            assertEquals(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, error.kind());
        }
    }

    private static ZLinkChannelEnvelope.Header requestHeader(String name) {
        return ZLinkChannelEnvelope.create(
                ZLinkChannelEnvelope.KIND_REQUEST,
                "lane",
                name,
                ZLinkChannelEnvelope.DEFAULT_CONTENT_TYPE,
                null,
                Map.of(),
                null);
    }
}
