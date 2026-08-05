/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.errors;

import systems.zlink.internal.ContractAccess;
import systems.zlink.runtime.nativeapi.Native;

final class NativeErrorRuntime {
    static {
        ContractAccess.register(new ContractAccess.NativeErrorAccess() {
            @Override
            public int errno() {
                return Native.errno();
            }

            @Override
            public String strerror(int errno) {
                return Native.strerror(errno);
            }
        });
    }

    private NativeErrorRuntime() {
    }

}
