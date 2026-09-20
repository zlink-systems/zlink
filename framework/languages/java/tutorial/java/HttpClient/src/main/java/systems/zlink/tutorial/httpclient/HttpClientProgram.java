package systems.zlink.tutorial.httpclient;

import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.httpclient.HttpResponse;
import systems.zlink.httpclient.RawHttpResponse;
import systems.zlink.httpclient.ZLinkHttpClient;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.concurrent.CompletionException;
import java.util.concurrent.atomic.AtomicInteger;

public final class HttpClientProgram {

    private HttpClientProgram() {}

    public static void main(String[] args) {
        // --8<-- [start:http-client-create]
        ZLinkHttpClient client =
                ZLinkHttpClient.create("http://127.0.0.1:5280")
                        .timeout(Duration.ofSeconds(3))
                        .build();
        // --8<-- [end:http-client-create]

        try {
            // --8<-- [start:http-first-request]
            PlayerProfile first =
                    client.get("/players/p1/profile")
                            .fetch(PlayerProfile.class)
                            .toCompletableFuture()
                            .join();
            System.out.println("first request: " + first.playerId() + " " + first.nickname());
            // --8<-- [end:http-first-request]

            // --8<-- [start:http-request-shaping]
            HttpResponse<NodeStatus> status =
                    client.get("/ops/nodes/game-server-1/status")
                            .header("x-trace-id", "tutorial-1")
                            .timeout(Duration.ofSeconds(5))
                            .submit(NodeStatus.class)
                            .toCompletableFuture()
                            .join();
            // The admin route has another base URL, so this example uses a separate client.
            try (ZLinkHttpClient admin =
                    ZLinkHttpClient.create("http://127.0.0.1:5281")
                            .basicAuth("ops", "tutorial-admin")
                            .build()) {
                HttpResponse<WeightChanged> weight =
                        admin.post("/admin/channels/profile/weight")
                                .query("value", "2")
                                .submit(WeightChanged.class)
                                .toCompletableFuture()
                                .join();
                System.out.println(
                        "request shaping: status "
                                + status.status()
                                + " weight "
                                + weight.body().weight());
            }
            // --8<-- [end:http-request-shaping]

            // --8<-- [start:http-json-body]
            RawHttpResponse player =
                    client.post("/players/p2")
                            .body(new CreatePlayer("rookie"))
                            .submitRaw()
                            .toCompletableFuture()
                            .join();
            RawHttpResponse room =
                    client.post("/rooms")
                            .body(new OpenRoom("lobby"))
                            .submitRaw()
                            .toCompletableFuture()
                            .join();
            String roomId = stringBody(room);
            RawHttpResponse chat =
                    client.post("/rooms/" + roomId + "/chat")
                            .body(new PostChat("p2", "hello"))
                            .submitRaw()
                            .toCompletableFuture()
                            .join();
            System.out.println(
                    "json body: player "
                            + player.status()
                            + " room "
                            + roomId
                            + " chat "
                            + chat.status());
            // roomId is reused by the response, compression, download, and upload steps below.
            // --8<-- [end:http-json-body]

            // --8<-- [start:http-response-kinds]
            HttpResponse<PlayerInfo> typed =
                    client.get("/players/p2").submit(PlayerInfo.class).toCompletableFuture().join();
            RawHttpResponse raw =
                    client.get("/players/p2").submitRaw().toCompletableFuture().join();
            PlayerInfo fetched =
                    client.get("/players/p2").fetch(PlayerInfo.class).toCompletableFuture().join();
            System.out.println(
                    "response kinds: typed "
                            + typed.status()
                            + " raw "
                            + raw.headers().get("content-type")
                            + " fetch "
                            + fetched.nickname());
            // --8<-- [end:http-response-kinds]

            // The redirect target is an actor URL; create p1 because this standalone
            // program may start before the tutorial's actor example has run.
            client.post("/players/p1")
                    .body(new CreatePlayer("rookie"))
                    .submitRaw()
                    .toCompletableFuture()
                    .join();

            // --8<-- [start:http-compressed-response]
            try (ZLinkHttpClient compressed =
                    ZLinkHttpClient.create("http://127.0.0.1:5280")
                            .timeout(Duration.ofSeconds(3))
                            .compression()
                            .build()) {
                HttpResponse<RoomState> response =
                        compressed
                                .get("/rooms/" + roomId)
                                .submit(RoomState.class)
                                .toCompletableFuture()
                                .join();
                boolean encodingRemoved =
                        !response.headers().containsKey("content-encoding")
                                && response.body() != null;
                System.out.println(
                        "compressed response: "
                                + response.status()
                                + " encoding-removed "
                                + encodingRemoved);
            }
            // --8<-- [end:http-compressed-response]

            // --8<-- [start:http-redirect]
            try (ZLinkHttpClient redirects =
                    ZLinkHttpClient.create("http://127.0.0.1:5280").followRedirects().build()) {
                HttpResponse<PlayerInfo> redirected =
                        redirects
                                .get("/player/p1")
                                .submit(PlayerInfo.class)
                                .toCompletableFuture()
                                .join();
                System.out.println(
                        "redirect: " + redirected.status() + " " + redirected.body().playerId());
            }
            // --8<-- [end:http-redirect]

            // --8<-- [start:http-basic-auth]
            try (ZLinkHttpClient unauthenticated =
                            ZLinkHttpClient.create("http://127.0.0.1:5281").build();
                    ZLinkHttpClient authenticated =
                            ZLinkHttpClient.create("http://127.0.0.1:5281")
                                    .basicAuth("ops", "tutorial-admin")
                                    .build()) {
                int without =
                        unauthenticated
                                .post("/admin/channels/profile/weight")
                                .query("value", "2")
                                .submitRaw()
                                .toCompletableFuture()
                                .join()
                                .status();
                int with =
                        authenticated
                                .post("/admin/channels/profile/weight")
                                .query("value", "2")
                                .submitRaw()
                                .toCompletableFuture()
                                .join()
                                .status();
                System.out.println("basic auth: without " + without + " with " + with);
            }
            // --8<-- [end:http-basic-auth]

            // --8<-- [start:http-download-stream]
            AtomicInteger chunks = new AtomicInteger();
            AtomicInteger bytes = new AtomicInteger();
            client.get("/rooms/" + roomId + "/export")
                    .download(
                            chunk -> {
                                chunks.incrementAndGet();
                                bytes.addAndGet(chunk.length);
                            })
                    .toCompletableFuture()
                    .join();
            System.out.println("download stream: chunks " + chunks.get() + " bytes " + bytes.get());
            // --8<-- [end:http-download-stream]

            // --8<-- [start:http-upload-stream]
            byte[][] lines = {
                "{\"playerId\":\"p2\",\"text\":\"import-1\"}\n".getBytes(StandardCharsets.UTF_8),
                "{\"playerId\":\"p2\",\"text\":\"import-2\"}\n".getBytes(StandardCharsets.UTF_8),
                "{\"playerId\":\"p2\",\"text\":\"import-3\"}\n".getBytes(StandardCharsets.UTF_8),
            };
            AtomicInteger nextLine = new AtomicInteger();
            HttpResponse<Imported> imported =
                    client.post("/rooms/" + roomId + "/import")
                            .bodyStream(
                                    () -> {
                                        int index = nextLine.getAndIncrement();
                                        return index < lines.length ? lines[index] : null;
                                    },
                                    "application/x-ndjson")
                            .submit(Imported.class)
                            .toCompletableFuture()
                            .join();
            System.out.println("upload stream: imported " + imported.body().imported());
            // --8<-- [end:http-upload-stream]

            // --8<-- [start:http-error-kinds]
            String badRequestKind;
            try {
                client.post("/players/p3").submit(String.class).toCompletableFuture().join();
                throw new IllegalStateException("expected bad request");
            } catch (CompletionException error) {
                badRequestKind = frameworkFailure(error).kind().name();
            }
            String connectionRefusedKind;
            try (ZLinkHttpClient closedPort =
                    ZLinkHttpClient.create("http://127.0.0.1:6180").build()) {
                closedPort.get("/").submitRaw().toCompletableFuture().join();
                throw new IllegalStateException("expected connection refusal");
            } catch (CompletionException error) {
                connectionRefusedKind = frameworkFailure(error).kind().name();
            }
            System.out.println(
                    "error kinds: bad request "
                            + badRequestKind
                            + " connection refused "
                            + connectionRefusedKind);
            // --8<-- [end:http-error-kinds]
        } finally {
            client.close();
        }
    }

    private static ZLinkFrameworkException frameworkFailure(Throwable error) {
        Throwable current = error;
        while (current instanceof CompletionException && current.getCause() != null) {
            current = current.getCause();
        }
        if (current instanceof ZLinkFrameworkException failure) {
            return failure;
        }
        throw new IllegalStateException("unexpected HTTP failure", current);
    }

    private static String stringBody(RawHttpResponse response) {
        String body = response.body().trim();
        return body.startsWith("\"") && body.endsWith("\"")
                ? body.substring(1, body.length() - 1)
                : body;
    }

    private record PlayerProfile(String playerId, String nickname, int level) {}

    private record NodeStatus(
            String meshName, String channelName, String calledBy, String uptime, int processId) {}

    private record WeightChanged(String channel, int weight) {}

    private record CreatePlayer(String nickname) {}

    private record OpenRoom(String title) {}

    private record PostChat(String playerId, String text) {}

    private record PlayerInfo(String playerId, String nickname) {}

    private record RoomState(String title, java.util.List<String> chat) {}

    private record Imported(int imported) {}
}
