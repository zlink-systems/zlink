/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contract;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;
import systems.zlink.contracts.sockets.CompletionKind;

class CompletionKindContractTest {
    @Test
    void preservesCoreCompletionKindValues() {
        assertEquals(1, CompletionKind.SEND.value());
        assertEquals(2, CompletionKind.REQUEST.value());
        assertEquals(3, CompletionKind.WRITABLE.value());

        assertSame(CompletionKind.SEND, CompletionKind.fromValue(1));
        assertSame(CompletionKind.REQUEST, CompletionKind.fromValue(2));
        assertSame(CompletionKind.WRITABLE, CompletionKind.fromValue(3));
    }

    @Test
    void rejectsUnknownCoreCompletionKindValues() {
        assertThrows(IllegalArgumentException.class,
            () -> CompletionKind.fromValue(0));
        assertThrows(IllegalArgumentException.class,
            () -> CompletionKind.fromValue(4));
    }
}
