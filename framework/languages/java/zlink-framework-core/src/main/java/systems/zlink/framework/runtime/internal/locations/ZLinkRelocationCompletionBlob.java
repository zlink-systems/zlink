package systems.zlink.framework.runtime.internal.locations;

import static systems.zlink.framework.runtime.protocol.ServiceWirePilotCodec.*;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.io.SequenceInputStream;
import java.io.UncheckedIOException;
import java.security.MessageDigest;
import java.time.Duration;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HexFormat;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.zip.CRC32C;

/** Stores completion bytes in the common Relocation Store chunk and manifest format. */
final class ZLinkRelocationCompletionBlob {
    static final long MAX_BYTES = 256L * 1024 * 1024 * 1024;
    private static final int CHUNK_BYTES = 64 * 1024 * 1024;
    private static final Duration RETENTION = Duration.ofHours(24);
    private final ZLinkRelocationStore store;

    ZLinkRelocationCompletionBlob(ZLinkRelocationStore store) {
        this.store = store;
    }

    record Stored(String reference, String sha256) {}

    CompletionStage<Stored> put(
            ZLinkRelocationCompletionRecordCodec.Record record,
            String inventorySha256,
            ZLinkStoreCancellation cancellation) {
        var payload = new Payload();
        ZLinkRelocationCompletionRecordCodec.write(record, payload);
        CompletionStage<List<RelocationChunkEntryV1>> writes =
                CompletableFuture.completedFuture(new ArrayList<>());
        for (int index = 0; index < payload.chunks.size(); index++) {
            int order = index;
            writes =
                    writes.thenCompose(
                            entries -> {
                                byte[] data = payload.chunks.set(order, null);
                                int length = data.length;
                                int checksum = crc32c(data);
                                return putVerified(encodeChunk(order, data), cancellation)
                                        .thenApply(
                                                reference -> {
                                                    entries.add(
                                                            new RelocationChunkEntryV1(
                                                                    order, reference, length,
                                                                    checksum));
                                                    return entries;
                                                });
                            });
        }
        return writes.thenCompose(
                entries -> {
                    var manifest =
                            new RelocationManifestV1(
                                    1,
                                    payload.length,
                                    payload.checksum(),
                                    HexFormat.of().parseHex(inventorySha256),
                                    entries);
                    return putVerified(encodeManifest(manifest), cancellation)
                            .thenApply(reference -> new Stored(reference, payload.sha256()));
                });
    }

    CompletionStage<ZLinkRelocationCompletionRecordCodec.Record> read(
            String reference,
            String expectedSha256,
            String inventorySha256,
            ZLinkStoreCancellation cancellation) {
        return requireBlob(reference, cancellation)
                .thenCompose(
                        bytes -> {
                            var manifest = decodeManifest(bytes);
                            if (manifest.chunks().isEmpty()
                                    || manifest.totalLength() <= 0
                                    || manifest.totalLength() > MAX_BYTES
                                    || !Arrays.equals(
                                            manifest.inventoryDigestSha256(),
                                            HexFormat.of().parseHex(inventorySha256))) {
                                throw dataLost("Invalid completion manifest or inventory digest.");
                            }
                            CompletionStage<Payload> reads =
                                    CompletableFuture.completedFuture(new Payload());
                            for (int index = 0; index < manifest.chunks().size(); index++) {
                                var entry = manifest.chunks().get(index);
                                if (entry.order() != index
                                        || entry.length() <= 0
                                        || entry.length() > CHUNK_BYTES
                                        || (index + 1 < manifest.chunks().size()
                                                && entry.length() != CHUNK_BYTES)) {
                                    throw dataLost("Invalid completion chunk order or length.");
                                }
                                reads =
                                        reads.thenCompose(
                                                payload ->
                                                        requireBlob(entry.reference(), cancellation)
                                                                .thenApply(
                                                                        encoded -> {
                                                                            var chunk =
                                                                                    decodeChunk(
                                                                                            encoded);
                                                                            if (chunk.order()
                                                                                            != entry
                                                                                                    .order()
                                                                                    || chunk.data()
                                                                                                    .length
                                                                                            != entry
                                                                                                    .length()
                                                                                    || crc32c(
                                                                                                    chunk
                                                                                                            .data())
                                                                                            != entry
                                                                                                    .checksumCrc32c()) {
                                                                                throw dataLost(
                                                                                        "Completion"
                                                                                                + " chunk"
                                                                                                + " differs"
                                                                                                + " from"
                                                                                                + " manifest.");
                                                                            }
                                                                            payload.add(
                                                                                    chunk.data());
                                                                            return payload;
                                                                        }));
                            }
                            return reads.thenApply(
                                    payload -> {
                                        if (payload.length != manifest.totalLength()
                                                || payload.checksum()
                                                        != manifest.totalChecksumCrc32c()
                                                || !payload.sha256().equals(expectedSha256)) {
                                            throw dataLost(
                                                    "Completion payload length or checksum"
                                                            + " differs.");
                                        }
                                        return ZLinkRelocationCompletionRecordCodec.decode(
                                                () ->
                                                        new SequenceInputStream(
                                                                Collections.enumeration(
                                                                        payload.chunks.stream()
                                                                                .map(
                                                                                        ByteArrayInputStream
                                                                                                ::new)
                                                                                .toList())));
                                    });
                        });
    }

    private CompletionStage<String> putVerified(byte[] bytes, ZLinkStoreCancellation cancellation) {
        return store.put(bytes, RETENTION, cancellation)
                .thenCompose(
                        stored ->
                                requireBlob(stored.reference(), cancellation)
                                        .thenApply(
                                                read -> {
                                                    if (!Arrays.equals(bytes, read)) {
                                                        throw dataLost(
                                                                "Completion blob differs after"
                                                                        + " Put.");
                                                    }
                                                    return stored.reference();
                                                }));
    }

    private CompletionStage<byte[]> requireBlob(
            String reference, ZLinkStoreCancellation cancellation) {
        return store.get(reference, cancellation)
                .thenApply(
                        read -> {
                            if (read instanceof ZLinkRelocationFound found) return found.payload();
                            throw dataLost("Completion blob is missing.");
                        });
    }

    private static byte[] encodeChunk(int order, byte[] data) {
        try {
            return encodeRelocationDataChunkV1(new RelocationDataChunkV1(order, data));
        } catch (IOException failure) {
            throw new UncheckedIOException(failure);
        }
    }

    private static byte[] encodeManifest(RelocationManifestV1 manifest) {
        try {
            return encodeRelocationManifestV1(manifest);
        } catch (IOException failure) {
            throw new UncheckedIOException(failure);
        }
    }

    private static RelocationManifestV1 decodeManifest(byte[] bytes) {
        try {
            return decodeRelocationManifestV1(bytes);
        } catch (IOException failure) {
            throw dataLost("Invalid completion manifest.");
        }
    }

    private static RelocationDataChunkV1 decodeChunk(byte[] bytes) {
        try {
            return decodeRelocationDataChunkV1(bytes);
        } catch (IOException failure) {
            throw dataLost("Invalid completion chunk.");
        }
    }

    private static int crc32c(byte[] bytes) {
        var crc = new CRC32C();
        crc.update(bytes);
        return (int) crc.getValue();
    }

    private static ZLinkAggregateRelocationCoordinator.RelocationDataLostException dataLost(
            String message) {
        return new ZLinkAggregateRelocationCoordinator.RelocationDataLostException(message);
    }

    private static final class Payload extends OutputStream {
        private final List<byte[]> chunks = new ArrayList<>();
        private final CRC32C crc = new CRC32C();
        private final MessageDigest digest = ZLinkRelocationCompletionRecordCodec.sha256();
        private ByteArrayOutputStream current = new ByteArrayOutputStream();
        private long length;

        void add(byte[] chunk) {
            chunks.add(chunk);
            length += chunk.length;
            crc.update(chunk);
            digest.update(chunk);
        }

        int checksum() {
            return (int) crc.getValue();
        }

        String sha256() {
            return HexFormat.of().formatHex(digest.digest());
        }

        @Override
        public void write(int value) {
            current.write(value);
            if (current.size() == CHUNK_BYTES) close();
        }

        @Override
        public void write(byte[] bytes, int offset, int count) {
            while (count > 0) {
                int size = Math.min(count, CHUNK_BYTES - current.size());
                current.write(bytes, offset, size);
                offset += size;
                count -= size;
                if (current.size() == CHUNK_BYTES) close();
            }
        }

        @Override
        public void close() {
            if (current.size() == 0) return;
            add(current.toByteArray());
            current = new ByteArrayOutputStream();
        }
    }
}
