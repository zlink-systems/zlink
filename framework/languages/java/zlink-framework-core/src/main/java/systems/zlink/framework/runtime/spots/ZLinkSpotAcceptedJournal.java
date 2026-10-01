package systems.zlink.framework.runtime.spots;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Optional;

final class ZLinkSpotAcceptedJournal {
    private ZLinkSpotAcceptedJournal() {}

    static byte[] encode(ZLinkBackendReceived received) {
        byte[] canonical = received.acceptedJournalRecord();
        if (canonical.length == 0) {
            throw new IllegalStateException(
                    "canonical accepted Spot journal record is unavailable");
        }
        return canonical;
    }

    static Record decode(byte[] encoded) {
        if (systems.zlink.framework.runtime.internal.service.ZLinkServiceFrozenRecordCodec
                .isCanonical(encoded)) {
            var decoded =
                    systems.zlink.framework.runtime.internal.service.ZLinkServiceFrozenRecordCodec
                            .decodeSpot(encoded);
            return new Record(
                    ZLinkBackendRequestResult.OK,
                    Optional.of(decoded.sourceNodeRid()),
                    decoded.sourceSpotId(),
                    decoded.replyRouteId(),
                    decoded.metadataFrame(),
                    decodeApplicationParts(
                            decoded.packetName(), decoded.contentType(), decoded.payload()),
                    decoded.operationHigh(),
                    decoded.operationLow(),
                    decoded.sourceOwnerId(),
                    decoded.sourceOwnerLeaseGeneration(),
                    decoded.sourceNodeGeneration(),
                    decoded.objectGeneration());
        }
        throw new IllegalArgumentException(
                "accepted Spot journal record is not canonical service-wire-v1");
    }

    private static List<byte[]> decodeApplicationParts(
            String packetName, String contentType, byte[] payload) {
        if (!ServiceWireConstants.FRAMEWORK_MULTIPART_PACKET_NAME.equals(packetName)) {
            return List.of(packetName.getBytes(StandardCharsets.UTF_8), payload);
        }
        List<Message> parts =
                ZLinkServiceM6AWireCodec.decodeFrameworkMultipart(
                        new ZLinkServiceM6AWireCodec.ApplicationPayload(
                                packetName, contentType, payload));
        try {
            return parts.stream().map(Message::toByteArray).toList();
        } finally {
            parts.forEach(Message::close);
        }
    }

    record Record(
            ZLinkBackendRequestResult result,
            Optional<RoutingId> routingId,
            Optional<String> spotId,
            Optional<Long> requestSequence,
            byte[] applicationMetadata,
            List<byte[]> parts,
            long operationHigh,
            long operationLow,
            String sourceOwnerId,
            long sourceOwnerLeaseGeneration,
            long sourceNodeGeneration,
            long objectGeneration) {
        Record {
            if (operationHigh == 0 && operationLow == 0) {
                throw new IllegalArgumentException(
                        "accepted Spot operation identity must not be zero");
            }
            // sourceNodeGeneration is a node lifecycle-generation opaque
            // equality token (.NET ulong, spec 01-glossary "Lifecycle
            // generation"): full range, only zero is unassigned. A signed
            // `<= 0` sentinel wrongly rejects a legitimate negative-as-long
            // value. sourceOwnerLeaseGeneration/objectGeneration are
            // spec-bounded to `1..long.MaxValue`, so `<= 0` is correct there.
            if (sourceOwnerId == null
                    || sourceOwnerId.isBlank()
                    || sourceOwnerLeaseGeneration <= 0
                    || sourceNodeGeneration == 0
                    || objectGeneration <= 0) {
                throw new IllegalArgumentException("accepted Spot source fence is invalid");
            }
            applicationMetadata = applicationMetadata.clone();
            parts = parts.stream().map(byte[]::clone).toList();
        }

        @Override
        public byte[] applicationMetadata() {
            return applicationMetadata.clone();
        }

        @Override
        public List<byte[]> parts() {
            return parts.stream().map(byte[]::clone).toList();
        }
    }
}
