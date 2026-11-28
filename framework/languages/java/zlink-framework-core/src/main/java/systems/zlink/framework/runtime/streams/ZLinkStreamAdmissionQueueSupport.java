package systems.zlink.framework.runtime.streams;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.ObjectMapper;

import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.AdmitPermit;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.AdmitPermitReply;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.Delivery;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.Enqueue;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.EnqueueReply;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.NodeReport;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.NodeReportReply;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.ReleasePermit;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

import java.util.EnumSet;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletionStage;

/** STREAM-side wire and Spot-call plumbing of the login admission queue (§3.4, §3.5.3). */
final class ZLinkStreamAdmissionQueueSupport {
    static final String ENQUEUE_PACKET = "$zlink.admission.enqueue";
    static final String POSITION_PACKET = "$zlink.admission.position";
    static final String TICKET_PACKET = "$zlink.admission.ticket";
    static final String TICKET_METADATA = "$zlink.admission.ticket";
    static final Map<String, String> QUEUE_REQUIRED =
            Map.of(ZLinkAdmissionQueueNode.QUEUE_REQUIRED_METADATA, "1");

    private static final ObjectMapper JSON = new ObjectMapper();

    private ZLinkStreamAdmissionQueueSupport() {}

    /** Binds the node's Spot calls to the queue Instance Spot through the existing route client. */
    static ZLinkAdmissionQueueNode.SpotLink link(
            ZLinkRouteClient route, String spotId, String meshName) {
        return new ZLinkAdmissionQueueNode.SpotLink() {
            @Override
            public CompletionStage<NodeReportReply> report(NodeReport report) {
                return request(report, NodeReportReply.class);
            }

            @Override
            public CompletionStage<EnqueueReply> enqueue(Enqueue enqueue) {
                return request(enqueue, EnqueueReply.class);
            }

            @Override
            public CompletionStage<AdmitPermitReply> admitPermit(AdmitPermit request) {
                return request(request, AdmitPermitReply.class);
            }

            @Override
            public void releasePermit(ReleasePermit release) {
                route.sendToSpot(spotId, release)
                        .instanceSpot(ZLinkAdmissionQueueMessages.SPOT_TYPE)
                        .inMesh(meshName)
                        .submit();
            }

            private <T> CompletionStage<T> request(Object message, Class<T> replyType) {
                return route.requestToSpot(spotId, message)
                        .instanceSpot(ZLinkAdmissionQueueMessages.SPOT_TYPE)
                        .inMesh(meshName)
                        .submit(replyType);
            }
        };
    }

    static ZLinkStreamHeader enqueueResponseHeader(ZLinkStreamHeader request) {
        return ZLinkStreamHeader.createResponse(
                request,
                ZLinkStreamCodec.JSON,
                EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
                "",
                Map.of());
    }

    /** {@code {"position":n,"estimatedWaitMs":m}} or {@code {"ticket":"...","expiresInMs":t}}. */
    static byte[] enqueueResponseBody(EnqueueReply reply) {
        return pushBody(reply.delivery());
    }

    static ZLinkStreamHeader pushHeader(Delivery push) {
        return new ZLinkStreamHeader(
                ZLinkStreamMessageKind.SEND,
                ZLinkStreamCodec.JSON,
                EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
                Optional.empty(),
                push.ticket() != null ? TICKET_PACKET : POSITION_PACKET,
                Map.of(),
                Optional.empty(),
                Optional.empty(),
                Optional.empty());
    }

    static byte[] pushBody(Delivery push) {
        return body(push.ticket(), push.expiresInMs(), push.position(), push.estimatedWaitMs());
    }

    private static byte[] body(
            String ticket, Integer expiresInMs, Integer position, Integer estimatedMs) {
        Map<String, Object> fields = new LinkedHashMap<>();
        if (ticket != null) {
            fields.put("ticket", ticket);
            fields.put("expiresInMs", expiresInMs);
        } else {
            fields.put("position", position);
            fields.put("estimatedWaitMs", estimatedMs);
        }
        try {
            return JSON.writeValueAsBytes(fields);
        } catch (JsonProcessingException failure) {
            throw new IllegalStateException("failed to encode admission queue payload", failure);
        }
    }
}
