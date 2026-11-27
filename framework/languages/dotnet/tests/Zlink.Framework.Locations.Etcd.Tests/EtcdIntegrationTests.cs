extern alias etcdprovider;
using System.Diagnostics;
using System.Text;
using System.Text.Json;

using Google.Protobuf;
using ClientIdentity = etcdprovider::Zlink.Framework.Locations.Etcd.ZLinkEtcdClientIdentity;
using ClientOperation = etcdprovider::Zlink.Framework.Locations.Etcd.EtcdOperation;
using ClientOptions = etcdprovider::Zlink.Framework.Locations.Etcd.ZLinkEtcdLocationOptions;
using Rpc = etcdprovider::Etcdserverpb;
using TlsOptions = etcdprovider::Zlink.Framework.Locations.Etcd.ZLinkEtcdTlsOptions;

namespace Zlink.Framework.Locations.Etcd.Tests;

public sealed class EtcdIntegrationTests
{
    [SkippableFact]
    public async Task Real_cluster_supports_conditions_expiry_maximum_batch_and_snapshot_pages()
    {
        var endpoints = Environment.GetEnvironmentVariable("ZLINK_ETCD_ENDPOINTS");
        Skip.If(
            string.IsNullOrWhiteSpace(endpoints),
            "Set ZLINK_ETCD_ENDPOINTS to comma-separated endpoints of the scripts/etcd cluster."
        );
        var root = FindRoot();
        var ns = "dotnet-" + Guid.NewGuid().ToString("N");
        var directory = Path.Combine(
            root,
            "framework/languages/dotnet/tests/Zlink.Framework.Locations.Etcd.Tests/.test-runs",
            ns
        );
        Directory.CreateDirectory(directory);
        var config = Path.Combine(directory, "admin.json");
        var addresses = endpoints!.Split(
            ',',
            StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries
        );
        await File.WriteAllTextAsync(
            config,
            JsonSerializer.Serialize(new { endpoints = addresses, @namespace = ns })
        );
        Process? admin = null;
        try
        {
            using (var init = StartAdmin(root, "init", config))
            {
                await init.WaitForExitAsync();
                Assert.True(
                    init.ExitCode == 0,
                    "Node admin init failed: " + await init.StandardError.ReadToEndAsync()
                );
            }
            admin = StartAdmin(root, "run", config);
            await using var store = new ZLinkEtcdLocationStore(o =>
            {
                o.Namespace = ns;
                foreach (var address in addresses)
                    o.Endpoints.Add(address);
            });
            var key = new ZLinkStoreKey("basic");
            Assert.IsType<ZLinkStoreReadResult.Missing>(await store.ReadAsync(key));
            var initial = Assert.IsType<ZLinkStoreWriteResult.Applied>(
                await store.WriteAsync(
                    new(
                        [new ZLinkStoreCondition.Missing(key)],
                        [new ZLinkStoreMutation.Put(key, new byte[] { 1, 2 }, null)]
                    )
                )
            );
            var found = Assert.IsType<ZLinkStoreReadResult.Found>(await store.ReadAsync(key)).Value;
            Assert.Equal(new byte[] { 1, 2 }, found.Bytes.ToArray());
            Assert.Equal(initial.PutVersions[key], found.Version);
            Assert.IsType<ZLinkStoreWriteResult.Conflict>(
                await store.WriteAsync(
                    new(
                        [new ZLinkStoreCondition.Value(key, new byte[] { 1, 3 })],
                        [new ZLinkStoreMutation.Delete(key)]
                    )
                )
            );
            Assert.IsType<ZLinkStoreWriteResult.Applied>(
                await store.WriteAsync(
                    new(
                        [
                            new ZLinkStoreCondition.Version(key, found.Version),
                            new ZLinkStoreCondition.Missing(new("other")),
                        ],
                        [new ZLinkStoreMutation.Put(key, new byte[] { 3 }, null)]
                    )
                )
            );
            Assert.IsType<ZLinkStoreWriteResult.Conflict>(
                await store.WriteAsync(
                    new(
                        [new ZLinkStoreCondition.Version(key, found.Version)],
                        [new ZLinkStoreMutation.Delete(key)]
                    )
                )
            );
            Assert.IsType<ZLinkStoreWriteResult.Applied>(
                await store.WriteAsync(
                    new(
                        [new ZLinkStoreCondition.Value(key, new byte[] { 3 })],
                        [new ZLinkStoreMutation.Delete(key)]
                    )
                )
            );
            Assert.IsType<ZLinkStoreReadResult.Missing>(await store.ReadAsync(key));

            var expiring = new ZLinkStoreKey("expiry");
            var expiryWrite = Assert.IsType<ZLinkStoreWriteResult.Applied>(
                await store.WriteAsync(
                    new(
                        [],
                        [
                            new ZLinkStoreMutation.Put(
                                expiring,
                                new byte[] { 5 },
                                TimeSpan.FromMilliseconds(20)
                            ),
                        ]
                    )
                )
            );
            var expires = Assert
                .IsType<ZLinkStoreReadResult.Found>(await store.ReadAsync(expiring))
                .Value;
            Assert.Equal(expiryWrite.StoreNow.AddMilliseconds(20), expires.ExpiresAt);
            var stopwatch = Stopwatch.StartNew();
            while (await store.ReadAsync(expiring) is not ZLinkStoreReadResult.Missing)
            {
                Assert.True(
                    stopwatch.Elapsed < TimeSpan.FromSeconds(10),
                    "Node admin did not advance the Store clock."
                );
                Assert.False(admin.HasExited, "Node admin terminated unexpectedly.");
                await Task.Delay(50);
            }
            Assert.IsType<ZLinkStoreWriteResult.Conflict>(
                await store.WriteAsync(
                    new([new ZLinkStoreCondition.Version(expiring, expires.Version)], [])
                )
            );

            // The SPI byte accounting is sum(key bytes + payload bytes), exactly 4 MiB.
            var mutations = Enumerable
                .Range(0, 2048)
                .Select(i =>
                {
                    var batchKey = new ZLinkStoreKey("batch/" + i.ToString("D4"));
                    var bytes = new byte[2048 - Encoding.UTF8.GetByteCount(batchKey.Value)];
                    bytes[0] = (byte)(i % 251);
                    return (ZLinkStoreMutation)new ZLinkStoreMutation.Put(batchKey, bytes, null);
                })
                .ToArray();
            Assert.Equal(
                4 * 1024 * 1024,
                mutations
                    .Cast<ZLinkStoreMutation.Put>()
                    .Sum(p => Encoding.UTF8.GetByteCount(p.Key.Value) + p.Bytes.Length)
            );
            var batch = Assert.IsType<ZLinkStoreWriteResult.Applied>(
                await store.WriteAsync(new([], mutations))
            );
            Assert.Equal(2048, batch.PutVersions.Count);
            Assert.Single(batch.PutVersions.Values.Distinct());
            var first = Assert
                .IsType<ZLinkStoreScanResult.Page>(await store.ScanAsync(new("batch/", null, 1000)))
                .Value;
            Assert.Equal(1000, first.Items.Count);
            Assert.NotNull(first.NextCursor);
            Assert.True(Encoding.UTF8.GetByteCount(first.NextCursor!.Value.Value) <= 4096);
            Assert.IsType<ZLinkStoreWriteResult.Applied>(
                await store.WriteAsync(
                    new(
                        [],
                        [
                            new ZLinkStoreMutation.Delete(new("batch/1000")),
                            new ZLinkStoreMutation.Put(new("batch/1001"), new byte[] { 99 }, null),
                        ]
                    )
                )
            );
            var second = Assert
                .IsType<ZLinkStoreScanResult.Page>(
                    await store.ScanAsync(new("batch/", first.NextCursor, 1000))
                )
                .Value;
            Assert.Equal(first.StoreNow, second.StoreNow);
            Assert.Equal(1000, second.Items.Count);
            Assert.Equal("batch/1000", second.Items[0].Key.Value);
            Assert.Equal((byte)(1000 % 251), second.Items[0].Value.Bytes.Span[0]);
            Assert.Equal((byte)(1001 % 251), second.Items[1].Value.Bytes.Span[0]);
            var last = Assert
                .IsType<ZLinkStoreScanResult.Page>(
                    await store.ScanAsync(new("batch/", second.NextCursor, 1000))
                )
                .Value;
            Assert.Equal(48, last.Items.Count);
            Assert.Equal(first.StoreNow, last.StoreNow);
            Assert.Null(last.NextCursor);
        }
        finally
        {
            if (admin is not null)
            {
                if (!admin.HasExited)
                {
                    admin.Kill(entireProcessTree: true);
                    await admin.WaitForExitAsync();
                }
                admin.Dispose();
            }
            Directory.Delete(directory, recursive: true);
            var parent = Path.GetDirectoryName(directory)!;
            if (!Directory.EnumerateFileSystemEntries(parent).Any())
                Directory.Delete(parent);
        }
    }

    [Fact]
    public async Task Client_sends_keys_only_and_revision_in_range_request()
    {
        await using var server = await FakeEtcdServer.StartAsync();
        var observed = false;
        server.BeforeCall = (method, request, _) =>
        {
            if (method == "Range")
            {
                var range = Assert.IsType<Etcdserverpb.RangeRequest>(request);
                Assert.True(range.KeysOnly);
                Assert.Equal(1, range.Revision);
                observed = true;
            }
            return Task.CompletedTask;
        };
        var options = new ClientOptions { Namespace = "client-capability" };
        options.Endpoints.Add(server.Endpoint);
        using var client = new EtcdClient(options);
        await client.Range(
            new Rpc.RangeRequest
            {
                Key = ByteString.CopyFromUtf8("key"),
                Revision = 1,
                KeysOnly = true,
            },
            new ClientOperation(DateTime.UtcNow.AddSeconds(5), CancellationToken.None)
        );
        Assert.True(observed);
    }

    [SkippableFact]
    public async Task Authenticated_mutual_tls_preserves_revision_and_keys_only_omits_values()
    {
        string? Required(string name)
        {
            var value = Environment.GetEnvironmentVariable(name);
            Skip.If(string.IsNullOrWhiteSpace(value), "Set " + name + " for secure etcd tests.");
            return value;
        }

        var endpoint = Required("ZLINK_ETCD_CLIENT_ENDPOINT")!;
        var user = Required("ZLINK_ETCD_CLIENT_USER")!;
        var password = Required("ZLINK_ETCD_CLIENT_PASSWORD")!;
        var ca = Required("ZLINK_ETCD_CLIENT_CA")!;
        var cert = Required("ZLINK_ETCD_CLIENT_CERT")!;
        var privateKey = Required("ZLINK_ETCD_CLIENT_KEY")!;
        var options = new ClientOptions
        {
            Namespace = "client-capability",
            Credentials = new() { User = user, Password = password },
            Tls = new TlsOptions
            {
                TrustedCaPem = await File.ReadAllTextAsync(ca),
                ClientIdentity = new ClientIdentity
                {
                    CertificatePem = await File.ReadAllTextAsync(cert),
                    KeyPem = await File.ReadAllTextAsync(privateKey),
                },
            },
        };
        options.Endpoints.Add(endpoint);
        using var client = new EtcdClient(options);
        var operation = new ClientOperation(DateTime.UtcNow.AddSeconds(10), CancellationToken.None);
        var key = ByteString.CopyFromUtf8("client-capability/" + Guid.NewGuid().ToString("N"));
        var payload = ByteString.CopyFrom(new byte[] { 1, 0, 255 });
        try
        {
            var written = await client.Txn(
                new Rpc.TxnRequest
                {
                    Success =
                    {
                        new Rpc.RequestOp
                        {
                            RequestPut = new Rpc.PutRequest { Key = key, Value = payload },
                        },
                    },
                },
                operation
            );
            var revision = written.Header.Revision;
            await client.Txn(
                new Rpc.TxnRequest
                {
                    Success =
                    {
                        new Rpc.RequestOp
                        {
                            RequestPut = new Rpc.PutRequest
                            {
                                Key = key,
                                Value = ByteString.CopyFromUtf8("replacement"),
                            },
                        },
                    },
                },
                operation
            );
            var full = await client.Range(
                new Rpc.RangeRequest { Key = key, Revision = revision },
                operation
            );
            var keys = await client.Range(
                new Rpc.RangeRequest
                {
                    Key = key,
                    Revision = revision,
                    KeysOnly = true,
                },
                operation
            );
            var fullValue = Assert.Single(full.Kvs);
            var keyOnly = Assert.Single(keys.Kvs);
            Assert.Equal(payload, fullValue.Value);
            Assert.Equal(revision, fullValue.ModRevision);
            Assert.True(keyOnly.Value.IsEmpty);
            var expected = fullValue.Clone();
            expected.Value = ByteString.Empty;
            Assert.Equal(expected, keyOnly);
            Assert.Equal(full.Header.ClusterId, keys.Header.ClusterId);
        }
        finally
        {
            await client.Txn(
                new Rpc.TxnRequest
                {
                    Success =
                    {
                        new Rpc.RequestOp
                        {
                            RequestDeleteRange = new Rpc.DeleteRangeRequest { Key = key },
                        },
                    },
                },
                operation
            );
        }
    }

    private static Process StartAdmin(string root, string command, string config)
    {
        var start = new ProcessStartInfo("node")
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardError = true,
            RedirectStandardOutput = true,
        };
        start.ArgumentList.Add(
            Path.Combine(
                root,
                "framework/languages/node/packages/framework-locations-etcd/dist/admin.js"
            )
        );
        start.ArgumentList.Add(command);
        start.ArgumentList.Add("--config");
        start.ArgumentList.Add(config);
        return Process.Start(start)
            ?? throw new IOException("Could not start the Node Store admin process.");
    }

    private static string FindRoot()
    {
        for (
            var directory = new DirectoryInfo(AppContext.BaseDirectory);
            directory is not null;
            directory = directory.Parent
        )
            if (File.Exists(Path.Combine(directory.FullName, "scripts/etcd/etcd-cluster.mjs")))
                return directory.FullName;
        throw new DirectoryNotFoundException("Repository root not found.");
    }
}
