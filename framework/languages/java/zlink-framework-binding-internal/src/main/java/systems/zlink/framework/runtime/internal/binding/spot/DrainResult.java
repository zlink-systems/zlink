/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

/**
 * The outcome of a {@code drainReady} call.
 *
 * @param resultCode the underlying recv result code (0 == OK)
 * @param hasResidue whether more ready owners remain undrained
 */
public record DrainResult(int resultCode, boolean hasResidue) {
}
