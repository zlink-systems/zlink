/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.internal;

/** Loads runtime implementations required by package-neutral contract state. */
public interface RuntimeBridgeProvider {
    void initializeNativeErrorAccess();

    void initializeNativeMessageAccess();
}
