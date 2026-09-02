/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.eventing;

import java.time.Duration;

/** Fires on an interval and can be polled or awaited. The caller owns this resource and must close it. */
public interface ZlinkTimer extends AutoCloseable {

    void start(Duration interval, long repeatCount);

    void stop();

    long recv();

    @Override
    void close();
}
