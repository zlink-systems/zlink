/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.eventing;


/** Callback invoked for each socket monitor event. */
@FunctionalInterface
public interface SocketMonitorHandler {
    void onEvent(MonitorEvent event);
}
