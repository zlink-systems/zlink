/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.bench.withgrpc.shared;

import java.net.ServerSocket;
import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;

/** The trigger response publishes the completed driver's start, including repeat and failure. */
public final class TriggerCompletionTest {
    private static final long DRIVER_START_NS = 123_456_789L;

    public static void main(String[] args) throws Exception {
        AtomicInteger calls = new AtomicInteger();
        CompletableFuture<Void> entered = new CompletableFuture<>();
        CompletableFuture<Void> release = new CompletableFuture<>();
        BenchHttpApplication.Controller controller = new BenchHttpApplication.Controller(
            () -> true, BenchHttpApplication.Counters::empty, request -> run(request, calls, entered, release));
        String triggerUrl = "http://127.0.0.1:" + freePort();
        String statsUrl = "http://127.0.0.1:" + freePort();
        try (BenchHttpApplication application = BenchHttpApplication.start(triggerUrl, statsUrl, controller);
             HttpClient client = HttpClient.newHttpClient()) {
            CompletableFuture<HttpResponse<String>> pending = postAsync(client, triggerUrl, "ok");
            entered.get(5, TimeUnit.SECONDS);
            CompletableFuture<HttpResponse<String>> duplicate = postAsync(client, triggerUrl, "ok");
            try {
                HttpResponse<String> conflict = post(client, triggerUrl, "conflict");
                check(conflict.statusCode() == 409 && calls.get() == 1,
                    "concurrent distinct trigger entered an occupied phase");
                check(!pending.isDone(), "trigger response preceded workload completion");
            } finally {
                release.complete(null);
            }
            HttpResponse<String> first = pending.get(5, TimeUnit.SECONDS);
            check(duplicate.get(5, TimeUnit.SECONDS).body().equals(first.body()) && calls.get() == 1,
                "pending duplicate did not share the completed outcome");
            check(first.statusCode() == 200, "successful phase was rejected: " + first.body());
            check(first.body().contains("\"startedAt\":" + DRIVER_START_NS),
                "acknowledgement must publish driver start: " + first.body());
            check(controller.snapshotJson().contains("\"phase\":\"idle\""),
                "response preceded phase completion");
            HttpResponse<String> repeated = post(client, triggerUrl, "ok");
            check(repeated.body().equals(first.body()) && calls.get() == 1,
                "duplicate trigger changed the acknowledgement or repeated work");
            HttpResponse<String> next = post(client, triggerUrl, "next");
            check(next.statusCode() == 200 && calls.get() == 2,
                "a distinct cell did not execute once in the same process");
            check(post(client, triggerUrl, "next").body().equals(next.body()) && calls.get() == 2,
                "distinct cell duplicate repeated work");
            HttpResponse<String> failed = post(client, triggerUrl, "failed");
            check(failed.statusCode() == 500 && failed.body().contains("expected phase failure"),
                "failed phase returned successful acknowledgement: " + failed.body());
            HttpResponse<String> repeatedFailure = post(client, triggerUrl, "failed");
            check(repeatedFailure.statusCode() == 500 && repeatedFailure.body().equals(failed.body())
                    && calls.get() == 3, "failed duplicate repeated work or lost its outcome");
        }
        System.out.println("TRIGGER_COMPLETION_OK");
    }

    private static long run(BenchHttpApplication.Trigger request, AtomicInteger calls,
                            CompletableFuture<Void> entered, CompletableFuture<Void> release) {
        calls.incrementAndGet();
        entered.complete(null);
        release.join();
        if (request.cellId().equals("failed")) throw new IllegalStateException("expected phase failure");
        return DRIVER_START_NS;
    }

    private static HttpResponse<String> post(HttpClient client, String url, String cell) throws Exception {
        return postAsync(client, url, cell).get(5, TimeUnit.SECONDS);
    }

    private static CompletableFuture<HttpResponse<String>> postAsync(HttpClient client, String url, String cell) {
        String body = "{\"runId\":\"completion\",\"cellId\":\"" + cell
            + "\",\"pattern\":\"request-serial\",\"payloadBytes\":1024,\"phase\":\"active\","
            + "\"durationMs\":1,\"requestWindow\":100,\"sendConcurrency\":8}";
        return client.sendAsync(HttpRequest.newBuilder(URI.create(url + "/bench/start"))
            .POST(HttpRequest.BodyPublishers.ofString(body)).build(), HttpResponse.BodyHandlers.ofString());
    }

    private static int freePort() throws Exception {
        try (ServerSocket socket = new ServerSocket(0)) { return socket.getLocalPort(); }
    }

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
}
