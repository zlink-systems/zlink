package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.Optional;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.runtime.internal.binding.spot.OperationId;
import systems.zlink.framework.runtime.internal.binding.spot.RecordKind;

class ZLinkJavaMeshSpotTest {
    @Test
    void spotSendDoesNotBecomeARequestWhenNativeMetadataContainsAnOperationId() {
        OperationId nativeOperation = new OperationId(7L, 11L);

        assertTrue(ZLinkJavaMeshSpot.requestSequence(RecordKind.SPOT_SEND, nativeOperation).isEmpty());
        assertEquals(
            Optional.of(11L),
            ZLinkJavaMeshSpot.requestSequence(RecordKind.SPOT_REQUEST, nativeOperation));
    }
}
