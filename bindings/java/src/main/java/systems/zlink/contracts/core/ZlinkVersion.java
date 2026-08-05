/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.core;

/** Exposes the native zlink library version. */
public final class ZlinkVersion {
    private ZlinkVersion() {}

    public static int[] get() {
        return Zlink.version();
    }
}
