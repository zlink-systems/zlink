package systems.zlink.contract;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;
import systems.zlink.contracts.sockets.RequestResult;

class RequestResultMappingContractTest {
    @Test
    void mapsEveryCoreRequestResultValue() {
        int[] values = {
            0,
            101,
            102,
            103,
            104,
            105,
            106,
            107,
            108,
            109,
            110,
            111,
            112,
            113
        };

        for (int value : values) {
            assertEquals(value, RequestResult.fromValue(value).value());
        }
        assertEquals(RequestResult.BACKPRESSURED, RequestResult.fromValue(113));
    }

    @Test
    void rejectsValuesOutsideTheCoreEnum() {
        assertThrows(IllegalArgumentException.class, () -> RequestResult.fromValue(100));
        assertThrows(IllegalArgumentException.class, () -> RequestResult.fromValue(114));
    }
}
