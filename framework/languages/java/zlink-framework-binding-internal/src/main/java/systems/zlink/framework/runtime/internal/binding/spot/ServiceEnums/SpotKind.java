/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

/** The kind of a spot. */
enum SpotKind {
    /** No spot kind (unset). */
    INVALID,
    /** A node's well-known entry spot. */
    ENTRY,
    /** A user-created spot. */
    USER
}
