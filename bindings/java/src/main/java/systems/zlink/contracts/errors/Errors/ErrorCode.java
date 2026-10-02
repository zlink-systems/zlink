/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.errors;

import systems.zlink.internal.NativeErrorCodes;

/** Native zlink-specific errno values above the POSIX range. */
enum ErrorCode {
    EFSM(NativeErrorCodes.EFSM),
    ENOCOMPATPROTO(NativeErrorCodes.ENOCOMPATPROTO),
    ETERM(NativeErrorCodes.ETERM),
    EMTHREAD(NativeErrorCodes.EMTHREAD);

    private final int value;
    ErrorCode(int v) { this.value = v; }
    public int getValue() { return value; }
}
