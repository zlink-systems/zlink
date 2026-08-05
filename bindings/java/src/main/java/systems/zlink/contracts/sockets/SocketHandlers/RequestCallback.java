/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;

import systems.zlink.contracts.messaging.Message;
import java.util.List;

/**
 * Receives request completion and reply payloads.
 *
 * <p>When {@code result} is {@link RequestResult#OK}, the callback owns the
 * messages in {@code parts} and must close them. Non-success results provide
 * an empty list.
 */
@FunctionalInterface
public interface RequestCallback {
    void onComplete(RequestResult result, List<Message> parts);
}
