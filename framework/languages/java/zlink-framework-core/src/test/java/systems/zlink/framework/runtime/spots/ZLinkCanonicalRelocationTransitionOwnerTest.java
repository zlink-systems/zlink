package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;

import com.fasterxml.jackson.databind.ObjectMapper;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.HexFormat;
import java.util.concurrent.CompletionException;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;

final class ZLinkCanonicalRelocationTransitionOwnerTest {
    @Test
    void validatedCommandWithoutSemanticOwnerFailsClosed() throws Exception {
        var fixture = new ObjectMapper().readTree(Files.readString(fixture()));
        byte[] prepare = HexFormat.of().parseHex(
            fixture.path("canonical").get(5).path("hex").asText());
        var owner = new ZLinkCanonicalRelocationTransitionOwner(
            ZLinkCanonicalRelocationTransitionOwner.unavailable());

        CompletionException failure = assertThrows(
            CompletionException.class,
            () -> owner.handle(RoutingId.from("node-a"), prepare)
                .toCompletableFuture()
                .join());

        IllegalStateException rejected = assertInstanceOf(
            IllegalStateException.class, failure.getCause());
        assertEquals(
            "canonical relocation command 40 has no production state owner",
            rejected.getMessage());
    }

    @Test
    void malformedCommandNeverReachesSemanticOwner() {
        int[] calls = {0};
        var owner = new ZLinkCanonicalRelocationTransitionOwner(
            (source, command, encoded) -> {
                calls[0]++;
                return java.util.concurrent.CompletableFuture
                    .completedFuture(null);
            });

        assertThrows(
            IllegalArgumentException.class,
            () -> owner.handle(
                RoutingId.from("node-a"),
                new byte[] {0x5a, 0x4d, 1, 39, 0}));
        assertEquals(0, calls[0]);
    }

    private static Path fixture() {
        Path current = Path.of(System.getProperty("user.dir")).toAbsolutePath();
        while (current != null) {
            Path candidate = current.resolve(
                "runtime/protocol/golden/relocation-control-v1.json");
            if (Files.isRegularFile(candidate)) {
                return candidate;
            }
            current = current.getParent();
        }
        throw new IllegalStateException(
            "shared relocation fixture was not found");
    }
}
