/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

/** Reservation and frozen-range detail returned by actor transfer prepare. */
public record ActorTransferPrepareResult(
    ActorTransferRole role,
    ActorTransferId transferId,
    ActorRef actor,
    long finalSequence,
    long reserveMessageCount,
    long reserveByteCount) {
}
