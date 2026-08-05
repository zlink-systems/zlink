/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.core;

/** A thread-safe atomic integer counter. The caller owns this resource and must close it. */
public interface AtomicCounter extends AutoCloseable {
    void set(int value);

    int increment();

    int decrement();

    int value();

    @Override
    void close();
}
