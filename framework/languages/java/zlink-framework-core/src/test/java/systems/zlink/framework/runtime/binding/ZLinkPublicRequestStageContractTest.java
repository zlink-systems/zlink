package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.actors.ZLinkActorRequestCall;
import systems.zlink.framework.channels.ZLinkRequestCall;
import systems.zlink.framework.spots.ZLinkSpotRequestCall;

import java.util.concurrent.CompletionStage;

final class ZLinkPublicRequestStageContractTest {
    @Test
    void serverRequestTerminalsExposeAdmissionAlongsideReply() throws Exception {
        Class<?> stage = Class.forName("systems.zlink.framework.channels.ZLinkRequestStage");
        assertTrue(CompletionStage.class.isAssignableFrom(stage));
        assertEquals(CompletionStage.class, stage.getMethod("admitted").getReturnType());
        for (Class<?> call :
                new Class<?>[] {
                    ZLinkRequestCall.class, ZLinkSpotRequestCall.class, ZLinkActorRequestCall.class
                }) {
            assertEquals(stage, call.getMethod("submit", Class.class).getReturnType());
        }
    }
}
