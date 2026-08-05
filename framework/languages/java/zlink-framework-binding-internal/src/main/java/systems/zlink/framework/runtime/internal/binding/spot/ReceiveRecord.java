/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

import systems.zlink.contracts.core.RoutingId;

/**
 * Metadata describing a single received message within a receive batch.
 *
 * @param kind the record kind
 * @param domain the ready domain bit mask
 * @param sourceNodeRid the source node routing id, if any
 * @param sourceSpotId the source spot routing id, if any
 * @param sourceBindingGeneration the validated bound-session generation, or zero
 * @param sourceActor the source actor reference, if any
 * @param operationId the correlated operation id, if any
 * @param operationKind the correlated operation kind
 * @param applicationCorrelation the application request correlation, if any
 * @param replyToken a reply token for request records, if any
 * @param channelName the channel name, if applicable
 * @param topic the topic, if applicable
 * @param contentType the wire content type for an application payload, if present
 * @param applicationMetadata opaque application metadata, if any
 * @param kindData the typed record-specific payload, if any
 * @param terminalResult the terminal result code for completions
 * @param failureErrno the failure errno for completions
 * @param partOffset the offset of this record's first part in the batch
 * @param partCount the number of parts belonging to this record
 */
public record ReceiveRecord(RecordKind kind, int domain, RoutingId sourceNodeRid,
                            RoutingId sourceSpotId, long sourceBindingGeneration,
                            ActorRef sourceActor,
                            OperationId operationId, OperationKind operationKind,
                            Long applicationCorrelation,
                            ReplyToken replyToken, String channelName, String topic,
                            String contentType,
                            byte[] applicationMetadata, MeshRecordPayload kindData,
                            int terminalResult,
                            int failureErrno, int partOffset, int partCount) {
    /** Creates a record without a record-specific typed payload. */
    public ReceiveRecord(
        RecordKind kind,
        int domain,
        RoutingId sourceNodeRid,
        RoutingId sourceSpotId,
        ActorRef sourceActor,
        OperationId operationId,
        OperationKind operationKind,
        ReplyToken replyToken,
        String channelName,
        String topic,
        byte[] applicationMetadata,
        int terminalResult,
        int failureErrno,
        int partOffset,
        int partCount) {
        this(
            kind,
            domain,
            sourceNodeRid,
            sourceSpotId,
            0L,
            sourceActor,
            operationId,
            operationKind,
            null,
            replyToken,
            channelName,
            topic,
            null,
            applicationMetadata,
            null,
            terminalResult,
            failureErrno,
            partOffset,
            partCount);
    }

    /** Creates an application record while retaining its wire content type. */
    public ReceiveRecord(
        RecordKind kind,
        int domain,
        RoutingId sourceNodeRid,
        RoutingId sourceSpotId,
        ActorRef sourceActor,
        OperationId operationId,
        OperationKind operationKind,
        ReplyToken replyToken,
        String channelName,
        String topic,
        String contentType,
        byte[] applicationMetadata,
        int terminalResult,
        int failureErrno,
        int partOffset,
        int partCount) {
        this(
            kind,
            domain,
            sourceNodeRid,
            sourceSpotId,
            0L,
            sourceActor,
            operationId,
            operationKind,
            null,
            replyToken,
            channelName,
            topic,
            contentType,
            applicationMetadata,
            null,
            terminalResult,
            failureErrno,
            partOffset,
            partCount);
    }

    /** Creates an application record with its wire request correlation. */
    public ReceiveRecord(
        RecordKind kind,
        int domain,
        RoutingId sourceNodeRid,
        RoutingId sourceSpotId,
        ActorRef sourceActor,
        OperationId operationId,
        OperationKind operationKind,
        ReplyToken replyToken,
        String channelName,
        String topic,
        String contentType,
        Long applicationCorrelation,
        byte[] applicationMetadata,
        int terminalResult,
        int failureErrno,
        int partOffset,
        int partCount) {
        this(
            kind,
            domain,
            sourceNodeRid,
            sourceSpotId,
            0L,
            sourceActor,
            operationId,
            operationKind,
            applicationCorrelation,
            replyToken,
            channelName,
            topic,
            contentType,
            applicationMetadata,
            null,
            terminalResult,
            failureErrno,
            partOffset,
            partCount);
    }

    /** Returns the actor lifecycle payload when this is a Spot control record. */
    public ActorControlRecord actorControl() {
        return kindData instanceof ActorControlRecord value ? value : null;
    }

    /** Returns the actor join payload when this is an actor join completion. */
    public ActorJoinCompletion joinCompletion() {
        return kindData instanceof ActorJoinCompletion value ? value : null;
    }

    /** Returns the transfer control payload when this is a transfer-control record. */
    public ActorTransferControl transferControl() {
        return kindData instanceof ActorTransferControl value ? value : null;
    }

    /** Returns the destination-specific capacity signal for a send-ready record. */
    public MeshSendReadyData sendReady() {
        return kindData instanceof MeshSendReadyData value ? value : null;
    }
}
