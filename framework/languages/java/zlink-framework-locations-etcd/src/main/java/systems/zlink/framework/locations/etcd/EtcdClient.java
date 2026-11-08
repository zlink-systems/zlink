package systems.zlink.framework.locations.etcd;

import com.google.protobuf.MessageLite;

import io.grpc.*;
import io.grpc.netty.shaded.io.grpc.netty.GrpcSslContexts;
import io.grpc.netty.shaded.io.grpc.netty.NettyChannelBuilder;
import io.grpc.netty.shaded.io.netty.handler.ssl.SslContext;
import io.grpc.netty.shaded.io.netty.handler.ssl.SslContextBuilder;
import io.grpc.stub.ClientCalls;
import io.grpc.stub.MetadataUtils;
import io.grpc.stub.StreamObserver;

import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;
import systems.zlink.framework.locations.etcd.internal.proto.etcdserverpb.AuthGrpc;
import systems.zlink.framework.locations.etcd.internal.proto.etcdserverpb.KVGrpc;
import systems.zlink.framework.locations.etcd.internal.proto.etcdserverpb.Rpc.*;

import java.io.ByteArrayInputStream;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;

final class EtcdClient implements AutoCloseable {
    enum ErrorKind {
        COMPACTION,
        NOSPACE,
        AUTH,
        STORE_FAILURE
    }

    static final class CallFailure extends CompletionException {
        final ErrorKind kind;

        CallFailure(ErrorKind kind) {
            super(
                    new java.io.IOException(
                            "The Location Store operation failed; its write outcome may be unknown."));
            this.kind = kind;
        }
    }

    static final class Operation implements AutoCloseable {
        final Deadline deadline;
        final Context.CancellableContext context = Context.ROOT.withCancellation();
        final ZLinkStoreCancellation cancellation;
        private final ScheduledFuture<?> watcher;
        private volatile boolean started;

        Operation(
                long timeoutNanos,
                ZLinkStoreCancellation cancellation,
                ScheduledExecutorService scheduler) {
            this.deadline = Deadline.after(timeoutNanos, TimeUnit.NANOSECONDS);
            this.cancellation = cancellation;
            // The SPI token is poll-only; poll only for operations that carry one.
            watcher =
                    cancellation == null
                            ? null
                            : scheduler.scheduleWithFixedDelay(
                                    () -> {
                                        if (cancellation.isCancellationRequested())
                                            context.cancel(null);
                                    },
                                    0,
                                    5,
                                    TimeUnit.MILLISECONDS);
        }

        void check() {
            if (context.isCancelled()
                    || (cancellation != null && cancellation.isCancellationRequested())) {
                if (!started)
                    throw new CancellationException("Store operation cancelled before I/O.");
                throw EtcdEncoding.failure();
            }
            if (deadline.isExpired()) throw EtcdEncoding.failure();
        }

        void start() {
            check();
            started = true;
        }

        public void close() {
            if (watcher != null) watcher.cancel(false);
            context.close();
        }
    }

    private final List<ManagedChannel> channels = new ArrayList<>();
    private final AtomicInteger endpoint = new AtomicInteger();
    private final ZLinkEtcdCredentials credentials;
    private volatile String token;
    final long timeoutNanos;

    EtcdClient(ZLinkEtcdLocationOptions options) {
        if (options == null) throw new IllegalArgumentException("Store options are required.");
        var ns = EtcdEncoding.utf8(options.namespace());
        if (ns.isEmpty() || ns.size() > 64 || options.namespace().indexOf(0) >= 0)
            throw new IllegalArgumentException("Invalid Store namespace.");
        if (options.operationTimeout() == null
                || options.operationTimeout().isNegative()
                || options.operationTimeout().isZero())
            throw new IllegalArgumentException("Invalid operation timeout.");
        timeoutNanos =
                options.operationTimeout().compareTo(java.time.Duration.ofNanos(Long.MAX_VALUE)) > 0
                        ? Long.MAX_VALUE
                        : options.operationTimeout().toNanos();
        if (options.endpoints() == null || options.endpoints().isEmpty())
            throw new IllegalArgumentException("Store endpoints are required.");
        List<URI> endpoints = new ArrayList<>();
        for (String endpoint : options.endpoints()) {
            URI uri;
            try {
                uri = URI.create(endpoint);
            } catch (RuntimeException error) {
                throw new IllegalArgumentException("Invalid Store endpoint.");
            }
            if (!("http".equals(uri.getScheme()) || "https".equals(uri.getScheme()))
                    || uri.getHost() == null
                    || uri.getUserInfo() != null
                    || uri.getQuery() != null
                    || uri.getFragment() != null
                    || (!uri.getPath().isEmpty() && !uri.getPath().equals("/"))
                    || uri.getPort() == 0
                    || uri.getPort() > 65535
                    || (!endpoints.isEmpty()
                            && !endpoints.get(0).getScheme().equals(uri.getScheme())))
                throw new IllegalArgumentException("Invalid Store endpoint.");
            endpoints.add(uri);
        }
        credentials = options.credentials();
        if (credentials != null
                && (credentials.user() == null
                        || credentials.user().isEmpty()
                        || credentials.password() == null
                        || credentials.password().isEmpty()))
            throw new IllegalArgumentException("Invalid Store credentials.");
        boolean secure = endpoints.get(0).getScheme().equals("https");
        if (!secure && options.tls() != null)
            throw new IllegalArgumentException("TLS requires HTTPS endpoints.");
        SslContext tls = null;
        if (secure) {
            try {
                SslContextBuilder builder = GrpcSslContexts.forClient();
                if (options.tls() != null) {
                    if (options.tls().trustedCaPem() != null)
                        builder.trustManager(pem(options.tls().trustedCaPem()));
                    var identity = options.tls().clientIdentity();
                    if (identity != null)
                        builder.keyManager(pem(identity.certificatePem()), pem(identity.keyPem()));
                }
                tls = builder.build();
            } catch (java.io.IOException | IllegalArgumentException error) {
                throw new IllegalArgumentException("Invalid Store TLS material.");
            }
        }
        try {
            for (URI uri : endpoints) {
                String host = uri.getHost();
                if (host.startsWith("[")) host = host.substring(1, host.length() - 1);
                var builder =
                        NettyChannelBuilder.forAddress(
                                        host,
                                        uri.getPort() == -1 ? (secure ? 443 : 80) : uri.getPort())
                                .disableRetry()
                                .disableServiceConfigLookUp()
                                .maxInboundMessageSize(8 * 1024 * 1024);
                if (secure) builder.sslContext(tls);
                else builder.usePlaintext();
                channels.add(builder.build());
            }
        } catch (RuntimeException error) {
            close();
            throw error;
        }
    }

    private static ByteArrayInputStream pem(String value) {
        if (value == null || value.isEmpty())
            throw new IllegalArgumentException("TLS PEM is required.");
        return new ByteArrayInputStream(value.getBytes(StandardCharsets.UTF_8));
    }

    static ErrorKind classify(Status.Code code, String description) {
        String text = description == null ? "" : description;
        if (code == Status.Code.OUT_OF_RANGE
                && text.contains("required revision has been compacted"))
            return ErrorKind.COMPACTION;
        if (code == Status.Code.RESOURCE_EXHAUSTED && text.contains("database space exceeded"))
            return ErrorKind.NOSPACE;
        if ((code == Status.Code.UNAUTHENTICATED && text.contains("invalid auth token"))
                || (code == Status.Code.INVALID_ARGUMENT
                        && text.contains("revision of auth store is old"))) return ErrorKind.AUTH;
        return ErrorKind.STORE_FAILURE;
    }

    static Throwable unwrap(Throwable error) {
        while (error instanceof CompletionException
                && !(error instanceof CallFailure)
                && error.getCause() != null) error = error.getCause();
        return error;
    }

    private <Q extends MessageLite, R> CompletableFuture<R> invoke(
            MethodDescriptor<Q, R> method, Q request, Operation operation, boolean authenticated) {
        operation.start();
        if (request.getSerializedSize() > 16 * 1024 * 1024)
            throw new IllegalArgumentException("Store request exceeds its transport bound.");
        int index = endpoint.get();
        Channel channel = channels.get(index);
        String current = token;
        if (authenticated && current != null) {
            Metadata headers = new Metadata();
            headers.put(Metadata.Key.of("token", Metadata.ASCII_STRING_MARSHALLER), current);
            channel =
                    ClientInterceptors.intercept(
                            channel, MetadataUtils.newAttachHeadersInterceptor(headers));
        }
        CompletableFuture<R> result = new CompletableFuture<>();
        Context previous = operation.context.attach();
        try {
            ClientCall<Q, R> call =
                    channel.newCall(
                            method,
                            CallOptions.DEFAULT
                                    .withDeadline(operation.deadline)
                                    .withMaxOutboundMessageSize(16 * 1024 * 1024));
            ClientCalls.asyncUnaryCall(
                    call,
                    request,
                    new StreamObserver<>() {
                        private R response;

                        public void onNext(R value) {
                            response = value;
                        }

                        public void onError(Throwable error) {
                            Status status = Status.fromThrowable(error);
                            if (status.getCode() == Status.Code.UNAVAILABLE
                                    || status.getCode() == Status.Code.DEADLINE_EXCEEDED)
                                endpoint.compareAndSet(index, (index + 1) % channels.size());
                            result.completeExceptionally(
                                    new CallFailure(
                                            classify(status.getCode(), status.getDescription())));
                        }

                        public void onCompleted() {
                            if (response == null)
                                result.completeExceptionally(EtcdEncoding.failure());
                            else result.complete(response);
                        }
                    });
        } finally {
            operation.context.detach(previous);
        }
        return result;
    }

    private CompletableFuture<Void> authenticate(Operation operation) {
        return invoke(
                        AuthGrpc.getAuthenticateMethod(),
                        AuthenticateRequest.newBuilder()
                                .setName(credentials.user())
                                .setPassword(credentials.password())
                                .build(),
                        operation,
                        false)
                .thenAccept(response -> token = response.getToken());
    }

    private <Q extends MessageLite, R> CompletableFuture<R> call(
            MethodDescriptor<Q, R> method, Q request, Operation operation) {
        CompletableFuture<Void> ready =
                credentials != null && token == null
                        ? authenticate(operation)
                        : CompletableFuture.completedFuture(null);
        return ready.thenCompose(
                ignored ->
                        invoke(method, request, operation, true)
                                .handle(
                                        (response, error) -> {
                                            if (error == null)
                                                return CompletableFuture.completedFuture(response);
                                            if (unwrap(error) instanceof CallFailure failure
                                                    && failure.kind == ErrorKind.AUTH
                                                    && credentials != null)
                                                return authenticate(operation)
                                                        .thenCompose(
                                                                nothing ->
                                                                        invoke(
                                                                                method, request,
                                                                                operation, true));
                                            return CompletableFuture.<R>failedFuture(unwrap(error));
                                        })
                                .thenCompose(value -> value));
    }

    CompletableFuture<TxnResponse> txn(TxnRequest request, Operation operation) {
        return call(KVGrpc.getTxnMethod(), request, operation);
    }

    CompletableFuture<RangeResponse> range(RangeRequest request, Operation operation) {
        return call(KVGrpc.getRangeMethod(), request, operation);
    }

    public void close() {
        for (ManagedChannel channel : channels) channel.shutdownNow();
        boolean interrupted = false;
        for (ManagedChannel channel : channels) {
            while (!channel.isTerminated())
                try {
                    channel.awaitTermination(1, TimeUnit.SECONDS);
                } catch (InterruptedException error) {
                    interrupted = true;
                }
        }
        if (interrupted) Thread.currentThread().interrupt();
    }
}
