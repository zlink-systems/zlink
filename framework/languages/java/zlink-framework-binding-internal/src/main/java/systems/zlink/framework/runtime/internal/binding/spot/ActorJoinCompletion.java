/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

/** The typed payload of an actor join completion record. */
public record ActorJoinCompletion(
    ActorJoinDecision joinResult,
    ActorRef actor,
    ActorLocation location) implements MeshRecordPayload {
}
