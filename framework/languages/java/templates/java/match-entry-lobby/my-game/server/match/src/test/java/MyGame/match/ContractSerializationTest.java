package MyGame.match;

import static org.junit.jupiter.api.Assertions.assertTrue;

import MyGame.*;
import com.fasterxml.jackson.databind.JsonNode;
import java.util.List;
import java.util.Comparator;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;

final class ContractSerializationTest {
    @Test
    void defaultTypedSerializerMatchesSharedGoldenJson() throws Exception {
        var serializer = new ZLinkJsonMessageSerializer();
        String[] golden = {
            "{\"playerId\":\"p1\",\"mode\":\"duel\"}",
            "{\"dx\":1,\"dy\":0,\"sequence\":\"7\"}",
            "{\"targetId\":\"p2\",\"sequence\":\"9007199254740993\"}",
            "{\"x\":41,\"y\":10,\"sequence\":\"7\"}",
            "{\"outcome\":\"hit\",\"targetId\":\"p2\",\"sequence\":\"9007199254740993\"}",
            "{\"roomId\":\"room-x\",\"nodeId\":\"matchA\",\"phase\":\"Playing\",\"winner\":null,\"players\":[{\"playerId\":\"p1\",\"x\":41,\"y\":10,\"hp\":75,\"lastSequence\":\"9007199254740993\"}]}",
            "{\"reason\":\"move-budget\",\"retryAfterMs\":120}",
            "{\"winner\":\"p1\"}"
        };
        Object[] samples = {
            new QueueRequest("p1", "duel"),
            new MoveRequest(1, 0, 7),
            new ShootRequest("p2", 9_007_199_254_740_993L),
            new MoveResult(41, 10, 7),
            new ShotResult("hit", "p2", 9_007_199_254_740_993L),
            new RoomView("room-x", "matchA", "Playing", null,
                    List.of(new PlayerView("p1", 41, 10, 75, 9_007_199_254_740_993L))),
            new ActionRejected("move-budget", 120),
            new MatchEnded("p1")
        };
        Comparator<JsonNode> values = (left, right) -> left.isNumber() && right.isNumber()
                ? left.decimalValue().compareTo(right.decimalValue())
                : left.equals(right) ? 0 : 1;
        for (int index = 0; index < samples.length; index++) {
            var expected = Json.MAPPER.readTree(golden[index]);
            var actual = Json.MAPPER.readTree(serializer.serialize(samples[index]).bytes());
            assertTrue(expected.equals(values, actual), "Wire golden: " + samples[index].getClass().getSimpleName());
        }
    }
}
