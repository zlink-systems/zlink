/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.core;

/** A background thread created and owned by the zlink context. The caller owns this resource and must close it. */
public interface ZlinkThread extends AutoCloseable {
    void join();

    @Override
    void close();
}
