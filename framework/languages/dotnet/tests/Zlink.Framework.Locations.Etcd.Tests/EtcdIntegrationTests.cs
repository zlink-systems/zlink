using System.Diagnostics;
using System.Text;
using System.Text.Json;

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
