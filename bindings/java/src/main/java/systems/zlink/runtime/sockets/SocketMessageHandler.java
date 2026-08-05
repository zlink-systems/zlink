/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import systems.zlink.contracts.messaging.Received;

@FunctionalInterface
public interface SocketMessageHandler {
    void onMessage(Received received);
}
