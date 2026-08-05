/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

/**
 * The batch capacity required to receive a claim's pending messages.
 *
 * @param messageCount required message-record capacity
 * @param partCount required part capacity
 * @param byteCount required byte capacity
 */
public record ReceiveRequirements(long messageCount, long partCount, long byteCount) {
}
