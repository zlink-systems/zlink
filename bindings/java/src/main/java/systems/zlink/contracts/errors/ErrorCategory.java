/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.errors;

/** Selects the public exception family used when mapping a native errno. */
public enum ErrorCategory {
    CONFIG,
    BIND,
    CONNECT,
    CLOSE,
    HANDLER,
    RECV,
    REQUEST,
    SUBMIT
}
