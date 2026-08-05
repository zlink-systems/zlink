/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.OutputStream;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Deque;
import java.util.List;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.zip.DeflaterOutputStream;
import java.util.zip.GZIPOutputStream;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

final class HttpClientContractTest {

    private record Player(int id, String name) {
    }

    private record CreateGameReq(String name) {
    }

    private record CreateGameRes(String id, boolean ranked) {
    }

    @Test
    void serverClientUsesSelectedExecutionTerminatorAndFetchIsAsynchronous() throws Exception {
        assertEquals(CompletionStage.class,
            ZLinkHttpRequestBuilder.class.getMethod("fetch", Class.class).getReturnType());
        assertThrows(NoSuchMethodException.class,
            () -> ZLinkHttpRequestBuilder.class.getMethod("async", Class.class));
        assertThrows(NoSuchMethodException.class,
            () -> ZLinkHttpRequestBuilder.class.getMethod("asyncRaw"));
        assertThrows(NoSuchMethodException.class,
            () -> ZLinkHttpRequestBuilder.class.getMethod(
                "callback", Class.class, ZLinkHttpCallback.class));
        assertEquals(CompletionStage.class,
            ZLinkHttpRequestBuilder.class.getMethod("submitRaw").getReturnType());
        assertEquals(CompletionStage.class,
            ZLinkHttpRequestBuilder.class.getMethod("submit", Class.class).getReturnType());
        assertEquals(void.class,
            ZLinkHttpRequestBuilder.class.getMethod(
                "submit", Class.class, ZLinkHttpCallback.class).getReturnType());
        assertEquals(ZLinkHttpServerClient.class,
            ZLinkHttpClientBuilder.class.getMethod(
                "buildServer", ZLinkHttpExecutionTurn.class).getReturnType());
        AtomicInteger asyncCalls = new AtomicInteger();
        AtomicInteger yieldCalls = new AtomicInteger();
        ZLinkHttpExecutionTurn turn = new ZLinkHttpExecutionTurn() {
            @Override
            public <T> CompletionStage<T> async(CompletionStage<T> operation) {
                asyncCalls.incrementAndGet();
                return operation;
            }

            @Override
            public <T> CompletionStage<T> yield(CompletionStage<T> operation) {
                yieldCalls.incrementAndGet();
                return operation;
            }
        };
        TestSupport.Server server = TestSupport.httpServer(exchange ->
            TestSupport.respond(exchange, 200, "{\"id\":7,\"name\":\"p\"}"));
        try (ZLinkHttpClient client = ZLinkHttpClient.create(server.baseUrl()).build()) {
            ZLinkHttpServerClient serverClient = new ZLinkHttpServerClient(
                client, turn, error -> { throw new AssertionError(error); });
            assertEquals(7, serverClient.get("/p").submit(Player.class)
                .toCompletableFuture().join().body().id());
            assertEquals(7, serverClient.get("/p").yield(Player.class)
                .toCompletableFuture().join().body().id());
            serverClient.post("/p").submit().toCompletableFuture().join();
            assertEquals(2, asyncCalls.get());
            assertEquals(1, yieldCalls.get());
        } finally {
            server.closeable().close();
        }
    }

    @Test
    void dispatchesEachMethod() throws Exception {
        List<String> seen = new ArrayList<>();
        TestSupport.Server server = TestSupport.httpServer(exchange -> {
            seen.add(exchange.getRequestMethod());
            TestSupport.respond(exchange, 200, "{}");
        });
        try (ZLinkHttpClient client = ZLinkHttpClient.create(server.baseUrl()).build()) {
            assertEquals(200, client.get("/r").submitRaw().toCompletableFuture().join().status());
            client.post("/r").submitRaw().toCompletableFuture().join();
            client.put("/r").submitRaw().toCompletableFuture().join();
            client.delete("/r").submitRaw().toCompletableFuture().join();
            client.patch("/r").submitRaw().toCompletableFuture().join();
            client.options("/r").submitRaw().toCompletableFuture().join();
            assertEquals(List.of("GET", "POST", "PUT", "DELETE", "PATCH", "OPTIONS"), seen);
        } finally {
            server.closeable().close();
        }
    }

    @Test
    void headReturnsStatusWithEmptyBody() throws Exception {
        TestSupport.Server server = TestSupport.httpServer(exchange -> {
            exchange.getResponseHeaders().add("x-marker", "present");
            TestSupport.respond(exchange, 200, "");
        });
        try (ZLinkHttpClient client = ZLinkHttpClient.create(server.baseUrl()).build()) {
            RawHttpResponse response = client.head("/r").submitRaw().toCompletableFuture().join();
            assertEquals(200, response.status());
            assertEquals("", response.body());
            assertEquals("present", response.headers().get("x-marker"));
        } finally {
            server.closeable().close();
        }
    }

    @Test
    void typedSubmitReturnsNullBodyForEmptySuccessResponse() throws Exception {
        TestSupport.Server server = TestSupport.httpServer(exchange ->
            TestSupport.respond(exchange, 204, ""));
        try (ZLinkHttpClient client = ZLinkHttpClient.create(server.baseUrl()).build()) {
            HttpResponse<Player> response = client.get("/players/none")
                .submit(Player.class)
                .toCompletableFuture()
                .join();
            assertEquals(204, response.status());
            assertNull(response.body());
            assertEquals("", response.rawBody());
        } finally {
            server.closeable().close();
        }
    }

    @Test
    void percentEncodesQuery() throws Exception {
        AtomicReference<String> rawUrl = new AtomicReference<>();
        TestSupport.Server server = TestSupport.httpServer(exchange -> {
            rawUrl.set(exchange.getRequestURI().toString());
            TestSupport.respond(exchange, 200, "{}");
        });
        try (ZLinkHttpClient client = ZLinkHttpClient.create(server.baseUrl()).build()) {
            client.get("/search").query("q", "a b&c").query("k", "v/w").submitRaw().toCompletableFuture().join();
            assertEquals("/search?q=a%20b%26c&k=v%2Fw", rawUrl.get());
        } finally {
            server.closeable().close();
        }
    }

    @Test
    void sendsDefaultAndRequestHeaders() throws Exception {
        AtomicReference<String> auth = new AtomicReference<>();
        AtomicReference<String> trace = new AtomicReference<>();
        TestSupport.Server server = TestSupport.httpServer(exchange -> {
            auth.set(exchange.getRequestHeaders().getFirst("authorization"));
            trace.set(exchange.getRequestHeaders().getFirst("x-trace"));
            TestSupport.respond(exchange, 200, "{}");
        });
        try (ZLinkHttpClient client = ZLinkHttpClient.create(server.baseUrl())
            .defaultHeader("authorization", "Bearer token-123").build()) {
            client.get("/r").header("x-trace", "abc").submitRaw().toCompletableFuture().join();
            assertEquals("Bearer token-123", auth.get());
            assertEquals("abc", trace.get());
        } finally {
            server.closeable().close();
        }
    }

    @Test
    void typedJsonRoundTrip() throws Exception {
        AtomicReference<String> received = new AtomicReference<>();
        TestSupport.Server server = TestSupport.httpServer(exchange -> {
            received.set(TestSupport.readBody(exchange));
            exchange.getResponseHeaders().add("content-type", "application/json");
            TestSupport.respond(exchange, 200, "{\"id\":\"game-7\",\"ranked\":true}");
        });
        try (ZLinkHttpClient client = ZLinkHttpClient.create(server.baseUrl()).build()) {
            HttpResponse<CreateGameRes> response = client.post("/games")
                .body(new CreateGameReq("ranked-0611")).submit(CreateGameRes.class).toCompletableFuture().join();
            assertTrue(received.get().contains("ranked-0611"));
            assertEquals("game-7", response.body().id());
            assertTrue(response.body().ranked());
        } finally {
            server.closeable().close();
        }
    }

    @Test
    void fetchUnwrapsTypedBody() throws Exception {
        TestSupport.Server server = TestSupport.httpServer(exchange ->
            TestSupport.respond(exchange, 200, "{\"id\":7,\"name\":\"Aria\"}"));
        try (ZLinkHttpClient client = ZLinkHttpClient.create(server.baseUrl()).build()) {
            Player player = client.get("/players/7").fetch(Player.class)
                .toCompletableFuture().join();
            assertEquals(7, player.id());
            assertEquals("Aria", player.name());
        } finally {
            server.closeable().close();
        }
    }

    @Test
    void rawBodySetsContentType() throws Exception {
        AtomicReference<String> contentType = new AtomicReference<>();
        AtomicReference<String> body = new AtomicReference<>();
        TestSupport.Server server = TestSupport.httpServer(exchange -> {
            contentType.set(exchange.getRequestHeaders().getFirst("content-type"));
            body.set(TestSupport.readBody(exchange));
            TestSupport.respond(exchange, 200, "{}");
        });
        try (ZLinkHttpClient client = ZLinkHttpClient.create(server.baseUrl()).build()) {
            client.post("/raw").body("plain text", "text/plain").submitRaw().toCompletableFuture().join();
            assertEquals("plain text", body.get());
            assertEquals("text/plain", contentType.get());
        } finally {
            server.closeable().close();
        }
    }

    @Test
    void formUrlencodedBody() throws Exception {
        AtomicReference<String> contentType = new AtomicReference<>();
        AtomicReference<String> body = new AtomicReference<>();
        TestSupport.Server server = TestSupport.httpServer(exchange -> {
            contentType.set(exchange.getRequestHeaders().getFirst("content-type"));
            body.set(TestSupport.readBody(exchange));
            TestSupport.respond(exchange, 200, "{}");
        });
        try (ZLinkHttpClient client = ZLinkHttpClient.create(server.baseUrl()).build()) {
            client.post("/form").form("name", "a b").form("city", "x/y").submitRaw().toCompletableFuture().join();
            assertEquals("application/x-www-form-urlencoded", contentType.get());
            assertEquals("name=a%20b&city=x%2Fy", body.get());
        } finally {
            server.closeable().close();
        }
    }

    @Test
    void multipartBody() throws Exception {
        AtomicReference<String> contentType = new AtomicReference<>();
        AtomicReference<String> body = new AtomicReference<>();
        TestSupport.Server server = TestSupport.httpServer(exchange -> {
            contentType.set(exchange.getRequestHeaders().getFirst("content-type"));
            body.set(TestSupport.readBody(exchange));
            TestSupport.respond(exchange, 200, "{}");
        });
        try (ZLinkHttpClient client = ZLinkHttpClient.create(server.baseUrl()).build()) {
            client.post("/upload")
                .multipart("field", "value")
                .multipartFile("file", "a.txt", "file-content", "text/plain")
                .submitRaw().toCompletableFuture().join();
            assertTrue(contentType.get().startsWith("multipart/form-data; boundary="));
            assertTrue(body.get().contains("name=\"field\""));
            assertTrue(body.get().contains("filename=\"a.txt\""));
            assertTrue(body.get().contains("file-content"));
        } finally {
            server.closeable().close();
        }
    }

    @Test
    void streamingUploadSendsAllChunks() throws Exception {
        AtomicReference<String> captured = new AtomicReference<>();
        ServerSocket serverSocket = new ServerSocket(0, 0, java.net.InetAddress.getLoopbackAddress());
        Thread serverThread = new Thread(() -> {
            try (Socket socket = serverSocket.accept()) {
                StringBuilder buf = new StringBuilder();
                int b;
                while ((b = socket.getInputStream().read()) >= 0) {
                    buf.append((char) b);
                    if (buf.indexOf("0\r\n\r\n") >= 0) {
                        break;
                    }
                }
                String raw = buf.substring(buf.indexOf("\r\n\r\n") + 4);
                StringBuilder body = new StringBuilder();
                int pos = 0;
                while (true) {
                    int nl = raw.indexOf("\r\n", pos);
                    int size = Integer.parseInt(raw.substring(pos, nl).trim(), 16);
                    if (size == 0) {
                        break;
                    }
                    body.append(raw, nl + 2, nl + 2 + size);
                    pos = nl + 2 + size + 2;
                }
                captured.set(body.toString());
                OutputStream out = socket.getOutputStream();
                out.write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}".getBytes(StandardCharsets.US_ASCII));
                out.flush();
            } catch (Exception ignored) {
                // test teardown
            }
        });
        serverThread.setDaemon(true);
        serverThread.start();
        int port = serverSocket.getLocalPort();
        try (ZLinkHttpClient client = ZLinkHttpClient.create("http://127.0.0.1:" + port).build()) {
            Deque<byte[]> chunks = new ArrayDeque<>(List.of(
                "part-1;".getBytes(StandardCharsets.UTF_8),
                "part-2;".getBytes(StandardCharsets.UTF_8),
                "part-3".getBytes(StandardCharsets.UTF_8)));
            client.post("/s")
                .bodyStream(() -> chunks.isEmpty() ? null : chunks.poll(), "application/octet-stream")
                .submitRaw().toCompletableFuture().join();
        } finally {
            serverThread.join(2000);
            serverSocket.close();
        }
        assertEquals("part-1;part-2;part-3", captured.get());
    }

    @Test
    void streamingDownloadDeliversChunksToSink() throws Exception {
        TestSupport.Server server = TestSupport.httpServer(exchange ->
            TestSupport.respond(exchange, 200, "streamed-response-payload"));
        try (ZLinkHttpClient client = ZLinkHttpClient.create(server.baseUrl()).build()) {
            StringBuilder sink = new StringBuilder();
            RawHttpResponse response = client.get("/download")
                .download(chunk -> sink.append(new String(chunk, StandardCharsets.UTF_8)))
                .toCompletableFuture().join();
            assertEquals(200, response.status());
            assertEquals("", response.body());
            assertEquals("streamed-response-payload", sink.toString());
        } finally {
            server.closeable().close();
        }
    }

    @Test
    void status400ThrowsRequestFailed() throws Exception {
        TestSupport.Server server = TestSupport.httpServer(exchange ->
            TestSupport.respond(exchange, 404, "{\"error\":\"missing\"}"));
        try (ZLinkHttpClient client = ZLinkHttpClient.create(server.baseUrl()).build()) {
            CompletionException ex = assertThrows(CompletionException.class,
                () -> client.get("/players/0").submit(Player.class).toCompletableFuture().join());
            ZLinkFrameworkException failure = (ZLinkFrameworkException) ex.getCause();
            assertEquals(ZLinkFrameworkErrorKind.INTERNAL_FAILURE, failure.kind());
        } finally {
            server.closeable().close();
        }
    }

    @Test
    void malformedJsonThrowsDecodeFailure() throws Exception {
        TestSupport.Server server = TestSupport.httpServer(exchange ->
            TestSupport.respond(exchange, 200, "not-json"));
        try (ZLinkHttpClient client = ZLinkHttpClient.create(server.baseUrl()).build()) {
            CompletionException ex = assertThrows(CompletionException.class,
                () -> client.get("/x").submit(Player.class).toCompletableFuture().join());
            ZLinkFrameworkException failure = (ZLinkFrameworkException) ex.getCause();
            assertEquals(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, failure.kind());
        } finally {
            server.closeable().close();
        }
    }

    @Test
    void followsRedirectAndRewritesPostToGetOn303() throws Exception {
        AtomicReference<String> finalMethod = new AtomicReference<>();
        TestSupport.Server server = TestSupport.httpServer(exchange -> {
            if (exchange.getRequestURI().getPath().equals("/start")) {
                exchange.getResponseHeaders().add("location", "/result");
                TestSupport.respond(exchange, 303, "");
                return;
            }
            finalMethod.set(exchange.getRequestMethod());
            TestSupport.respond(exchange, 200, "{\"id\":\"game-1\",\"ranked\":false}");
        });
        try (ZLinkHttpClient client = ZLinkHttpClient.create(server.baseUrl()).followRedirects(3).build()) {
            HttpResponse<CreateGameRes> response = client.post("/start")
                .body(new CreateGameReq("x")).submit(CreateGameRes.class).toCompletableFuture().join();
            assertEquals("GET", finalMethod.get());
            assertEquals("game-1", response.body().id());
        } finally {
            server.closeable().close();
        }
    }

    @Test
    void preservesAuthorizationOnSameOriginRedirect() throws Exception {
        AtomicReference<String> authAtResult = new AtomicReference<>();
        TestSupport.Server server = TestSupport.httpServer(exchange -> {
            if (exchange.getRequestURI().getPath().equals("/start")) {
                exchange.getResponseHeaders().add("location", "/result");
                TestSupport.respond(exchange, 307, "");
                return;
            }
            authAtResult.set(exchange.getRequestHeaders().getFirst("authorization"));
            TestSupport.respond(exchange, 200, "{}");
        });
        try (ZLinkHttpClient client = ZLinkHttpClient.create(server.baseUrl())
            .bearerToken("secret").followRedirects(3).build()) {
            client.get("/start").submitRaw().toCompletableFuture().join();
            assertEquals("Bearer secret", authAtResult.get());
        } finally {
            server.closeable().close();
        }
    }

    @Test
    void stripsAuthorizationOnCrossOriginRedirect() throws Exception {
        AtomicReference<String> authAtOther = new AtomicReference<>();
        TestSupport.Server other = TestSupport.httpServer(exchange -> {
            authAtOther.set(exchange.getRequestHeaders().getFirst("authorization"));
            TestSupport.respond(exchange, 200, "{}");
        });
        TestSupport.Server origin = TestSupport.httpServer(exchange -> {
            exchange.getResponseHeaders().add("location", other.baseUrl() + "/result");
            TestSupport.respond(exchange, 307, "");
        });
        try (ZLinkHttpClient client = ZLinkHttpClient.create(origin.baseUrl())
            .bearerToken("secret").followRedirects(3).build()) {
            client.get("/start").submitRaw().toCompletableFuture().join();
            assertNull(authAtOther.get());
        } finally {
            origin.closeable().close();
            other.closeable().close();
        }
    }

    @Test
    void proxyCredentialsAreNotSentAsOriginRequestHeaders() throws Exception {
        AtomicReference<String> proxyAuthorization = new AtomicReference<>();
        TestSupport.Server proxy = TestSupport.httpServer(exchange -> {
            proxyAuthorization.set(exchange.getRequestHeaders().getFirst("proxy-authorization"));
            TestSupport.respond(exchange, 200, "{}");
        });
        try (ZLinkHttpClient client = ZLinkHttpClient.create("http://origin.invalid")
            .proxy(proxy.baseUrl())
            .proxyBasicAuth("proxy-user", "proxy-secret")
            .build()) {
            client.get("/resource").submitRaw().toCompletableFuture().join();
            assertNull(proxyAuthorization.get());
        } finally {
            proxy.closeable().close();
        }
    }

    @Test
    void timeoutCoversTheWholeRedirectAttempt() throws Exception {
        AtomicInteger hops = new AtomicInteger();
        TestSupport.Server server = TestSupport.httpServer(exchange -> {
            try {
                Thread.sleep(100);
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
            }
            if (hops.incrementAndGet() == 1) {
                exchange.getResponseHeaders().add("location", "/second");
                TestSupport.respond(exchange, 302, "");
            } else {
                TestSupport.respond(exchange, 200, "{}");
            }
        });
        try (ZLinkHttpClient client = ZLinkHttpClient.create(server.baseUrl())
            .timeout(Duration.ofMillis(150))
            .followRedirects(2)
            .build()) {
            assertThrows(CompletionException.class,
                () -> client.get("/first").submitRaw().toCompletableFuture().join());
        } finally {
            server.closeable().close();
        }
    }

    @Test
    void redirectLimitExceededThrows() throws Exception {
        TestSupport.Server server = TestSupport.httpServer(exchange -> {
            exchange.getResponseHeaders().add("location", "/loop");
            TestSupport.respond(exchange, 302, "");
        });
        try (ZLinkHttpClient client = ZLinkHttpClient.create(server.baseUrl()).followRedirects(2).build()) {
            assertThrows(CompletionException.class,
                () -> client.get("/loop").submitRaw().toCompletableFuture().join());
        } finally {
            server.closeable().close();
        }
    }

    @Test
    void retriesRetriableTransportFailure() throws Exception {
        AtomicInteger connections = new AtomicInteger();
        ServerSocket serverSocket = new ServerSocket(0, 0, java.net.InetAddress.getLoopbackAddress());
        Thread serverThread = new Thread(() -> {
            while (!serverSocket.isClosed()) {
                try {
                    Socket socket = serverSocket.accept();
                    if (connections.incrementAndGet() <= 1) {
                        socket.close();
                        continue;
                    }
                    socket.getInputStream().read(new byte[1024]);
                    OutputStream out = socket.getOutputStream();
                    out.write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}".getBytes(StandardCharsets.US_ASCII));
                    out.flush();
                    socket.close();
                } catch (Exception ignored) {
                    return;
                }
            }
        });
        serverThread.setDaemon(true);
        serverThread.start();
        int port = serverSocket.getLocalPort();
        try (ZLinkHttpClient client = ZLinkHttpClient.create("http://127.0.0.1:" + port).retry(3).build()) {
            RawHttpResponse response = client.get("/r").submitRaw().toCompletableFuture().join();
            assertEquals(200, response.status());
            assertTrue(connections.get() >= 2);
        } finally {
            serverSocket.close();
        }
    }

    @Test
    void cookieJarRoundTripWithPathScope() throws Exception {
        AtomicReference<String> cookieAtScoped = new AtomicReference<>();
        AtomicReference<String> cookieAtRoot = new AtomicReference<>();
        TestSupport.Server server = TestSupport.httpServer(exchange -> {
            String path = exchange.getRequestURI().getPath();
            if (path.equals("/login")) {
                exchange.getResponseHeaders().add("Set-Cookie", "session=abc; Path=/secure");
            } else if (path.equals("/secure/data")) {
                cookieAtScoped.set(exchange.getRequestHeaders().getFirst("cookie"));
            } else {
                cookieAtRoot.set(exchange.getRequestHeaders().getFirst("cookie"));
            }
            TestSupport.respond(exchange, 200, "{}");
        });
        try (ZLinkHttpClient client = ZLinkHttpClient.create(server.baseUrl()).cookies().build()) {
            client.get("/login").submitRaw().toCompletableFuture().join();
            client.get("/secure/data").submitRaw().toCompletableFuture().join();
            client.get("/open").submitRaw().toCompletableFuture().join();
            assertEquals("session=abc", cookieAtScoped.get());
            assertNull(cookieAtRoot.get());
        } finally {
            server.closeable().close();
        }
    }

    @Test
    void transparentlyDecodesGzip() throws Exception {
        runCompressionTest(true);
    }

    @Test
    void transparentlyDecodesDeflate() throws Exception {
        runCompressionTest(false);
    }

    private void runCompressionTest(boolean gzip) throws Exception {
        String payload = "{\"id\":42,\"name\":\"Compressed\"}";
        TestSupport.Server server = TestSupport.httpServer(exchange -> {
            var buffer = new java.io.ByteArrayOutputStream();
            try (OutputStream compressor = gzip
                ? new GZIPOutputStream(buffer)
                : new DeflaterOutputStream(buffer)) {
                compressor.write(payload.getBytes(StandardCharsets.UTF_8));
            }
            exchange.getResponseHeaders().add("content-type", "application/json");
            exchange.getResponseHeaders().add("content-encoding", gzip ? "gzip" : "deflate");
            TestSupport.respondBytes(exchange, 200, buffer.toByteArray());
        });
        try (ZLinkHttpClient client = ZLinkHttpClient.create(server.baseUrl()).compression().build()) {
            HttpResponse<Player> response = client.get("/data").submit(Player.class).toCompletableFuture().join();
            assertEquals(42, response.body().id());
            assertFalse(response.headers().containsKey("content-encoding"));
        } finally {
            server.closeable().close();
        }
    }

    @Test
    void basicAndBearerAuth() throws Exception {
        List<String> seen = new ArrayList<>();
        TestSupport.Server server = TestSupport.httpServer(exchange -> {
            seen.add(exchange.getRequestHeaders().getFirst("authorization"));
            TestSupport.respond(exchange, 200, "{}");
        });
        try {
            try (ZLinkHttpClient basic = ZLinkHttpClient.create(server.baseUrl()).basicAuth("aria", "secret").build()) {
                basic.get("/a").submitRaw().toCompletableFuture().join();
            }
            try (ZLinkHttpClient bearer = ZLinkHttpClient.create(server.baseUrl()).bearerToken("tok-9").build()) {
                bearer.get("/b").submitRaw().toCompletableFuture().join();
            }
            assertEquals("Basic " + java.util.Base64.getEncoder().encodeToString("aria:secret".getBytes(StandardCharsets.UTF_8)), seen.get(0));
            assertEquals("Bearer tok-9", seen.get(1));
        } finally {
            server.closeable().close();
        }
    }

    @Test
    void secureCookieNotSentOverHttpAndMaxAgeDeletes() throws Exception {
        AtomicReference<String> cookieAfterSecure = new AtomicReference<>();
        AtomicReference<String> cookieAfterDelete = new AtomicReference<>();
        TestSupport.Server server = TestSupport.httpServer(exchange -> {
            String path = exchange.getRequestURI().getPath();
            if (path.equals("/set")) {
                exchange.getResponseHeaders().add("Set-Cookie", "sid=abc; Secure");
                exchange.getResponseHeaders().add("Set-Cookie", "keep=1");
            } else if (path.equals("/c1")) {
                cookieAfterSecure.set(exchange.getRequestHeaders().getFirst("cookie"));
                exchange.getResponseHeaders().add("Set-Cookie", "keep=; Max-Age=0");
            } else {
                cookieAfterDelete.set(exchange.getRequestHeaders().getFirst("cookie"));
            }
            TestSupport.respond(exchange, 200, "{}");
        });
        try (ZLinkHttpClient client = ZLinkHttpClient.create(server.baseUrl()).cookies().build()) {
            client.get("/set").submitRaw().toCompletableFuture().join();
            client.get("/c1").submitRaw().toCompletableFuture().join();
            client.get("/c2").submitRaw().toCompletableFuture().join();
            assertEquals("keep=1", cookieAfterSecure.get());
            assertNull(cookieAfterDelete.get());
        } finally {
            server.closeable().close();
        }
    }

    @Test
    void compressedMalformedBodyThrows() throws Exception {
        TestSupport.Server server = TestSupport.httpServer(exchange -> {
            exchange.getResponseHeaders().add("content-encoding", "gzip");
            TestSupport.respondBytes(exchange, 200, new byte[] {1, 2, 3, 4, 5, 6, 7, 8});
        });
        try (ZLinkHttpClient client = ZLinkHttpClient.create(server.baseUrl()).compression().build()) {
            assertThrows(CompletionException.class,
                () -> client.get("/bad").submitRaw().toCompletableFuture().join());
        } finally {
            server.closeable().close();
        }
    }

    @Test
    void timeoutThrows() throws Exception {
        TestSupport.Server server = TestSupport.httpServer(exchange -> {
            try {
                Thread.sleep(500);
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
            }
            TestSupport.respond(exchange, 200, "{}");
        });
        try (ZLinkHttpClient client = ZLinkHttpClient.create(server.baseUrl()).timeout(Duration.ofMillis(80)).build()) {
            CompletionException ex = assertThrows(CompletionException.class,
                () -> client.get("/slow").submitRaw().toCompletableFuture().join());
            ZLinkFrameworkException failure = (ZLinkFrameworkException) ex.getCause();
            assertEquals(ZLinkFrameworkErrorKind.INTERNAL_FAILURE, failure.kind());
        } finally {
            server.closeable().close();
        }
    }

    @Test
    void bodyReadUsesTheRemainingRequestDeadline() throws Exception {
        ServerSocket serverSocket = new ServerSocket(
            0, 0, java.net.InetAddress.getLoopbackAddress());
        Thread serverThread = new Thread(() -> {
            try (Socket socket = serverSocket.accept()) {
                java.io.BufferedReader request = new java.io.BufferedReader(
                    new java.io.InputStreamReader(socket.getInputStream(), StandardCharsets.US_ASCII));
                String line;
                while ((line = request.readLine()) != null && !line.isEmpty()) {
                    // Consume the request headers before writing the response.
                }
                OutputStream out = socket.getOutputStream();
                out.write((
                    "HTTP/1.1 200 OK\r\n"
                        + "Content-Length: 1024\r\n"
                        + "Connection: keep-alive\r\n\r\n"
                        + "x").getBytes(StandardCharsets.US_ASCII));
                out.flush();
                Thread.sleep(1_000);
            } catch (Exception ignored) {
                // The client closes the body stream when the deadline expires.
            }
        }, "zlink-http-stalled-body-test");
        serverThread.setDaemon(true);
        serverThread.start();
        try (ZLinkHttpClient client = ZLinkHttpClient.create(
                "http://127.0.0.1:" + serverSocket.getLocalPort())
            .timeout(Duration.ofMillis(120))
            .build()) {
            CompletionException error = assertThrows(
                CompletionException.class,
                () -> client.get("/stalled")
                    .submitRaw()
                    .toCompletableFuture()
                    .orTimeout(2, java.util.concurrent.TimeUnit.SECONDS)
                    .join());
            assertTrue(error.getCause() instanceof ZLinkFrameworkException);
        } finally {
            serverSocket.close();
            serverThread.interrupt();
            serverThread.join(1_000);
        }
    }

    @Test
    void maxResponseBodySizeEnforced() throws Exception {
        TestSupport.Server server = TestSupport.httpServer(exchange ->
            TestSupport.respond(exchange, 200, "x".repeat(4096)));
        try (ZLinkHttpClient client = ZLinkHttpClient.create(server.baseUrl()).maxResponseBodySize(1024).build()) {
            assertThrows(CompletionException.class,
                () -> client.get("/big").submitRaw().toCompletableFuture().join());
        } finally {
            server.closeable().close();
        }
    }

    @Test
    void nonBlockingConcurrency() throws Exception {
        TestSupport.Server server = TestSupport.httpServer(exchange -> {
            try {
                Thread.sleep(200);
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
            }
            TestSupport.respond(exchange, 200, "{}");
        });
        try (ZLinkHttpClient client = ZLinkHttpClient.create(server.baseUrl()).build()) {
            long start = System.nanoTime();
            List<java.util.concurrent.CompletableFuture<RawHttpResponse>> futures = new ArrayList<>();
            for (int i = 0; i < 20; i++) {
                futures.add(client.get("/r").submitRaw().toCompletableFuture());
            }
            for (var future : futures) {
                assertEquals(200, future.join().status());
            }
            long elapsedMs = (System.nanoTime() - start) / 1_000_000;
            assertTrue(elapsedMs < 2000, "elapsed=" + elapsedMs);
        } finally {
            server.closeable().close();
        }
    }

    @Test
    void validationRejectsInvalidConfiguration() {
        ZLinkFrameworkException invalid = assertThrows(
            ZLinkFrameworkException.class, () -> ZLinkHttpClient.create().build());
        assertEquals(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, invalid.kind());
        assertThrows(ZLinkFrameworkException.class, () -> ZLinkHttpClient.create("ftp://x").build());
        assertThrows(ZLinkFrameworkException.class, () -> ZLinkHttpClient.create("http://h").timeout(Duration.ZERO));
        assertThrows(ZLinkFrameworkException.class,
            () -> ZLinkHttpClient.create("http://h").timeout(Duration.ofNanos(1))
                .timeout(Duration.ofMillis(Integer.MAX_VALUE).plusMillis(1)));
        assertDoesNotThrow(() -> ZLinkHttpClient.create("http://h")
            .timeout(Duration.ofNanos(1)));
        assertThrows(ZLinkFrameworkException.class, () -> ZLinkHttpClient.create("http://h").proxy("https://p").build());
        assertThrows(ZLinkFrameworkException.class, () -> ZLinkHttpClient.create("http://h").followRedirects(0));
        assertThrows(ZLinkFrameworkException.class, () -> ZLinkHttpClient.create("http://h").retry(0));
    }

    @Test
    void validationRejectsBadRequestConfiguration() throws Exception {
        try (ZLinkHttpClient client = ZLinkHttpClient.create("http://127.0.0.1:1").build()) {
            assertThrows(ZLinkFrameworkException.class, () -> client.get("no-slash"));
            assertThrows(ZLinkFrameworkException.class,
                () -> client.post("/r").body("a", "text/plain").form("b", "c").submitRaw());
            assertThrows(ZLinkFrameworkException.class,
                () -> client.post("/r").bodyStream(null, "application/octet-stream"));
            assertThrows(ZLinkFrameworkException.class, () -> client.get("/r").download(null));
        }
    }

    @Test
    void trustsTestCertificateOverHttps() throws Exception {
        TestSupport.Server server = TestSupport.httpsServer(exchange -> TestSupport.respond(exchange, 200, "{}"), false);
        try (ZLinkHttpClient client = ZLinkHttpClient.create(server.baseUrl())
            .trustCertificateFile(TestSupport.resourcePath("/tls/server-cert.pem")).build()) {
            assertEquals(200, client.get("/secure").submitRaw().toCompletableFuture().join().status());
        } finally {
            server.closeable().close();
        }
    }

    @Test
    void untrustedHttpsCertificateRejected() throws Exception {
        TestSupport.Server server = TestSupport.httpsServer(exchange -> TestSupport.respond(exchange, 200, "{}"), false);
        try (ZLinkHttpClient client = ZLinkHttpClient.create(server.baseUrl())
            .trustCertificateFile(TestSupport.resourcePath("/tls/other-cert.pem")).build()) {
            assertThrows(CompletionException.class,
                () -> client.get("/secure").submitRaw().toCompletableFuture().join());
        } finally {
            server.closeable().close();
        }
    }

    @Test
    void presentsClientCertificateForMtls() throws Exception {
        TestSupport.Server server = TestSupport.httpsServer(exchange -> TestSupport.respond(exchange, 200, "{}"), true);
        try (ZLinkHttpClient client = ZLinkHttpClient.create(server.baseUrl())
            .trustCertificateFile(TestSupport.resourcePath("/tls/server-cert.pem"))
            .clientCertificateFile(
                TestSupport.resourcePath("/tls/client-cert.pem"),
                TestSupport.resourcePath("/tls/client-key.pem"))
            .build()) {
            assertEquals(200, client.get("/mtls").submitRaw().toCompletableFuture().join().status());
        } finally {
            server.closeable().close();
        }
    }
}
