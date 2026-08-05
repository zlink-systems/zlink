package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.assertEquals;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.HexFormat;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;

final class JavaNodeStreamInteropTest {
    @Test
    void nodeConnector_decodesJavaRequestFrame_andJavaDecodesNodeResponse() throws Exception {
        Map<String, String> metadata = new LinkedHashMap<>();
        metadata.put("trace", "java-node-1");
        byte[] requestHeader = ZLinkStreamWireProtocol.encodeHeader(new ZLinkStreamWireProtocol.Header(
            ZLinkStreamWireProtocol.KIND_REQUEST,
            ZLinkStreamWireProtocol.CODEC_JSON,
            ZLinkStreamWireProtocol.FLAG_HAS_REQUEST_SEQ
                | ZLinkStreamWireProtocol.FLAG_HAS_METADATA,
            42L,
            "Join",
            metadata,
            null));
        byte[] requestPayload = "{\"join\":true}".getBytes(StandardCharsets.UTF_8);
        byte[] requestFrame = ZLinkStreamWireProtocol.encodeFrame(requestHeader, requestPayload, 64 * 1024);

        byte[] responseFrame = runNodeInterop(requestFrame);
        ZLinkStreamWireProtocol.Frame decodedFrame = ZLinkStreamWireProtocol.decodeFrame(responseFrame);
        ZLinkStreamWireProtocol.Header decodedHeader =
            ZLinkStreamWireProtocol.decodeHeader(decodedFrame.header());

        assertEquals(ZLinkStreamWireProtocol.KIND_RESPONSE, decodedHeader.kind());
        assertEquals(ZLinkStreamWireProtocol.CODEC_JSON, decodedHeader.codec());
        assertEquals(42L, decodedHeader.requestSeq());
        assertEquals("", decodedHeader.name());
        assertEquals("node", decodedHeader.metadata().get("source"));
        assertEquals("{\"accepted\":true}", new String(decodedFrame.payload(), StandardCharsets.UTF_8));
    }

    private static byte[] runNodeInterop(byte[] requestFrame) throws Exception {
        Path connectorDist = nodeConnectorDist();
        Process process = new ProcessBuilder(
                "node",
                "-e",
                nodeScript(),
                connectorDist.toString(),
                HexFormat.of().formatHex(requestFrame))
            .redirectErrorStream(true)
            .start();
        boolean exited = process.waitFor(Duration.ofSeconds(10).toMillis(), TimeUnit.MILLISECONDS);
        if (!exited) {
            process.destroyForcibly();
            process.waitFor(1, TimeUnit.SECONDS);
            throw new AssertionError("node interop process did not exit within 10s");
        }
        String output = new String(process.getInputStream().readAllBytes(), StandardCharsets.UTF_8).trim();
        if (process.exitValue() != 0) {
            throw new AssertionError("node interop process failed: " + output);
        }
        return HexFormat.of().parseHex(output);
    }

    private static Path nodeConnectorDist() throws IOException {
        Path current = Path.of("").toAbsolutePath();
        for (Path candidate = current; candidate != null; candidate = candidate.getParent()) {
            Path dist = candidate.resolve("framework/languages/node/packages/stream-connector/dist");
            if (Files.isRegularFile(dist.resolve("index.js"))) {
                return dist;
            }
            Path siblingDist = candidate.resolve("../node/packages/stream-connector/dist").normalize();
            if (Files.isRegularFile(siblingDist.resolve("index.js"))) {
                return siblingDist;
            }
        }
        throw new AssertionError("Node stream connector dist not found");
    }

    private static String nodeScript() {
        return """
            const path = require('node:path');
            const connector = require(process.argv[1]);
            const { ZlinkStreamFrameCodec } = require(
              path.join(process.argv[1], 'Runtime/Protocol/ZlinkStreamFrameCodec.js'));
            const { ZlinkStreamHeaderCodec } = require(
              path.join(process.argv[1], 'Runtime/Protocol/ZlinkStreamHeaderCodec.js'));
            const requestFrame = Buffer.from(process.argv[2], 'hex');
            const decodedFrame = ZlinkStreamFrameCodec.decode(requestFrame);
            const decodedHeader = ZlinkStreamHeaderCodec.decode(decodedFrame.header);
            if (decodedHeader.kind !== connector.ZlinkStreamMessageKind.Request ||
                decodedHeader.codec !== connector.ZlinkStreamCodec.Json ||
                decodedHeader.requestSeq !== 42n ||
                decodedHeader.name !== 'Join' ||
                decodedHeader.metadata.get('trace') !== 'java-node-1' ||
                Buffer.from(decodedFrame.payload).toString('utf8') !== '{"join":true}') {
              throw new Error('Node failed to decode Java request frame');
            }
            const responseHeader = ZlinkStreamHeaderCodec.encode({
              kind: connector.ZlinkStreamMessageKind.Response,
              codec: connector.ZlinkStreamCodec.Json,
              flags: connector.ZlinkStreamHeaderFlags.HasRequestSeq,
              requestSeq: decodedHeader.requestSeq,
              name: 'JoinAccepted',
              metadata: connector.ZlinkStreamMetadataMap.empty.with('source', 'node')
            });
            const responseFrame = ZlinkStreamFrameCodec.encode(
              responseHeader,
              new TextEncoder().encode('{"accepted":true}'));
            process.stdout.write(Buffer.from(responseFrame).toString('hex'));
            """;
    }
}
