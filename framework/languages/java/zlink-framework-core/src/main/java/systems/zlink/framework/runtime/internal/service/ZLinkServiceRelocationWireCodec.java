package systems.zlink.framework.runtime.internal.service;

import java.io.IOException;
import java.util.List;
import java.util.Objects;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;
import systems.zlink.framework.runtime.protocol.ServiceWirePilotCodec;

/** Canonical service-wire command 33/46 relocation reply codec. */
public final class ZLinkServiceRelocationWireCodec {
    public byte[] encodeReplyRelay(ReplyRelay value) {
        Objects.requireNonNull(value, "value");
        value.validate();
        try {
            List<byte[]> frames = ServiceWirePilotCodec.encodeReplyRelay33(
                new ServiceWirePilotCodec.ReplyRelay33(
                    toGenerated(value.operation()),
                    value.replyRouteId(),
                    new ServiceWirePilotCodec.MaintenanceRelocationReplyContext(
                        toGenerated(value.relocation()),
                        value.targetAttemptGeneration(),
                        toGenerated(value.coordinator()),
                        value.participantId(),
                        value.sequence()),
                    value.terminalResult(),
                    value.failureCode(),
                    null));
            if (frames.size() != 1) {
                throw invalid("command 33 frame projection");
            }
            return frames.get(0);
        } catch (IOException failure) {
            throw invalid("command 33 field", failure);
        }
    }

    public ReplyRelay decodeReplyRelay(byte[] encoded) {
        Objects.requireNonNull(encoded, "encoded");
        try {
            var generated = ServiceWirePilotCodec.decodeReplyRelay33(
                List.of(encoded));
            if (!(generated.context() instanceof
                    ServiceWirePilotCodec.MaintenanceRelocationReplyContext
                        context)
                || generated.payload() != null) {
                throw invalid("command 33 frame projection");
            }
            ReplyRelay result = new ReplyRelay(
                fromGenerated(generated.operation()),
                generated.replyRouteId(),
                fromGenerated(context.relocation()),
                context.targetAttemptGeneration(),
                fromGenerated(context.coordinator()),
                context.participantId(),
                context.sequence(),
                generated.terminalResult(),
                generated.failureCode());
            result.validate();
            return result;
        } catch (IOException failure) {
            throw invalid("command 33 frame", failure);
        }
    }

    public byte[] encodeReplyRelayAck(ReplyRelayAck value) {
        Objects.requireNonNull(value, "value");
        value.validate();
        try {
            return ServiceWirePilotCodec.encodeReplyRelayAck46(
                new ServiceWirePilotCodec.ReplyRelayAck46(
                    toGenerated(value.relocation()),
                    toGenerated(value.coordinator()),
                    toGenerated(value.operation()),
                    value.replyRouteId(),
                    toGenerated(value.requestSource()),
                    value.status()));
        } catch (IOException failure) {
            throw invalid("command 46 field", failure);
        }
    }

    public ReplyRelayAck decodeReplyRelayAck(byte[] encoded) {
        Objects.requireNonNull(encoded, "encoded");
        try {
            var generated = ServiceWirePilotCodec.decodeReplyRelayAck46(encoded);
            ReplyRelayAck result = new ReplyRelayAck(
                fromGenerated(generated.relocation()),
                fromGenerated(generated.coordinator()),
                fromGenerated(generated.operation()),
                generated.replyRouteId(),
                fromGenerated(generated.requestSource()),
                generated.status());
            result.validate();
            return result;
        } catch (IOException failure) {
            throw invalid("command 46 frame", failure);
        }
    }

    private static ServiceWirePilotCodec.OperationId toGenerated(
        Operation value) {
        return new ServiceWirePilotCodec.OperationId(value.high(), value.low());
    }

    private static Operation fromGenerated(
        ServiceWirePilotCodec.OperationId value) {
        return new Operation(value.high(), value.low());
    }

    private static ServiceWirePilotCodec.RelocationId toGenerated(
        RelocationId value) {
        return new ServiceWirePilotCodec.RelocationId(value.high(), value.low());
    }

    private static RelocationId fromGenerated(
        ServiceWirePilotCodec.RelocationId value) {
        return new RelocationId(value.high(), value.low());
    }

    private static ServiceWirePilotCodec.CoordinatorFence toGenerated(
        CoordinatorFence value) {
        return new ServiceWirePilotCodec.CoordinatorFence(
            value.ownerId(),
            value.leaseGeneration(),
            value.nodeRid().toBytes(),
            value.nodeGeneration(),
            value.expectedAuthorityStoreVersion());
    }

    private static CoordinatorFence fromGenerated(
        ServiceWirePilotCodec.CoordinatorFence value) {
        return new CoordinatorFence(
            value.coordinatorOwnerId(),
            value.coordinatorLeaseGeneration(),
            RoutingId.from(value.coordinatorNodeRid()),
            value.coordinatorNodeGeneration(),
            value.expectedAuthorityStoreVersion());
    }

    private static ServiceWirePilotCodec.RequestSourceFence toGenerated(
        RequestSourceFence value) {
        return new ServiceWirePilotCodec.RequestSourceFence(
            value.ownerId(),
            value.leaseGeneration(),
            value.nodeRid().toBytes(),
            value.nodeGeneration());
    }

    private static RequestSourceFence fromGenerated(
        ServiceWirePilotCodec.RequestSourceFence value) {
        return new RequestSourceFence(
            value.sourceOwnerId(),
            value.sourceOwnerLeaseGeneration(),
            RoutingId.from(value.sourceNodeRid()),
            value.sourceNodeGeneration());
    }

    private static boolean validFailureCode(int value) {
        return value >= 0 && value <= 22
            || value >= 33 && value <= 35;
    }

    private static boolean validTerminalResult(int value) {
        return value == 0 || value >= 101 && value <= 113;
    }

    private static boolean validTerminalFailure(
        int terminalResult, int failureCode) {
        //  Schema terminal-failure-integrity (spec 51:43-47): delegate to the
        //  generated single source instead of redefining the pair table here.
        return ServiceWireConstants.validTerminalFailure(
            terminalResult, failureCode);
    }

    private static IllegalArgumentException invalid(String field) {
        return new IllegalArgumentException(
            "invalid canonical reply relay " + field);
    }

    private static IllegalArgumentException invalid(
        String field, Throwable cause) {
        return new IllegalArgumentException(
            "invalid canonical reply relay " + field, cause);
    }

    public record Operation(long high, long low) {
        private void validate() {
            if (high == 0 && low == 0) throw invalid("operation");
        }
    }

    public record RelocationId(long high, long low) {
        private void validate() {
            if (high == 0 && low == 0) throw invalid("relocation id");
        }
    }

    public record CoordinatorFence(
        String ownerId,
        long leaseGeneration,
        RoutingId nodeRid,
        long nodeGeneration,
        String expectedAuthorityStoreVersion) {
        private void validate() {
            requireText(ownerId, "coordinator owner");
            requireText(expectedAuthorityStoreVersion,
                "expected authority store version");
            Objects.requireNonNull(nodeRid, "nodeRid");
            if (leaseGeneration == 0 || nodeGeneration == 0) {
                throw invalid("coordinator fence");
            }
        }
    }

    public record RequestSourceFence(
        String ownerId,
        long leaseGeneration,
        RoutingId nodeRid,
        long nodeGeneration) {
        private void validate() {
            requireText(ownerId, "request source owner");
            Objects.requireNonNull(nodeRid, "nodeRid");
            if (leaseGeneration == 0 || nodeGeneration == 0) {
                throw invalid("request source fence");
            }
        }
    }

    public record ReplyRelay(
        Operation operation,
        long replyRouteId,
        RelocationId relocation,
        long targetAttemptGeneration,
        CoordinatorFence coordinator,
        long participantId,
        long sequence,
        int terminalResult,
        int failureCode) {
        private void validate() {
            Objects.requireNonNull(operation, "operation").validate();
            Objects.requireNonNull(relocation, "relocation").validate();
            Objects.requireNonNull(coordinator, "coordinator").validate();
            if (replyRouteId == 0 || targetAttemptGeneration == 0
                || participantId == 0 || sequence == 0
                || !validTerminalResult(terminalResult)
                || !validFailureCode(failureCode)
                || !validTerminalFailure(terminalResult, failureCode)) {
                throw invalid("command 33 field");
            }
        }
    }

    public record ReplyRelayAck(
        RelocationId relocation,
        CoordinatorFence coordinator,
        Operation operation,
        long replyRouteId,
        RequestSourceFence requestSource,
        int status) {
        private void validate() {
            Objects.requireNonNull(relocation, "relocation").validate();
            Objects.requireNonNull(coordinator, "coordinator").validate();
            Objects.requireNonNull(operation, "operation").validate();
            Objects.requireNonNull(requestSource, "requestSource").validate();
            if (replyRouteId == 0 || status != 1 && status != 2) {
                throw invalid("command 46 field");
            }
        }
    }

    private static void requireText(String value, String field) {
        if (value == null || value.isEmpty() || value.indexOf('\0') >= 0) {
            throw invalid(field);
        }
    }

}
