package systems.zlink.framework.runtime.internal.locations;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.zip.CRC32C;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.locations.*;

/** Stores a logical relocation root as canonical ZLTC chunks and a ZLTM manifest. */
final class ZLinkRelocationTreeStore {
    static final int CHUNK_BYTES = 64 * 1024 * 1024;
    static final int MAX_CHUNKS = 4096;
    static final long MAX_LOGICAL_BYTES = 256L * 1024 * 1024 * 1024;
    private static final int HEADER_BYTES = 11;
    private static final int CHECKSUM_BYTES = 4;
    private static final int MAX_REFERENCE_BYTES = 4096;
    private static final Duration RENEW_THRESHOLD = Duration.ofHours(12);
    private static final byte[] CHUNK_MAGIC = {'Z', 'L', 'T', 'C'};
    private static final byte[] MANIFEST_MAGIC = {'Z', 'L', 'T', 'M'};

    private ZLinkRelocationTreeStore() {
    }

    static CompletionStage<Stored> put(
        ZLinkRelocationStore store,
        byte[] logicalRoot,
        byte[] inventoryDigest,
        Duration retention,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(store, "store");
        byte[] logical = Objects.requireNonNull(logicalRoot, "logicalRoot").clone();
        byte[] digest = Objects.requireNonNull(
            inventoryDigest,
            "inventoryDigest").clone();
        if (logical.length == 0 || digest.length != 32) {
            return failed(new DataLostException(
                "relocation logical root and SHA-256 inventory are required"));
        }
        int count = chunkCount(logical.length);
        List<Chunk> chunks = new ArrayList<>(count);
        CompletionStage<Void> chain = CompletableFuture.completedFuture(null);
        CRC32C logicalChecksum = new CRC32C();
        for (int order = 0; order < count; order++) {
            int offset = order * CHUNK_BYTES;
            int length = Math.min(CHUNK_BYTES, logical.length - offset);
            byte[] data = Arrays.copyOfRange(logical, offset, offset + length);
            logicalChecksum.update(data);
            int chunkOrder = order;
            chain = chain.thenCompose(ignored -> {
                byte[] encoded = encodeChunk(chunkOrder, data);
                return putVerified(store, encoded, retention, cancellation)
                    .thenAccept(stored -> chunks.add(new Chunk(
                        chunkOrder,
                        stored.reference(),
                        data.length,
                        crc32c(data))));
            });
        }
        long checksum = logicalChecksum.getValue();
        return chain.thenCompose(ignored -> {
            byte[] manifest = encodeManifest(
                logical.length,
                checksum,
                digest,
                chunks);
            return putVerified(store, manifest, retention, cancellation)
                .thenApply(root -> new Stored(
                    root,
                    logical.length,
                    checksum,
                    chunks.size()));
        });
    }

    static CompletionStage<Read> read(
        ZLinkRelocationStore store,
        String rootReference,
        long expectedRootChecksum,
        ZLinkStoreCancellation cancellation) {
        return store.get(rootReference, cancellation).thenCompose(rootRead -> {
            if (!(rootRead instanceof ZLinkRelocationFound found)
                || crc32c(found.payload()) != expectedRootChecksum) {
                return failed(new DataLostException(
                    "relocation manifest is missing or corrupt"));
            }
            Manifest manifest;
            try {
                manifest = decodeManifest(found.payload());
            } catch (RuntimeException failure) {
                return failed(failure);
            }
            ByteArrayOutputStream logical = new ByteArrayOutputStream(
                Math.toIntExact(manifest.logicalLength()));
            CompletionStage<Void> chain = CompletableFuture.completedFuture(null);
            for (Chunk chunk : manifest.chunks()) {
                chain = chain.thenCompose(ignored ->
                    store.get(chunk.reference(), cancellation)
                        .thenCompose(chunkRead -> {
                            if (!(chunkRead instanceof ZLinkRelocationFound value)) {
                                return failed(new DataLostException(
                                    "relocation chunk is missing: "
                                        + chunk.reference()));
                            }
                            byte[] data;
                            try {
                                data = decodeChunk(value.payload(), chunk);
                            } catch (RuntimeException failure) {
                                return failed(failure);
                            }
                            logical.writeBytes(data);
                            return CompletableFuture.completedFuture(null);
                        }));
            }
            return chain.thenCompose(ignored -> {
                byte[] bytes = logical.toByteArray();
                if (bytes.length != manifest.logicalLength()
                    || crc32c(bytes) != manifest.logicalChecksum()) {
                    return failed(new DataLostException(
                        "relocation logical stream checksum is invalid"));
                }
                return CompletableFuture.completedFuture(new Read(
                    bytes,
                    manifest.inventoryDigest(),
                    manifest.logicalChecksum(),
                    manifest.chunks().size()));
            });
        });
    }

    static CompletionStage<Void> renew(
        ZLinkRelocationStore store,
        String rootReference,
        long expectedRootChecksum,
        Duration retention,
        ZLinkStoreCancellation cancellation) {
        return store.get(rootReference, cancellation).thenCompose(rootRead -> {
            if (!(rootRead instanceof ZLinkRelocationFound found)
                || crc32c(found.payload()) != expectedRootChecksum) {
                return failed(new DataLostException(
                    "relocation manifest cannot be renewed"));
            }
            Manifest manifest = decodeManifest(found.payload());
            CompletionStage<Void> chain = CompletableFuture.completedFuture(null);
            for (Chunk chunk : manifest.chunks()) {
                chain = chain.thenCompose(ignored ->
                    store.get(chunk.reference(), cancellation)
                        .thenCompose(read -> {
                            if (!(read instanceof ZLinkRelocationFound value)) {
                                return failed(new DataLostException(
                                    "relocation chunk cannot be renewed"));
                            }
                            decodeChunk(value.payload(), chunk);
                            return renewComponent(
                                store,
                                chunk.reference(),
                                retention,
                                cancellation);
                        }));
            }
            return chain.thenCompose(ignored -> renewComponent(
                store,
                rootReference,
                retention,
                cancellation));
        });
    }

    static CompletionStage<Void> delete(
        ZLinkRelocationStore store,
        String rootReference,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(store, "store");
        Objects.requireNonNull(rootReference, "rootReference");
        Objects.requireNonNull(cancellation, "cancellation");
        // Chunk references are content-addressed and may be shared by another
        // published manifest. Removing only the manifest makes its root
        // unreachable; provider retention later removes unreferenced chunks.
        return store.delete(rootReference, cancellation)
            .handle((value, failure) -> null);
    }

    private static CompletionStage<ZLinkRelocationStored> putVerified(
        ZLinkRelocationStore store,
        byte[] encoded,
        Duration retention,
        ZLinkStoreCancellation cancellation) {
        return store.put(encoded, retention, cancellation).thenCompose(stored -> {
            if (stored.reference() == null || stored.reference().isBlank()
                || stored.checksumCrc32c() != crc32c(encoded)
                || Duration.between(stored.storeNow(), stored.expiresAt())
                    .compareTo(RENEW_THRESHOLD) <= 0) {
                return failed(new DataLostException(
                    "Relocation Store returned an invalid immutable reference"));
            }
            return store.get(stored.reference(), cancellation)
                .thenCompose(read -> read instanceof ZLinkRelocationFound found
                        && Arrays.equals(found.payload(), encoded)
                    ? CompletableFuture.completedFuture(stored)
                    : failed(new DataLostException(
                        "Relocation Store did not preserve immutable bytes")));
        });
    }

    private static CompletionStage<Void> renewComponent(
        ZLinkRelocationStore store,
        String reference,
        Duration retention,
        ZLinkStoreCancellation cancellation) {
        return store.renew(reference, retention, cancellation)
            .thenCompose(result -> result instanceof ZLinkRelocationRenewed renewed
                    && Duration.between(renewed.storeNow(), renewed.expiresAt())
                        .compareTo(RENEW_THRESHOLD) > 0
                ? CompletableFuture.completedFuture(null)
                : failed(new DataLostException(
                    "relocation tree component renewal failed")));
    }

    private static byte[] encodeChunk(int order, byte[] data) {
        ByteBuffer body = ByteBuffer.allocate(8 + data.length)
            .order(ByteOrder.BIG_ENDIAN)
            .putInt(order)
            .putInt(data.length)
            .put(data);
        return frame(CHUNK_MAGIC, body.array());
    }

    private static byte[] encodeManifest(
        long logicalLength,
        long logicalChecksum,
        byte[] digest,
        List<Chunk> chunks) {
        int bodyLength = 1 + 8 + 4 + 1 + 32 + 4;
        List<byte[]> references = new ArrayList<>(chunks.size());
        for (Chunk chunk : chunks) {
            byte[] reference = chunk.reference().getBytes(StandardCharsets.UTF_8);
            if (reference.length < 1 || reference.length > MAX_REFERENCE_BYTES) {
                throw new DataLostException(
                    "relocation reference exceeds its UTF-8 bound");
            }
            references.add(reference);
            bodyLength = Math.addExact(bodyLength, 4 + 2 + reference.length + 8 + 4);
        }
        ByteBuffer body = ByteBuffer.allocate(bodyLength)
            .order(ByteOrder.BIG_ENDIAN)
            .put((byte) 1)
            .putLong(logicalLength)
            .putInt((int) logicalChecksum)
            .put((byte) 32)
            .put(digest)
            .putInt(chunks.size());
        for (int index = 0; index < chunks.size(); index++) {
            Chunk chunk = chunks.get(index);
            byte[] reference = references.get(index);
            body.putInt(chunk.order())
                .putShort((short) reference.length)
                .put(reference)
                .putLong(chunk.length())
                .putInt((int) chunk.checksum());
        }
        return frame(MANIFEST_MAGIC, body.array());
    }

    private static byte[] frame(byte[] magic, byte[] body) {
        ByteBuffer frame = ByteBuffer.allocate(
                HEADER_BYTES + body.length + CHECKSUM_BYTES)
            .order(ByteOrder.BIG_ENDIAN)
            .put(magic)
            .put((byte) 1)
            .putShort((short) 0)
            .putInt(body.length)
            .put(body);
        int checksum = (int) crc32c(
            Arrays.copyOf(frame.array(), frame.position()));
        frame.putInt(checksum);
        return frame.array();
    }

    private static Manifest decodeManifest(byte[] encoded) {
        ByteBuffer body = decodeFrame(encoded, MANIFEST_MAGIC);
        if (Byte.toUnsignedInt(body.get()) != 1) {
            throw new DataLostException("invalid relocation manifest version");
        }
        long logicalLength = body.getLong();
        long logicalChecksum = Integer.toUnsignedLong(body.getInt());
        if (Byte.toUnsignedInt(body.get()) != 32) {
            throw new DataLostException("invalid relocation inventory digest");
        }
        byte[] digest = take(body, 32);
        long count = Integer.toUnsignedLong(body.getInt());
        if (logicalLength <= 0 || logicalLength > MAX_LOGICAL_BYTES
            || count < 1 || count > MAX_CHUNKS) {
            throw new DataLostException("relocation manifest bounds are invalid");
        }
        List<Chunk> chunks = new ArrayList<>((int) count);
        long total = 0;
        for (int index = 0; index < count; index++) {
            long order = Integer.toUnsignedLong(body.getInt());
            int referenceLength = Short.toUnsignedInt(body.getShort());
            if (order != index || referenceLength < 1
                || referenceLength > MAX_REFERENCE_BYTES) {
                throw new DataLostException("relocation chunk order is invalid");
            }
            String reference = strictUtf8(take(body, referenceLength));
            long length = body.getLong();
            long checksum = Integer.toUnsignedLong(body.getInt());
            if (length <= 0 || length > CHUNK_BYTES
                || index < count - 1 && length != CHUNK_BYTES) {
                throw new DataLostException("relocation chunk length is invalid");
            }
            total = Math.addExact(total, length);
            chunks.add(new Chunk(index, reference, length, checksum));
        }
        if (body.hasRemaining() || total != logicalLength) {
            throw new DataLostException("relocation manifest length is invalid");
        }
        return new Manifest(logicalLength, logicalChecksum, digest, chunks);
    }

    private static byte[] decodeChunk(byte[] encoded, Chunk expected) {
        ByteBuffer body = decodeFrame(encoded, CHUNK_MAGIC);
        long order = Integer.toUnsignedLong(body.getInt());
        long length = Integer.toUnsignedLong(body.getInt());
        if (order != expected.order() || length != expected.length()
            || length <= 0 || length > CHUNK_BYTES
            || body.remaining() != length) {
            throw new DataLostException("relocation chunk envelope is invalid");
        }
        byte[] data = take(body, (int) length);
        if (crc32c(data) != expected.checksum()) {
            throw new DataLostException("relocation chunk checksum is invalid");
        }
        return data;
    }

    private static ByteBuffer decodeFrame(byte[] encoded, byte[] magic) {
        if (encoded.length < HEADER_BYTES + CHECKSUM_BYTES
            || !Arrays.equals(Arrays.copyOf(encoded, 4), magic)) {
            throw new DataLostException("relocation frame header is invalid");
        }
        ByteBuffer input = ByteBuffer.wrap(encoded).order(ByteOrder.BIG_ENDIAN);
        input.position(4);
        int version = Byte.toUnsignedInt(input.get());
        int reserved = Short.toUnsignedInt(input.getShort());
        long bodyLength = Integer.toUnsignedLong(input.getInt());
        long expectedLength = HEADER_BYTES + bodyLength + CHECKSUM_BYTES;
        long expectedChecksum = Integer.toUnsignedLong(
            ByteBuffer.wrap(encoded, encoded.length - 4, 4).getInt());
        if (version != 1 || reserved != 0 || expectedLength != encoded.length
            || crc32c(Arrays.copyOf(encoded, encoded.length - 4))
                != expectedChecksum) {
            throw new DataLostException(
                "relocation frame length or checksum is invalid");
        }
        return ByteBuffer.wrap(encoded, HEADER_BYTES, (int) bodyLength)
            .slice().order(ByteOrder.BIG_ENDIAN);
    }

    private static byte[] take(ByteBuffer input, int length) {
        if (length < 0 || length > input.remaining()) {
            throw new DataLostException("relocation frame is truncated");
        }
        byte[] value = new byte[length];
        input.get(value);
        return value;
    }

    private static String strictUtf8(byte[] bytes) {
        try {
            return StandardCharsets.UTF_8.newDecoder()
                .onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT)
                .decode(ByteBuffer.wrap(bytes)).toString();
        } catch (CharacterCodingException failure) {
            throw new DataLostException("relocation reference is not UTF-8");
        }
    }

    private static int chunkCount(long length) {
        long count = (length + CHUNK_BYTES - 1) / CHUNK_BYTES;
        if (count < 1 || count > MAX_CHUNKS) {
            throw new DataLostException("relocation logical stream is too large");
        }
        return (int) count;
    }

    private static long crc32c(byte[] bytes) {
        CRC32C checksum = new CRC32C();
        checksum.update(bytes);
        return checksum.getValue();
    }

    private static <T> CompletionStage<T> failed(Throwable failure) {
        return CompletableFuture.failedFuture(failure);
    }

    record Stored(
        ZLinkRelocationStored root,
        long logicalLength,
        long logicalChecksum,
        int chunkCount) {
    }

    record Read(
        byte[] logicalRoot,
        byte[] inventoryDigest,
        long logicalChecksum,
        int chunkCount) {
        Read {
            logicalRoot = logicalRoot.clone();
            inventoryDigest = inventoryDigest.clone();
        }

        @Override public byte[] logicalRoot() { return logicalRoot.clone(); }
        @Override public byte[] inventoryDigest() { return inventoryDigest.clone(); }
    }

    static final class DataLostException extends RuntimeException {
        DataLostException(String message) {
            super(message);
        }
    }

    private record Manifest(
        long logicalLength,
        long logicalChecksum,
        byte[] inventoryDigest,
        List<Chunk> chunks) {
        Manifest {
            inventoryDigest = inventoryDigest.clone();
            chunks = List.copyOf(chunks);
        }

        @Override public byte[] inventoryDigest() { return inventoryDigest.clone(); }
    }

    private record Chunk(int order, String reference, long length, long checksum) {
    }
}
