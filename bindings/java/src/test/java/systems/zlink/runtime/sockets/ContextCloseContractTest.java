/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.runtime.sockets;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.CloseResult;
import systems.zlink.contracts.errors.ZlinkCloseException;

final class ContextCloseContractTest {
    @Test
    void closeCallsTermOnceAfterShutdown() throws Exception {
        TestSupport.assumeNative();
        CompletionNativeFixture.runProbe(TermProbe.class);
    }

    public static final class TermProbe {
        public static void main(String[] args) throws Throwable {
            CompletionNativeFixture core = new CompletionNativeFixture();
            Context context = Zlink.createContext();
            core.ctxTermInterrupted.set(1);
            ZlinkCloseException failure = assertThrows(ZlinkCloseException.class,
                context::close);
            assertEquals(CloseResult.INTERNAL_ERROR, failure.getResult());
            assertEquals(1, core.ctxTermCalls.get());
            System.exit(0);
        }
    }
}
