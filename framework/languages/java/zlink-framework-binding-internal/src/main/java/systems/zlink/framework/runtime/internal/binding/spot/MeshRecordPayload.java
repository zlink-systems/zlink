/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

/** Marker for a typed payload carried in a receive record's kind data. */
sealed interface MeshRecordPayload
    permits ActorControlRecord, ActorJoinCompletion, ActorTransferControl,
    MeshSendReadyData {
}
