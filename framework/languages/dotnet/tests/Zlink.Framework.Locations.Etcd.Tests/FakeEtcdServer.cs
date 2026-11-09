using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.Net;
using System.Net.Security;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using Etcdserverpb;
using Google.Protobuf;
using Grpc.Core;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Hosting.Server;
using Microsoft.AspNetCore.Hosting.Server.Features;
using Microsoft.AspNetCore.Server.Kestrel.Core;
using Microsoft.AspNetCore.Server.Kestrel.Https;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Mvccpb;

namespace Zlink.Framework.Locations.Etcd.Tests;

// This model owns only the etcd wire contract needed by provider tests.
// Hooks run before a request is applied, allowing deterministic failures and races.
internal sealed class FakeEtcdServer : IAsyncDisposable
{
    private readonly WebApplication app;
    private readonly object sync = new();
    private readonly SortedDictionary<long, Dictionary<string, KeyValue>> history = new();
    private Dictionary<string, KeyValue> current = new(StringComparer.Ordinal);
    internal long Revision { get; private set; } = 1;
    internal ulong ClusterId { get; set; } = 0xfedcba9876543210;
    internal string Endpoint { get; private set; } = "";
    internal long CompactedRevision { get; set; }
    internal ConcurrentQueue<(string Method, DateTime Deadline)> Calls { get; } = new();
    internal Func<string, IMessage, ServerCallContext, Task>? BeforeCall { get; set; }
    internal int AuthenticateCount;
    internal int WriteCount;

    private FakeEtcdServer(WebApplication app)
    {
        this.app = app;
        history[Revision] = new(StringComparer.Ordinal);
    }

    internal static async Task<FakeEtcdServer> StartAsync(
        X509Certificate2? tlsCertificate = null,
        Func<X509Certificate2, X509Chain?, SslPolicyErrors, bool>? validateClientCertificate = null
    )
    {
        var builder = WebApplication.CreateBuilder();
        builder.Logging.ClearProviders();
        builder.WebHost.ConfigureKestrel(options =>
            options.Listen(
                IPAddress.Loopback,
                0,
                listen =>
                {
                    listen.Protocols = HttpProtocols.Http2;
                    if (tlsCertificate is not null)
                        listen.UseHttps(https =>
                        {
                            https.ServerCertificate = tlsCertificate;
                            https.ClientCertificateMode = ClientCertificateMode.RequireCertificate;
                            https.ClientCertificateValidation = validateClientCertificate;
                        });
                }
            )
        );
        builder.Services.AddGrpc(options =>
        {
            options.MaxReceiveMessageSize = 16 * 1024 * 1024;
            options.MaxSendMessageSize = 8 * 1024 * 1024;
        });
        FakeEtcdServer? server = null;
        builder.Services.AddSingleton(_ => server!);
        var app = builder.Build();
        server = new FakeEtcdServer(app);
        app.MapGrpcService<KvService>();
        app.MapGrpcService<AuthService>();
        await app.StartAsync();
        server.Endpoint = app
            .Services.GetRequiredService<IServer>()
            .Features.Get<IServerAddressesFeature>()!
            .Addresses.Single();
        return server;
    }

    internal ZLinkEtcdLocationStore CreateStore(
        string ns = "test",
        TimeSpan? timeout = null,
        ZLinkEtcdCredentials? credentials = null,
        params string[] followingEndpoints
    ) =>
        new(options =>
        {
            options.Namespace = ns;
            options.Endpoints.Add(Endpoint);
            foreach (var endpoint in followingEndpoints)
                options.Endpoints.Add(endpoint);
            options.OperationTimeout = timeout ?? TimeSpan.FromSeconds(5);
            options.Credentials = credentials;
        });

    internal static byte[] Key(string ns, string part, string? key = null) =>
        Encoding.UTF8.GetBytes(ns + "\0" + part + (key is null ? "" : "\0" + key));

    internal void Clock(ulong now = 1000, string ns = "test")
    {
        var bytes = new byte[8];
        BinaryPrimitives.WriteUInt64BigEndian(bytes, now);
        Seed(Key(ns, "c"), bytes);
    }

    internal void Record(string key, byte[] value, ulong? expiry = null, string ns = "test")
    {
        var metadata = new byte[14];
        metadata[0] = 1;
        metadata[1] = expiry.HasValue ? (byte)1 : (byte)0;
        BinaryPrimitives.WriteUInt64BigEndian(metadata.AsSpan(2), expiry ?? 0);
        BinaryPrimitives.WriteUInt32BigEndian(metadata.AsSpan(10), (uint)value.Length);
        SeedMany((Key(ns, "m", key), metadata), (Key(ns, "p", key), value));
    }

    internal void Seed(byte[] key, byte[] value, long? modRevision = null) =>
        SeedManyCore(modRevision, (key, value));

    private void SeedMany(params (byte[] Key, byte[] Value)[] values) => SeedManyCore(null, values);

    private void SeedManyCore(long? modRevision, params (byte[] Key, byte[] Value)[] values)
    {
        lock (sync)
        {
            Revision++;
            foreach (var (key, value) in values)
            {
                var hex = Convert.ToHexString(key);
                current[hex] = new KeyValue
                {
                    Key = ByteString.CopyFrom(key),
                    Value = ByteString.CopyFrom(value),
                    CreateRevision = current.TryGetValue(hex, out var previous)
                        ? previous.CreateRevision
                        : Revision,
                    ModRevision = modRevision ?? Revision,
                    Version = 1,
                };
            }
            Snapshot();
        }
    }

    private void Snapshot() =>
        history[Revision] = current.ToDictionary(
            p => p.Key,
            p => p.Value.Clone(),
            StringComparer.Ordinal
        );

    private ResponseHeader Header() => new() { ClusterId = ClusterId, Revision = Revision };

    private async Task Before(string method, IMessage request, ServerCallContext context)
    {
        Calls.Enqueue((method, context.Deadline));
        if (BeforeCall is { } hook)
            await hook(method, request, context);
    }

    private RangeResponse RangeCore(RangeRequest request)
    {
        Assert.False(request.Serializable);
        if (request.Revision > 0 && request.Revision <= CompactedRevision)
            throw new RpcException(
                new Status(
                    StatusCode.OutOfRange,
                    "etcdserver: mvcc: required revision has been compacted"
                )
            );
        var source =
            request.Revision <= 0 ? current : history.Last(p => p.Key <= request.Revision).Value;
        var key = Convert.ToHexString(request.Key.Span);
        var end = Convert.ToHexString(request.RangeEnd.Span);
        var matches = source
            .Where(p =>
                end.Length == 0
                    ? p.Key == key
                    : string.CompareOrdinal(p.Key, key) >= 0
                        && (end == "00" || string.CompareOrdinal(p.Key, end) < 0)
            )
            .OrderBy(p => p.Key, StringComparer.Ordinal)
            .Select(p => p.Value)
            .ToArray();
        var count =
            request.Limit <= 0 ? matches.Length : Math.Min(matches.Length, (int)request.Limit);
        var response = new RangeResponse
        {
            Header = Header(),
            Count = matches.Length,
            More = count < matches.Length,
        };
        response.Kvs.AddRange(matches.Take(count).Select(kv => kv.Clone()));
        return response;
    }

    private bool Matches(Compare compare)
    {
        current.TryGetValue(Convert.ToHexString(compare.Key.Span), out var value);
        var comparison = compare.Target switch
        {
            Compare.Types.CompareTarget.Create => (value?.CreateRevision ?? 0).CompareTo(
                compare.CreateRevision
            ),
            Compare.Types.CompareTarget.Mod => (value?.ModRevision ?? 0).CompareTo(
                compare.ModRevision
            ),
            Compare.Types.CompareTarget.Version => (value?.Version ?? 0).CompareTo(compare.Version),
            Compare.Types.CompareTarget.Value => (
                value is null ? Array.Empty<byte>() : value.Value.ToByteArray()
            )
                .AsSpan()
                .SequenceCompareTo(compare.Value.Span),
            _ => throw new InvalidOperationException("Unexpected comparison"),
        };
        return compare.Result switch
        {
            Compare.Types.CompareResult.Equal => comparison == 0,
            Compare.Types.CompareResult.NotEqual => comparison != 0,
            Compare.Types.CompareResult.Less => comparison < 0,
            Compare.Types.CompareResult.Greater => comparison > 0,
            _ => throw new InvalidOperationException("Unexpected comparison result"),
        };
    }

    private TxnResponse TxnCore(TxnRequest request)
    {
        var succeeded = request.Compare.All(Matches);
        var operations = succeeded ? request.Success : request.Failure;
        var writes = operations.Any(op =>
            op.RequestPut is not null || op.RequestDeleteRange is not null
        );
        if (writes)
        {
            Revision++;
            Interlocked.Increment(ref WriteCount);
        }
        var response = new TxnResponse { Header = Header(), Succeeded = succeeded };
        foreach (var operation in operations)
        {
            if (operation.RequestRange is { } range)
                response.Responses.Add(new ResponseOp { ResponseRange = RangeCore(range) });
            else if (operation.RequestPut is { } put)
            {
                var hex = Convert.ToHexString(put.Key.Span);
                current.TryGetValue(hex, out var previous);
                current[hex] = new KeyValue
                {
                    Key = put.Key,
                    Value = put.Value,
                    ModRevision = Revision,
                    CreateRevision = previous?.CreateRevision ?? Revision,
                    Version = (previous?.Version ?? 0) + 1,
                };
                response.Responses.Add(
                    new ResponseOp { ResponsePut = new PutResponse { Header = Header() } }
                );
            }
            else if (operation.RequestDeleteRange is { } delete)
            {
                Assert.Empty(delete.RangeEnd);
                var removed = current.Remove(Convert.ToHexString(delete.Key.Span));
                response.Responses.Add(
                    new ResponseOp
                    {
                        ResponseDeleteRange = new DeleteRangeResponse
                        {
                            Header = Header(),
                            Deleted = removed ? 1 : 0,
                        },
                    }
                );
            }
            else
                throw new InvalidOperationException("Unexpected request operation");
        }
        if (writes)
            Snapshot();
        return response;
    }

    internal TxnResponse Apply(TxnRequest request)
    {
        lock (sync)
            return TxnCore(request);
    }

    public async ValueTask DisposeAsync() => await app.DisposeAsync();

    internal sealed class KvService : KV.KVBase
    {
        private readonly FakeEtcdServer owner;

        public KvService(FakeEtcdServer owner) => this.owner = owner;

        public override async Task<RangeResponse> Range(
            RangeRequest request,
            ServerCallContext context
        )
        {
            await owner.Before("Range", request, context);
            lock (owner.sync)
                return owner.RangeCore(request);
        }

        public override async Task<TxnResponse> Txn(TxnRequest request, ServerCallContext context)
        {
            await owner.Before("Txn", request, context);
            lock (owner.sync)
                return owner.TxnCore(request);
        }
    }

    internal sealed class AuthService : Auth.AuthBase
    {
        private readonly FakeEtcdServer owner;

        public AuthService(FakeEtcdServer owner) => this.owner = owner;

        public override async Task<AuthenticateResponse> Authenticate(
            AuthenticateRequest request,
            ServerCallContext context
        )
        {
            await owner.Before("Authenticate", request, context);
            var number = Interlocked.Increment(ref owner.AuthenticateCount);
            return new AuthenticateResponse { Header = owner.Header(), Token = "token" + number };
        }
    }
}
