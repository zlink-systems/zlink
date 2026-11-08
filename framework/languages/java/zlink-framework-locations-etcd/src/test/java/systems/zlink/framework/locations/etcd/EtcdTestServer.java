package systems.zlink.framework.locations.etcd;

import com.google.protobuf.ByteString;

import io.grpc.Context;
import io.grpc.Server;
import io.grpc.Status;
import io.grpc.netty.shaded.io.grpc.netty.NettyServerBuilder;
import io.grpc.stub.StreamObserver;

import systems.zlink.framework.locations.etcd.internal.proto.etcdserverpb.AuthGrpc;
import systems.zlink.framework.locations.etcd.internal.proto.etcdserverpb.KVGrpc;
import systems.zlink.framework.locations.etcd.internal.proto.etcdserverpb.Rpc.*;
import systems.zlink.framework.locations.etcd.internal.proto.mvccpb.Kv.KeyValue;

import java.nio.ByteBuffer;
import java.util.*;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.BiConsumer;

/** A deterministic MVCC server with fault injection at the RPC boundary. */
final class EtcdTestServer implements AutoCloseable {
    static final Comparator<ByteString> BYTES = ByteString.unsignedLexicographicalComparator();
    final NavigableMap<ByteString, KeyValue> records = new TreeMap<>(BYTES);
    final NavigableMap<Long, NavigableMap<ByteString, KeyValue>> history = new TreeMap<>();
    final AtomicInteger txnCalls = new AtomicInteger();
    final AtomicInteger rangeCalls = new AtomicInteger();
    final AtomicInteger authCalls = new AtomicInteger();
    final List<Long> deadlines = Collections.synchronizedList(new ArrayList<>());
    volatile BiConsumer<TxnRequest, StreamObserver<TxnResponse>> txnFault;
    volatile BiConsumer<RangeRequest, StreamObserver<RangeResponse>> rangeFault;
    volatile long compacted;
    volatile long revision = 1;
    volatile long cluster = 42;
    final Server server;

    EtcdTestServer() throws Exception {
        this(null);
    }

    EtcdTestServer(io.grpc.netty.shaded.io.netty.handler.ssl.SslContext tls) throws Exception {
        var builder = NettyServerBuilder.forPort(0).maxInboundMessageSize(16 * 1024 * 1024);
        if (tls != null) builder.sslContext(tls);
        server =
                builder.addService(
                                new KVGrpc.KVImplBase() {
                                    @Override
                                    public void txn(
                                            TxnRequest request, StreamObserver<TxnResponse> out) {
                                        txnCalls.incrementAndGet();
                                        recordDeadline();
                                        var fault = txnFault;
                                        if (fault != null) fault.accept(request, out);
                                        else transact(request, out);
                                    }

                                    @Override
                                    public void range(
                                            RangeRequest request,
                                            StreamObserver<RangeResponse> out) {
                                        rangeCalls.incrementAndGet();
                                        recordDeadline();
                                        var fault = rangeFault;
                                        if (fault != null) fault.accept(request, out);
                                        else respondRange(request, out);
                                    }
                                })
                        .addService(
                                new AuthGrpc.AuthImplBase() {
                                    @Override
                                    public void authenticate(
                                            AuthenticateRequest request,
                                            StreamObserver<AuthenticateResponse> out) {
                                        recordDeadline();
                                        out.onNext(
                                                AuthenticateResponse.newBuilder()
                                                        .setToken(
                                                                "token-"
                                                                        + authCalls
                                                                                .incrementAndGet())
                                                        .build());
                                        out.onCompleted();
                                    }
                                })
                        .build()
                        .start();
        clock(1000);
    }

    String endpoint() {
        return "http://localhost:" + server.getPort();
    }

    ZLinkEtcdLocationOptions options() {
        return new ZLinkEtcdLocationOptions()
                .setEndpoints(List.of(endpoint()))
                .setNamespace("test");
    }

    private void recordDeadline() {
        var deadline = Context.current().getDeadline();
        deadlines.add(
                deadline == null
                        ? Long.MAX_VALUE
                        : System.nanoTime() + deadline.timeRemaining(TimeUnit.NANOSECONDS));
    }

    static ByteString bytes(String value) {
        return ByteString.copyFromUtf8(value);
    }

    static ByteString clockBytes(long value) {
        return ByteString.copyFrom(ByteBuffer.allocate(8).putLong(value).array());
    }

    static ByteString metadata(boolean expiry, long time, int length) {
        return ByteString.copyFrom(
                ByteBuffer.allocate(14)
                        .put((byte) 1)
                        .put((byte) (expiry ? 1 : 0))
                        .putLong(time)
                        .putInt(length)
                        .array());
    }

    synchronized void clock(long time) {
        put(bytes("test\0c"), clockBytes(time));
    }

    synchronized void put(ByteString key, ByteString value) {
        revision++;
        write(key, value, revision);
        save();
    }

    synchronized void record(String key, byte[] value, Long expiry) {
        record("test", key, value, expiry);
    }

    synchronized void record(String namespace, String key, byte[] value, Long expiry) {
        revision++;
        write(
                bytes(namespace + "\0m\0" + key),
                metadata(expiry != null, expiry == null ? 0 : expiry, value.length),
                revision);
        write(bytes(namespace + "\0p\0" + key), ByteString.copyFrom(value), revision);
        save();
    }

    private void write(ByteString key, ByteString value, long rev) {
        var old = records.get(key);
        records.put(
                key,
                KeyValue.newBuilder()
                        .setKey(key)
                        .setValue(value)
                        .setCreateRevision(old == null ? rev : old.getCreateRevision())
                        .setModRevision(rev)
                        .setVersion(old == null ? 1 : old.getVersion() + 1)
                        .build());
    }

    private void save() {
        history.put(revision, new TreeMap<>(records));
    }

    private ResponseHeader header() {
        return ResponseHeader.newBuilder().setClusterId(cluster).setRevision(revision).build();
    }

    synchronized void transact(TxnRequest request, StreamObserver<TxnResponse> out) {
        try {
            boolean succeeded = request.getCompareList().stream().allMatch(this::compare);
            var ops = succeeded ? request.getSuccessList() : request.getFailureList();
            boolean writing =
                    ops.stream().anyMatch(op -> op.hasRequestPut() || op.hasRequestDeleteRange());
            if (writing) revision++;
            var response = TxnResponse.newBuilder().setSucceeded(succeeded);
            for (var op : ops) {
                if (op.hasRequestRange())
                    response.addResponses(
                            ResponseOp.newBuilder().setResponseRange(range(op.getRequestRange())));
                else if (op.hasRequestPut()) {
                    write(op.getRequestPut().getKey(), op.getRequestPut().getValue(), revision);
                    response.addResponses(
                            ResponseOp.newBuilder()
                                    .setResponsePut(PutResponse.getDefaultInstance()));
                } else if (op.hasRequestDeleteRange()) {
                    records.remove(op.getRequestDeleteRange().getKey());
                    response.addResponses(
                            ResponseOp.newBuilder()
                                    .setResponseDeleteRange(
                                            DeleteRangeResponse.getDefaultInstance()));
                }
            }
            if (writing) save();
            out.onNext(response.setHeader(header()).build());
            out.onCompleted();
        } catch (io.grpc.StatusRuntimeException error) {
            out.onError(error);
        }
    }

    private boolean compare(Compare comparison) {
        var current = records.get(comparison.getKey());
        int order =
                switch (comparison.getTarget()) {
                    case CREATE ->
                            Long.compare(
                                    current == null ? 0 : current.getCreateRevision(),
                                    comparison.getCreateRevision());
                    case MOD ->
                            Long.compare(
                                    current == null ? 0 : current.getModRevision(),
                                    comparison.getModRevision());
                    case VALUE ->
                            BYTES.compare(
                                    current == null ? ByteString.EMPTY : current.getValue(),
                                    comparison.getValue());
                    case VERSION ->
                            Long.compare(
                                    current == null ? 0 : current.getVersion(),
                                    comparison.getVersion());
                    default -> throw new AssertionError("Unsupported test comparison");
                };
        return switch (comparison.getResult()) {
            case EQUAL -> order == 0;
            case NOT_EQUAL -> order != 0;
            case LESS -> order < 0;
            case GREATER -> order > 0;
            default -> false;
        };
    }

    synchronized void respondRange(RangeRequest request, StreamObserver<RangeResponse> out) {
        try {
            out.onNext(range(request));
            out.onCompleted();
        } catch (io.grpc.StatusRuntimeException error) {
            out.onError(error);
        }
    }

    private RangeResponse range(RangeRequest request) {
        if (request.getRevision() > 0 && request.getRevision() <= compacted)
            throw Status.OUT_OF_RANGE
                    .withDescription("etcdserver: mvcc: required revision has been compacted")
                    .asRuntimeException();
        var source =
                request.getRevision() == 0
                        ? records
                        : history.floorEntry(request.getRevision()).getValue();
        var response = RangeResponse.newBuilder().setHeader(header());
        if (request.getRangeEnd().isEmpty()) {
            var value = source.get(request.getKey());
            if (value != null) response.addKvs(value);
        } else {
            var values = source.subMap(request.getKey(), true, request.getRangeEnd(), false);
            int count = 0;
            for (var value : values.values()) {
                if (request.getLimit() > 0 && count == request.getLimit()) {
                    response.setMore(true);
                    break;
                }
                response.addKvs(value);
                count++;
            }
        }
        return response.build();
    }

    @Override
    public void close() throws Exception {
        server.shutdownNow();
        if (!server.awaitTermination(5, TimeUnit.SECONDS))
            throw new AssertionError("Test gRPC server did not stop");
    }
}
