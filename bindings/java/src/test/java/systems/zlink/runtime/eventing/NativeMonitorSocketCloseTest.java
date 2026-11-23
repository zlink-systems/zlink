/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.eventing;

import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.CloseResult;
import systems.zlink.contracts.errors.ZlinkCloseException;
import systems.zlink.runtime.nativeapi.InternalAccess;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

final class NativeMonitorSocketCloseTest {
    @Test
    void rejectedNativeClosePreservesHandle() {
        try (var context = Zlink.createContext();
             var socket = context.createStreamSocket()) {
            // Core rejects monitor close on a live socket with no monitor state.
            var handle = InternalAccess.socketHandle(socket);
            var monitor = new NativeMonitorSocket(handle, true);

            var failure = assertThrows(ZlinkCloseException.class, monitor::close);

            assertEquals(CloseResult.INVALID_HANDLE, failure.getResult());
            assertEquals(handle, monitor.handle());
        }
    }
}
