package systems.zlink.framework.runtime.internal.service;

import systems.zlink.contracts.core.RoutingId;

import java.util.Objects;
import java.util.Optional;

/** Adapts the canonical durable Instance activation recovery envelope. */
public final class ZLinkInstanceActivationRecoveryCodec {
    public void validateColdActivation(
            ZLinkServiceM6BWireCodec.InstanceSpotMessage message,
            RecoveryEnvelope envelope,
            byte[] metadata) {
        if (!(message.route() instanceof ZLinkServiceM6BWireCodec.InstanceColdActivation cold)
                || !message.instanceIntent()
                || !cold.targetSpotId().equals(envelope.targetSpotId())
                || !cold.targetNodeRid().equals(envelope.targetNodeRid())
                || cold.targetNodeGeneration() != envelope.targetNodeGeneration()
                || !cold.targetMeshName().equals(envelope.targetMeshName())
                || !cold.stableType().equals(envelope.stableType())
                || !cold.targetDescriptorVersion().equals(envelope.descriptorVersion())
                || cold.deadlineUnixMs() != envelope.deadlineUnixMs()
                || !message.sourceNodeRid().equals(envelope.sourceNodeRid())
                || message.sourceNodeGeneration() != envelope.sourceNodeGeneration()
                || !java.util.Objects.equals(
                        message.sourceSpotId(), envelope.sourceSpotId().orElse(null))
                || message.request() != envelope.request()
                || message.operationHigh() != envelope.operationHigh()
                || message.operationLow() != envelope.operationLow()
                || !java.util.Objects.equals(message.replyRouteId(), envelope.replyRouteId())
                || ((message.flags()
                                        & systems.zlink.framework.runtime.protocol
                                                .ServiceWireConstants.FLAG_METADATA)
                                != 0)
                        != (envelope.metadataFrame().length != 0)
                || !java.util.Arrays.equals(metadata, envelope.metadataFrame())) {
            throw new systems.zlink.framework.errors.ZLinkFrameworkException(
                    systems.zlink.framework.errors.ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                    "Instance cold activation route and recovery envelope do not match");
        }
    }

    private static final String DURABLE_TYPE = "instance-activation-recovery-v1";

    public byte[] encode(RecoveryEnvelope envelope) {
        byte[] metadata = envelope.metadataFrame();
        try {
            var value =
                    new ServiceWireCodec.InstanceActivationRecoveryV1(
                            new ServiceWireCodec.Text8(envelope.targetSpotId()),
                            new ServiceWireCodec.Text8(envelope.stableType()),
                            new ServiceWireCodec.Text8(envelope.targetMeshName()),
                            new ServiceWireCodec.Rid(envelope.targetNodeRid().toBytes()),
                            new ServiceWireCodec.NonzeroU64(envelope.targetNodeGeneration()),
                            new ServiceWireCodec.Text8(envelope.descriptorVersion()),
                            new ServiceWireCodec.Rid(envelope.sourceNodeRid().toBytes()),
                            new ServiceWireCodec.NonzeroU64(envelope.sourceNodeGeneration()),
                            envelope.sourceSpotId().isPresent()
                                    ? ServiceWireCodec.Bool8.TRUE
                                    : ServiceWireCodec.Bool8.FALSE,
                            envelope.sourceSpotId().map(ServiceWireCodec.Text8::new).orElse(null),
                            envelope.request()
                                    ? ServiceWireCodec.InstanceOperationKind.REQUEST
                                    : ServiceWireCodec.InstanceOperationKind.SEND,
                            new ServiceWireCodec.OperationId(
                                    new ServiceWireCodec.U64(envelope.operationHigh()),
                                    new ServiceWireCodec.U64(envelope.operationLow())),
                            envelope.request()
                                    ? new ServiceWireCodec.InstanceReplyRouteRequest(
                                            new ServiceWireCodec.NonzeroU64(
                                                    envelope.replyRouteId()))
                                    : new ServiceWireCodec.InstanceReplyRouteSend(),
                            new ServiceWireCodec.NonzeroU64(envelope.deadlineUnixMs()),
                            metadata.length > 0
                                    ? ServiceWireCodec.Bool8.TRUE
                                    : ServiceWireCodec.Bool8.FALSE,
                            metadata.length > 0
                                    ? ServiceWireCodec.decodeMetadataFrame(
                                            metadata, ZLinkServiceM6BWireCodec.generatedContext())
                                    : null,
                            ServiceWireCodec.decodeApplicationPayloadEnvelopeV1(
                                    envelope.applicationPayloadFrame(),
                                    ZLinkServiceM6BWireCodec.generatedContext()));
            return ServiceWireCodec.encodeDurable(
                    DURABLE_TYPE, value, ZLinkServiceM6BWireCodec.generatedContext());
        } catch (java.io.IOException failure) {
            throw new IllegalArgumentException(
                    "invalid Instance activation recovery envelope", failure);
        }
    }

    public RecoveryEnvelope decode(byte[] encoded) {
        try {
            var value =
                    (ServiceWireCodec.InstanceActivationRecoveryV1)
                            ServiceWireCodec.decodeDurable(
                                    DURABLE_TYPE,
                                    encoded,
                                    ZLinkServiceM6BWireCodec.generatedContext());
            return new RecoveryEnvelope(
                    value.targetSpotId().value(),
                    value.stableType().value(),
                    value.targetMeshName().value(),
                    RoutingId.from(value.targetNodeRid().value()),
                    value.targetNodeGeneration().value(),
                    value.targetDescriptorVersion().value(),
                    RoutingId.from(value.sourceNodeRid().value()),
                    value.sourceNodeGeneration().value(),
                    value.hasSourceSpotId() == ServiceWireCodec.Bool8.TRUE
                            ? Optional.of(value.sourceSpotId().value())
                            : Optional.empty(),
                    value.operationKind() == ServiceWireCodec.InstanceOperationKind.REQUEST,
                    value.operation().high().value(),
                    value.operation().low().value(),
                    value.replyRoute() instanceof ServiceWireCodec.InstanceReplyRouteRequest request
                            ? request.replyRouteId().value()
                            : null,
                    value.deadlineUnixMs().value(),
                    value.hasMetadata() == ServiceWireCodec.Bool8.TRUE
                            ? ServiceWireCodec.encodeMetadataFrame(
                                    value.metadata(), ZLinkServiceM6BWireCodec.generatedContext())
                            : new byte[0],
                    ServiceWireCodec.encodeApplicationPayloadEnvelopeV1(
                            value.applicationPayload(),
                            ZLinkServiceM6BWireCodec.generatedContext()));
        } catch (java.io.IOException failure) {
            throw new IllegalArgumentException(
                    "invalid Instance activation recovery envelope", failure);
        }
    }

    public record RecoveryEnvelope(
            String targetSpotId,
            String stableType,
            String targetMeshName,
            RoutingId targetNodeRid,
            long targetNodeGeneration,
            String descriptorVersion,
            RoutingId sourceNodeRid,
            long sourceNodeGeneration,
            Optional<String> sourceSpotId,
            boolean request,
            long operationHigh,
            long operationLow,
            Long replyRouteId,
            long deadlineUnixMs,
            byte[] metadataFrame,
            byte[] applicationPayloadFrame) {
        public RecoveryEnvelope {
            sourceSpotId = Objects.requireNonNull(sourceSpotId, "sourceSpotId");
            metadataFrame = Objects.requireNonNull(metadataFrame, "metadataFrame").clone();
            applicationPayloadFrame =
                    Objects.requireNonNull(applicationPayloadFrame, "applicationPayloadFrame")
                            .clone();
        }

        @Override
        public byte[] metadataFrame() {
            return metadataFrame.clone();
        }

        @Override
        public byte[] applicationPayloadFrame() {
            return applicationPayloadFrame.clone();
        }
    }
}
