package systems.zlink.framework.runtime.spots;

import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

/**
 * Owns the production ingress boundary for canonical relocation controls.
 *
 * <p>The raw backend validates the complete service-wire record before this
 * owner receives it. Validation alone never makes a relocation transition
 * effective; the coordinator must also own its semantic state.</p>
 */
final class ZLinkCanonicalRelocationTransitionOwner {
    private final StateMachine stateMachine;

    ZLinkCanonicalRelocationTransitionOwner(StateMachine stateMachine) {
        this.stateMachine = Objects.requireNonNull(
            stateMachine, "stateMachine");
    }

    void install(ZLinkInternalMeshNode node) {
        Objects.requireNonNull(node, "node")
            .setCanonicalRelocationControlHandler(this::handle);
    }

    CompletionStage<Void> handle(
        RoutingId transportSource,
        byte[] encoded) {
        Objects.requireNonNull(transportSource, "transportSource");
        byte[] record = Objects.requireNonNull(encoded, "encoded").clone();
        int command = command(record);
        try {
            return Objects.requireNonNull(
                stateMachine.apply(transportSource, command, record),
                "canonical relocation state machine returned null");
        } catch (RuntimeException failure) {
            return CompletableFuture.failedFuture(failure);
        }
    }

    /**
     * Rejects every transition until a production coordinator owns the
     * corresponding decoded state.
     */
    static StateMachine unavailable() {
        return (source, command, encoded) -> CompletableFuture.failedFuture(
            new IllegalStateException(
                "canonical relocation command "
                    + command
                    + " has no production state owner"));
    }

    private static int command(byte[] record) {
        if (record.length < 5
            || Byte.toUnsignedInt(record[0]) != ServiceWireConstants.MAGIC_0
            || Byte.toUnsignedInt(record[1]) != ServiceWireConstants.MAGIC_1
            || Byte.toUnsignedInt(record[2])
                != ServiceWireConstants.WIRE_MAJOR) {
            throw new IllegalArgumentException(
                "canonical relocation prefix is invalid");
        }
        int command = Byte.toUnsignedInt(record[3]);
        boolean known =
            command == ServiceWireConstants.COMMAND_RELOCATION_READY
                || command == ServiceWireConstants.COMMAND_RELOCATION_DATA
                || command == ServiceWireConstants.COMMAND_RELOCATION_ACK
                || command == ServiceWireConstants.COMMAND_RELOCATION_SEAL
                || command == ServiceWireConstants.COMMAND_RELOCATION_COMPLETE
                || command == ServiceWireConstants.COMMAND_RELOCATION_PREPARE
                || command
                    == ServiceWireConstants.COMMAND_RELOCATION_RESERVED;
        if (!known) {
            throw new IllegalArgumentException(
                "canonical relocation command is invalid");
        }
        return command;
    }

    @FunctionalInterface
    interface StateMachine {
        CompletionStage<Void> apply(
            RoutingId transportSource,
            int command,
            byte[] encoded);
    }
}
