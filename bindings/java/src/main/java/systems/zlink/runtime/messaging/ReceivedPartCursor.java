/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.messaging;

import systems.zlink.internal.ContractAccess;
import systems.zlink.contracts.messaging.Message;

public interface ReceivedPartCursor extends ContractAccess.ReceivedPartCursor {
    @Override
    Message nextPartOrNull();

    @Override
    void close();
}
