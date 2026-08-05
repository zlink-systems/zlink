/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

/** The 128-bit identity shared by both sides of an actor transfer. */
public record ActorTransferId(long high, long low) {
}
