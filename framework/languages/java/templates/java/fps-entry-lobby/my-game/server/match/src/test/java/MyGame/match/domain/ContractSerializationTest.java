package MyGame.match.domain;

import static org.junit.jupiter.api.Assertions.*;

import MyGame.*;
import MyGame.Geometry.Vec3;
import com.fasterxml.jackson.databind.JsonNode;
import java.nio.charset.StandardCharsets;
import java.util.Comparator;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;

final class ContractSerializationTest {
    @Test
    void defaultTypedSerializerMatchesGoldenJson() throws Exception {
        var serializer = new ZLinkJsonMessageSerializer();
        String[] golden = {
            "{\"delta\":{\"x\":1,\"y\":0,\"z\":-2},\"sequence\":\"7\"}",
            "{\"outcome\":\"hit\",\"targetId\":null,\"sequence\":\"9007199254740991\"}",
            "{\"roomId\":\"room-x\",\"nodeId\":\"matchA\",\"players\":[{\"playerId\":\"a\",\"position\":{\"x\":48,\"y\":0,\"z\":18},\"hp\":100,\"lastSequence\":\"4\"}],\"phase\":\"Playing\",\"winner\":null}",
            "{\"reason\":\"move-budget\",\"retryAfterMs\":120}"
        };
        Object[] samples = {
            new MoveRequest(new Vec3(1, 0, -2), 7),
            new ShotResult("hit", null, Rules.MAX_SEQUENCE),
            new RoomView("room-x", "matchA", java.util.List.of(
                new PlayerView("a", new Vec3(48, 0, 18), 100, 4)), MatchPhase.Playing, null),
            new ActionRejected("move-budget", 120)
        };
        Comparator<JsonNode> values = (left, right) -> left.isNumber() && right.isNumber()
                ? left.decimalValue().compareTo(right.decimalValue())
                : left.equals(right) ? 0 : 1;
        for (int i = 0; i < samples.length; i++) {
            var expected = Json.MAPPER.readTree(golden[i]);
            var actual = Json.MAPPER.readTree(serializer.serialize(samples[i]).bytes());
            assertTrue(expected.equals(values, actual), "Wire golden: " + samples[i].getClass().getSimpleName());
        }
        assertArrayEquals(new String[] {"Waiting", "Playing", "Finished"},
                java.util.Arrays.stream(MatchPhase.values()).map(Enum::name).toArray(String[]::new));
        for (var phase : MatchPhase.values()) {
            assertEquals("\"" + phase.name() + "\"", new String(serializer.serialize(phase).bytes(), StandardCharsets.UTF_8));
        }
    }
}
