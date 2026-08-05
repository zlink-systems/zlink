/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

/** The typed payload of an infrastructure transfer-control record. */
record ActorTransferControl(
    ActorTransferPhase phase,
    ActorTransferRole role,
    ActorTransferId transferId,
    ActorRef actor,
    long membershipEpoch,
    long finalSequence,
    int resultCode,
    int failureErrno) implements MeshRecordPayload {
}
