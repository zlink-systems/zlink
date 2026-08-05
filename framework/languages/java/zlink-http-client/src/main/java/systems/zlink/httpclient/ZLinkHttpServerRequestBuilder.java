/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient;

import java.time.Duration;
import java.util.Objects;
import java.util.concurrent.CompletionStage;
import java.util.function.Consumer;

public final class ZLinkHttpServerRequestBuilder {
    private final ZLinkHttpRequestBuilder request;
    private final ZLinkHttpExecutionTurn executionTurn;
    private final Consumer<Throwable> errorObserver;

    ZLinkHttpServerRequestBuilder(
        ZLinkHttpRequestBuilder request,
        ZLinkHttpExecutionTurn executionTurn,
        Consumer<Throwable> errorObserver) {
        this.request = request;
        this.executionTurn = executionTurn;
        this.errorObserver = errorObserver;
    }

    public ZLinkHttpServerRequestBuilder header(String name, String value) { request.header(name, value); return this; }
    public ZLinkHttpServerRequestBuilder query(String name, String value) { request.query(name, value); return this; }
    public ZLinkHttpServerRequestBuilder timeout(Duration value) { request.timeout(value); return this; }
    public ZLinkHttpServerRequestBuilder body(Object value) { request.body(value); return this; }
    public ZLinkHttpServerRequestBuilder body(String content, String contentType) { request.body(content, contentType); return this; }
    public ZLinkHttpServerRequestBuilder form(String name, String value) { request.form(name, value); return this; }
    public ZLinkHttpServerRequestBuilder multipart(String name, String value) { request.multipart(name, value); return this; }

    public CompletionStage<Void> submit() {
        CompletionStage<Void> submission = executionTurn.async(request.submitRaw())
            .thenApply(response -> null);
        submission.whenComplete((ignored, error) -> {
            if (error != null) {
                errorObserver.accept(error);
            }
        });
        return submission;
    }

    public <T> CompletionStage<HttpResponse<T>> submit(Class<T> type) {
        return executionTurn.async(request.submit(type));
    }

    public CompletionStage<RawHttpResponse> submitRaw() {
        return executionTurn.async(request.submitRaw());
    }

    public <T> CompletionStage<HttpResponse<T>> yield(Class<T> type) {
        return executionTurn.yield(request.submit(type));
    }

    public CompletionStage<RawHttpResponse> yieldRaw() {
        return executionTurn.yield(request.submitRaw());
    }

    public <T> void submit(Class<T> type, ZLinkHttpCallback<T> callback) {
        Objects.requireNonNull(callback, "callback");
        executionTurn.yield(request.submit(type)).whenComplete((response, error) ->
            callback.complete(error, error == null ? response : null));
    }
}
