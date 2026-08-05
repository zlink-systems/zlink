/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

/** Callback invoked when a mesh node's dispatch ready index becomes non-empty. */
@FunctionalInterface
public interface MeshReadyHandler {
    /**
     * Handles a ready notification.
     *
     * @param readyDomains the domains that became ready (bit mask)
     * @return the domain mask the handler still wishes to be notified about
     */
    int onReady(int readyDomains);
}
