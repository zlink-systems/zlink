/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.eventing;


/** Callback invoked when a timer fires. */
@FunctionalInterface
public interface TimerHandler {
    void onFire(ZlinkTimer timer, long fireCount);
}
