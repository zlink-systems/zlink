package systems.zlink.framework.runtime.internal.backend;

import static org.junit.jupiter.api.Assertions.*;

import static systems.zlink.framework.errors.ZLinkFrameworkErrorKind.*;
import static systems.zlink.framework.runtime.internal.backend.ZLinkRequestFailureMapping.Context.*;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;

final class ZLinkRequestFailureMappingTest {
    @Test
    void everyStoredAliasPairSatisfiesSchemaAndRejectsAnInvalidTerminal() throws Exception {
        var rowsField = ZLinkRequestFailureMapping.class.getDeclaredField("ROWS");
        rowsField.setAccessible(true);
        for (Object row : (Object[]) rowsField.get(null)) {
            var terminalMethod = row.getClass().getDeclaredMethod("terminal");
            var codeMethod = row.getClass().getDeclaredMethod("code");
            terminalMethod.setAccessible(true);
            codeMethod.setAccessible(true);
            int terminal = (int) terminalMethod.invoke(row);
            int code = (int) codeMethod.invoke(row);
            assertTrue(
                    systems.zlink.framework.runtime.protocol.ServiceWireConstants
                            .validTerminalFailure(terminal, code),
                    "schema alias " + code);
            assertFalse(
                    systems.zlink.framework.runtime.protocol.ServiceWireConstants
                            .validTerminalFailure(code == 0 ? 102 : 0, code),
                    "invalid alias " + code);
        }
    }

    @Test
    void typedContextsPreserveEveryExistingOutgoingKindAlias() {
        ZLinkFrameworkErrorKind[] kinds = {
            NOT_FOUND,
            ALREADY_EXISTS,
            TYPE_MISMATCH,
            REJECTED,
            UNAVAILABLE,
            DEADLINE_EXCEEDED,
            SHUTTING_DOWN,
            PROTOCOL_ERROR,
            INVALID_OPERATION,
            DATA_LOST,
            INTERNAL_FAILURE,
            NOT_CONFIGURED
        };
        int[] actorCodes = {14, 3, 4, 15, 13, 19, 17, 16, 21, 35, 17, 9};
        int[] spotCodes = {14, 3, 7, 15, 13, 19, 17, 16, 33, 35, 17, 9};
        for (int i = 0; i < kinds.length; i++) {
            assertEquals(
                    actorCodes[i],
                    ZLinkRequestFailureMapping.outgoingCode(kinds[i], ACTOR_RELOCATION));
            assertEquals(
                    spotCodes[i],
                    ZLinkRequestFailureMapping.outgoingCode(kinds[i], SPOT_RELOCATION));
            assertArrayEquals(
                    new int[] {107, 21},
                    ZLinkRequestFailureMapping.outgoing(kinds[i], SUPERSEDED_ACTOR_JOIN));
            int[] general = ZLinkRequestFailureMapping.outgoing(kinds[i], GENERAL);
            int[] actorJoin = ZLinkRequestFailureMapping.outgoing(kinds[i], ACTOR_JOIN);
            assertArrayEquals(kinds[i] == TYPE_MISMATCH ? new int[] {107, 4} : general, actorJoin);
        }
        assertEquals(ZLinkFrameworkErrorKind.values().length, kinds.length);
    }

    @Test
    void relocationReceiveAliasesKeepTheirExistingMeaning() {
        int[] codes = {3, 4, 7, 9, 13, 14, 15, 16, 18, 19, 21, 33, 35};
        ZLinkFrameworkErrorKind[] kinds = {
            ALREADY_EXISTS,
            TYPE_MISMATCH,
            TYPE_MISMATCH,
            NOT_CONFIGURED,
            UNAVAILABLE,
            NOT_FOUND,
            REJECTED,
            PROTOCOL_ERROR,
            UNAVAILABLE,
            DEADLINE_EXCEEDED,
            INVALID_OPERATION,
            INVALID_OPERATION,
            DATA_LOST
        };
        for (int i = 0; i < codes.length; i++) {
            assertEquals(kinds[i], ZLinkRequestFailureMapping.incoming(codes[i], ACTOR_RELOCATION));
            assertEquals(kinds[i], ZLinkRequestFailureMapping.incoming(codes[i], SPOT_RELOCATION));
        }
        assertEquals(INTERNAL_FAILURE, ZLinkRequestFailureMapping.incoming(17, ACTOR_RELOCATION));
        for (int code : new int[] {0, 8, 12, 20, 34, 999}) {
            assertNull(ZLinkRequestFailureMapping.incoming(code, ACTOR_RELOCATION));
        }
        assertNull(ZLinkRequestFailureMapping.incoming(999, GENERAL));
    }
}
