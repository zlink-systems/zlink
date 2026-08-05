/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.core;

import java.time.Duration;

/** A high-resolution elapsed-time stopwatch. The caller owns this resource and must close it. */
public interface ZlinkStopwatch extends AutoCloseable {
    Duration intermediate();

    Duration stop();

    @Override
    void close();
}
