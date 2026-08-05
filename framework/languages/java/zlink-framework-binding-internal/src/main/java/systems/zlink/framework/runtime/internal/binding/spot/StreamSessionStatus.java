/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

/** A point-in-time status snapshot of a STREAM session service. */
record StreamSessionStatus(StreamSessionState state, long lifecycleGeneration,
                                  long sessionCount, long bindingCount,
                                  long pendingMessageCount, long pendingByteCount,
                                  int lastError) {
}
