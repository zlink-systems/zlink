/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

/**
 * Identifies an in-flight mesh request/operation.
 *
 * @param high the high 64 bits of the operation id
 * @param low the low 64 bits of the operation id
 */
public record OperationId(long high, long low) {
}
