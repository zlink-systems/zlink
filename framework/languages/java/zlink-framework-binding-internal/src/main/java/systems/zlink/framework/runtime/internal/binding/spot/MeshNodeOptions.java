/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

/**
 * Creation options for a mesh node.
 *
 * @param meshName the mesh membership name, or null/empty for the default
 * @param trustProfile the trust profile name, or null/empty for the default
 */
record MeshNodeOptions(String meshName, String trustProfile) {
    /** Returns options with all fields unset (defaults). */
    public static MeshNodeOptions defaults() {
        return new MeshNodeOptions(null, null);
    }
}
