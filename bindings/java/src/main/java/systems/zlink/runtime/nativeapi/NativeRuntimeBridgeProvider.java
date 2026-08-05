/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.nativeapi;

import systems.zlink.internal.RuntimeBridgeProvider;

/** Module-private service that initializes native contract accessors. */
public final class NativeRuntimeBridgeProvider
    implements RuntimeBridgeProvider {
    @Override
    public void initializeNativeErrorAccess() {
        load("systems.zlink.runtime.errors.NativeErrorRuntime");
    }

    @Override
    public void initializeNativeMessageAccess() {
        load("systems.zlink.runtime.messaging.NativeMessageRuntime");
    }

    private static void load(String className) {
        try {
            Class.forName(className, true,
                NativeRuntimeBridgeProvider.class.getClassLoader());
        } catch (ClassNotFoundException ex) {
            throw new IllegalStateException("cannot load " + className, ex);
        }
    }
}
