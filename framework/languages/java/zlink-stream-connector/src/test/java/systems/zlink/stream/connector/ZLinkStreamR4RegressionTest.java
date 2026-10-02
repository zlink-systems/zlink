package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import java.net.URI;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.TimeoutException;

final class ZLinkStreamR4RegressionTest {
    @Test
    void packetNamesFollowTheSharedUnicodeWhiteSpaceFixture() throws Exception {
        Path root = Path.of("").toAbsolutePath();
        while (!Files.isDirectory(root.resolve("framework/test/fixtures"))) {
            root = root.getParent();
            if (root == null) {
                throw new IllegalStateException("shared stream packet-name fixture was not found");
            }
        }
        ZLinkStreamConnector connector =
                ZLinkStreamConnectorFactory.create(
                        ZLinkStreamConnectorOptions.createDefault(URI.create("tcp://127.0.0.1:1")));
        try {
            int cases = 0;
            for (String line :
                    Files.readAllLines(
                            root.resolve(
                                    "framework/test/fixtures/stream-packet-name-whitespace.tsv"))) {
                if (line.startsWith("#") || line.isEmpty()) {
                    continue;
                }
                String[] fields = line.split("\t");
                assertTrue(fields.length == 2, line);
                assertTrue(fields[1].equals("true") || fields[1].equals("false"), line);
                cases++;
                StringBuilder name = new StringBuilder();
                if (!fields[0].equals("EMPTY")) {
                    for (String codePoint : fields[0].split(",")) {
                        name.appendCodePoint(Integer.parseInt(codePoint, 16));
                    }
                }
                if (Boolean.parseBoolean(fields[1])) {
                    assertThrows(
                            IllegalArgumentException.class,
                            () -> ZLinkStreamWireProtocol.validatePacketName(name.toString()),
                            fields[0]);
                    assertEquals(
                            ZLinkStreamErrorCode.VALIDATION_FAILED,
                            assertThrows(
                                            ZLinkStreamException.class,
                                            () ->
                                                    connector.on(
                                                            name.toString(),
                                                            message ->
                                                                    CompletableFuture
                                                                            .completedFuture(null)),
                                            fields[0])
                                    .errorCode());
                } else {
                    assertDoesNotThrow(
                            () -> ZLinkStreamWireProtocol.validatePacketName(name.toString()),
                            fields[0]);
                    try (AutoCloseable registration =
                            connector.on(
                                    name.toString(),
                                    message -> CompletableFuture.completedFuture(null))) {
                        // Registration succeeds without establishing a transport connection.
                    }
                }
            }
            assertTrue(cases > 0, "shared fixture must contain packet-name cases");
        } finally {
            ConnectorTestAwait.await(connector.close());
        }
    }

    @Test
    void assertionHelpersPreserveUncodedTimeouts() {
        for (String message : new String[] {"connect timed out", "request timed out"}) {
            TimeoutException timeout = new TimeoutException(message);
            assertSame(
                    timeout,
                    assertThrows(
                            TimeoutException.class,
                            () ->
                                    ZLinkStreamAssert.expectFailure(
                                            () -> {
                                                throw timeout;
                                            },
                                            null)));
            assertSame(
                    timeout,
                    assertThrows(
                            TimeoutException.class,
                            () ->
                                    ZLinkStreamAssert.expectTimeout(
                                            () -> {
                                                throw timeout;
                                            })));
            CompletionException wrapper = new CompletionException(timeout);
            assertSame(
                    wrapper,
                    assertThrows(
                            CompletionException.class,
                            () ->
                                    ZLinkStreamAssert.expectFailure(
                                            () -> {
                                                throw wrapper;
                                            },
                                            null)));
            assertSame(
                    wrapper,
                    assertThrows(
                            CompletionException.class,
                            () ->
                                    ZLinkStreamAssert.expectTimeout(
                                            () -> {
                                                throw wrapper;
                                            })));
        }
    }
}
