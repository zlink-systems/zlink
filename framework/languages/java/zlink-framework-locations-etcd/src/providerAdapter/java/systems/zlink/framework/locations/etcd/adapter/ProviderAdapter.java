package systems.zlink.framework.locations.etcd.adapter;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ObjectNode;

import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.locations.etcd.*;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicBoolean;

/** JSONL conversion for the public provider SPI; case decisions belong to the common runner. */
public final class ProviderAdapter {
    private static final ObjectMapper JSON = new ObjectMapper();
    private final Map<String, ZLinkEtcdLocationStore> stores = new ConcurrentHashMap<>();
    private final Map<String, Cancellation> operations = new ConcurrentHashMap<>();
    private final Set<String> seen = new HashSet<>();
    private final PrintWriter output =
            new PrintWriter(new OutputStreamWriter(System.out, StandardCharsets.UTF_8));

    private static final class Cancellation implements ZLinkStoreCancellation {
        final AtomicBoolean requested = new AtomicBoolean();

        @Override
        public boolean isCancellationRequested() {
            return requested.get();
        }
    }

    private static String text(JsonNode node, String field) {
        JsonNode value = node.get(field);
        if (value == null || !value.isTextual())
            throw new IllegalArgumentException("Expected string: " + field);
        return value.textValue();
    }

    private static long decimal(JsonNode node) {
        if (!node.isTextual() || !node.textValue().matches("-?(0|[1-9][0-9]*)"))
            throw new IllegalArgumentException("Expected integer decimal milliseconds.");
        return Long.parseLong(node.textValue());
    }

    private static long integer(JsonNode node) {
        if (node == null || !node.isIntegralNumber() || !node.canConvertToLong())
            throw new IllegalArgumentException("Expected JSON integer within signed 64-bit range.");
        return node.longValue();
    }

    private static byte[] bytes(JsonNode node) {
        if (node == null || !node.isTextual())
            throw new IllegalArgumentException("Expected canonical base64.");
        byte[] decoded = Base64.getDecoder().decode(node.textValue());
        if (!Base64.getEncoder().encodeToString(decoded).equals(node.textValue()))
            throw new IllegalArgumentException("Expected canonical base64.");
        return decoded;
    }

    private static ObjectNode kind(String value) {
        return JSON.createObjectNode().put("kind", value);
    }

    private static ObjectNode value(ZLinkStoreValue value) {
        ObjectNode result =
                JSON.createObjectNode()
                        .put("bytesBase64", Base64.getEncoder().encodeToString(value.bytes()))
                        .put("version", value.version().value())
                        .put("storeNowMs", Long.toString(value.storeNow().toEpochMilli()));
        if (value.expiresAt() == null) result.putNull("expiresAtMs");
        else result.put("expiresAtMs", Long.toString(value.expiresAt().toEpochMilli()));
        return result;
    }

    private static ObjectNode result(Object value) {
        return switch (value) {
            case ZLinkStoreReadFound found -> kind("found").set("value", value(found.value()));
            case ZLinkStoreReadMissing missing ->
                    kind("missing")
                            .put("storeNowMs", Long.toString(missing.storeNow().toEpochMilli()));
            case ZLinkStoreWriteConflict conflict ->
                    kind("conflict")
                            .put("storeNowMs", Long.toString(conflict.storeNow().toEpochMilli()));
            case ZLinkStoreWriteApplied applied -> {
                ObjectNode result =
                        kind("applied")
                                .put(
                                        "storeNowMs",
                                        Long.toString(applied.storeNow().toEpochMilli()));
                var versions = result.putArray("putVersions");
                applied.putVersions()
                        .forEach(
                                (key, version) ->
                                        versions.addObject()
                                                .put("key", key.value())
                                                .put("version", version.value()));
                yield result;
            }
            case ZLinkStoreScanExpired ignored -> kind("expired");
            case ZLinkStoreScanPageResult page -> {
                ObjectNode result = kind("page");
                ObjectNode body =
                        result.putObject("value")
                                .put(
                                        "storeNowMs",
                                        Long.toString(page.value().storeNow().toEpochMilli()));
                if (page.value().nextCursor() == null) body.putNull("nextCursor");
                else body.put("nextCursor", page.value().nextCursor().value());
                var items = body.putArray("items");
                page.value()
                        .items()
                        .forEach(
                                item ->
                                        items.addObject()
                                                .put("key", item.key().value())
                                                .set("value", value(item.value())));
                yield result;
            }
            default -> throw new IllegalArgumentException("Unknown public result.");
        };
    }

    private static ZLinkEtcdLocationOptions options(JsonNode input) {
        List<String> endpoints = new ArrayList<>();
        for (JsonNode endpoint : input.required("endpoints")) {
            if (!endpoint.isTextual())
                throw new IllegalArgumentException("Expected endpoint string.");
            endpoints.add(endpoint.textValue());
        }
        var options =
                new ZLinkEtcdLocationOptions()
                        .setEndpoints(endpoints)
                        .setNamespace(text(input, "namespace"));
        if (input.has("operationTimeoutMs"))
            options.setOperationTimeout(
                    Duration.ofMillis(integer(input.required("operationTimeoutMs"))));
        if (input.hasNonNull("credentials")) {
            JsonNode credentials = input.get("credentials");
            options.setCredentials(
                    new ZLinkEtcdCredentials(
                            text(credentials, "user"), text(credentials, "password")));
        }
        if (input.hasNonNull("tls")) {
            JsonNode tls = input.get("tls");
            var nativeTls = new ZLinkEtcdTlsOptions();
            if (tls.hasNonNull("trustedCaPem"))
                nativeTls.setTrustedCaPem(text(tls, "trustedCaPem"));
            if (tls.hasNonNull("clientIdentity")) {
                JsonNode identity = tls.get("clientIdentity");
                nativeTls.setClientIdentity(
                        new ZLinkEtcdClientIdentity(
                                text(identity, "certificatePem"), text(identity, "keyPem")));
            }
            options.setTls(nativeTls);
        }
        return options;
    }

    private static ZLinkStoreWriteRequest writeRequest(JsonNode input) {
        List<ZLinkStoreCondition> conditions = new ArrayList<>();
        for (JsonNode condition : input.required("conditions")) {
            ZLinkStoreKey key = new ZLinkStoreKey(text(condition, "key"));
            conditions.add(
                    switch (text(condition, "kind")) {
                        case "missing" -> new ZLinkStoreMissingCondition(key);
                        case "version" ->
                                new ZLinkStoreVersionCondition(
                                        key, new ZLinkStoreVersion(text(condition, "version")));
                        case "value" ->
                                new ZLinkStoreValueCondition(
                                        key, bytes(condition.get("bytesBase64")));
                        default -> throw new IllegalArgumentException("Unknown condition.");
                    });
        }
        List<ZLinkStoreMutation> mutations = new ArrayList<>();
        for (JsonNode mutation : input.required("mutations")) {
            ZLinkStoreKey key = new ZLinkStoreKey(text(mutation, "key"));
            mutations.add(
                    switch (text(mutation, "kind")) {
                        case "delete" -> new ZLinkStoreDelete(key);
                        case "put" ->
                                new ZLinkStorePut(
                                        key,
                                        bytes(mutation.get("bytesBase64")),
                                        mutation.has("retentionMs")
                                                ? Duration.ofMillis(
                                                        decimal(mutation.get("retentionMs")))
                                                : null);
                        default -> throw new IllegalArgumentException("Unknown mutation.");
                    });
        }
        return new ZLinkStoreWriteRequest(conditions, mutations);
    }

    private synchronized void reply(String id, ObjectNode body) {
        ObjectNode response = JSON.createObjectNode().put("v", 1);
        if (id == null) response.putNull("id");
        else response.put("id", id);
        response.setAll(body);
        output.println(response);
        output.flush();
    }

    private void success(String id, ObjectNode result) {
        reply(id, JSON.createObjectNode().put("ok", true).set("result", result));
    }

    private void failure(String id, String op, Throwable error, boolean provider) {
        while ((error instanceof CompletionException || error instanceof ExecutionException)
                && error.getCause() != null) error = error.getCause();
        String category =
                !provider
                        ? "adapter-error"
                        : error instanceof IllegalArgumentException
                                ? "argument-error"
                                : error instanceof IOException
                                                || error instanceof CancellationException
                                        ? "store-failure"
                                        : "adapter-error";
        ObjectNode body = JSON.createObjectNode().put("ok", false);
        ObjectNode detail =
                body.putObject("error")
                        .put("category", category)
                        .put("nativeType", error.getClass().getName())
                        .put("message", error.getMessage());
        // The provider cancels without I/O only before the first RPC.
        if (category.equals("store-failure"))
            detail.put(
                    "commit",
                    op.equals("write") && !(error instanceof CancellationException)
                            ? "unknown"
                            : "none");
        reply(id, body);
    }

    private CompletableFuture<Void> dispatch(JsonNode message) {
        String id = null, op = "";
        boolean provider = false;
        try {
            id = text(message, "id");
            if (integer(message.required("v")) != 1 || !seen.add(id))
                throw new IllegalArgumentException("Invalid version or duplicate request id.");
            op = text(message, "op");
            if (op.equals("cancel")) {
                Cancellation cancellation = operations.get(text(message, "target"));
                if (cancellation != null) cancellation.requested.set(true);
                success(id, kind("cancelled").put("active", cancellation != null));
                return CompletableFuture.completedFuture(null);
            }
            String storeId = text(message, "store");
            if (op.equals("create")) {
                if (stores.containsKey(storeId))
                    throw new IllegalArgumentException("Duplicate store.");
                var options = options(message.required("options"));
                provider = true;
                stores.put(storeId, new ZLinkEtcdLocationStore(options));
                success(id, kind("created"));
                return CompletableFuture.completedFuture(null);
            }
            ZLinkEtcdLocationStore store = stores.get(storeId);
            if (store == null) throw new IllegalArgumentException("Unknown store.");
            Object request =
                    switch (op) {
                        case "read" -> new ZLinkStoreKey(text(message, "key"));
                        case "write" -> writeRequest(message.required("request"));
                        case "scan" -> {
                            JsonNode input = message.required("request");
                            yield new ZLinkStoreScanRequest(
                                    text(input, "prefix"),
                                    input.hasNonNull("cursor")
                                            ? new ZLinkStoreScanCursor(text(input, "cursor"))
                                            : null,
                                    Math.toIntExact(integer(input.required("limit"))));
                        }
                        case "dispose" -> null;
                        default -> throw new IllegalArgumentException("Unknown operation.");
                    };
            Cancellation cancellation = new Cancellation();
            operations.put(id, cancellation);
            provider = true;
            CompletionStage<?> pending =
                    switch (op) {
                        case "read" -> store.read((ZLinkStoreKey) request, cancellation);
                        case "write" -> store.write((ZLinkStoreWriteRequest) request, cancellation);
                        case "scan" -> store.scan((ZLinkStoreScanRequest) request, cancellation);
                        case "dispose" ->
                                CompletableFuture.runAsync(
                                        () -> {
                                            store.close();
                                            stores.remove(storeId);
                                        });
                        default -> throw new IllegalStateException("Unknown operation.");
                    };
            String responseId = id, operation = op;
            return pending.handle(
                            (nativeResult, error) -> {
                                try {
                                    if (error != null) failure(responseId, operation, error, true);
                                    else {
                                        try {
                                            success(
                                                    responseId,
                                                    operation.equals("dispose")
                                                            ? kind("disposed")
                                                            : result(nativeResult));
                                        } catch (RuntimeException conversion) {
                                            failure(responseId, operation, conversion, false);
                                        }
                                    }
                                } finally {
                                    operations.remove(responseId);
                                }
                                return (Void) null;
                            })
                    .toCompletableFuture();
        } catch (RuntimeException error) {
            if (id != null) operations.remove(id);
            failure(id, op, error, provider);
            return CompletableFuture.completedFuture(null);
        }
    }

    private void run() throws IOException {
        List<CompletableFuture<Void>> pending = new ArrayList<>();
        try (BufferedReader input =
                new BufferedReader(new InputStreamReader(System.in, StandardCharsets.UTF_8))) {
            String line;
            while ((line = input.readLine()) != null) {
                try {
                    pending.add(dispatch(JSON.readTree(line)));
                } catch (IOException | RuntimeException error) {
                    failure(null, "", error, false);
                }
            }
            CompletableFuture.allOf(pending.toArray(CompletableFuture[]::new)).join();
        } finally {
            stores.values().forEach(ZLinkEtcdLocationStore::close);
        }
    }

    public static void main(String[] args) throws IOException {
        new ProviderAdapter().run();
    }
}
