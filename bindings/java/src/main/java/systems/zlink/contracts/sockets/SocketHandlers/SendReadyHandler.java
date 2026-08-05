/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;


/** Callback invoked when a socket becomes ready to send. */
@FunctionalInterface
public interface SendReadyHandler {
    void onReady();
}
