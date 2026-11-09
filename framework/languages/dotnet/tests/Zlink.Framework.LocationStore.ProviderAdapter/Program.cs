using System.Collections.Concurrent;
using System.Globalization;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Locations.Etcd;

Console.InputEncoding = Encoding.UTF8;
Console.OutputEncoding = new UTF8Encoding(false);
await new ProviderAdapter().RunAsync();

sealed class ProviderAdapter
{
    private readonly ConcurrentDictionary<string, ZLinkEtcdLocationStore> stores = new();
    private readonly ConcurrentDictionary<string, CancellationTokenSource> operations = new();
    private readonly HashSet<string> seen = new(StringComparer.Ordinal);
    private readonly object output = new();

    public async Task RunAsync()
    {
        var pending = new List<Task>();
        while (await Console.In.ReadLineAsync() is { } line)
        {
            try
            {
                using var document = JsonDocument.Parse(line);
                pending.Add(DispatchAsync(document.RootElement.Clone()));
                pending.RemoveAll(task => task.IsCompleted);
            }
            catch (JsonException error)
            {
                Reply(null, new { ok = false, error = Error(error, "adapter-error", null) });
            }
        }
        await Task.WhenAll(pending);
        foreach (var store in stores.Values)
            await store.DisposeAsync();
    }

    private void Reply(string? id, object body)
    {
        var fields = JsonSerializer.SerializeToElement(body);
        var response = new Dictionary<string, object?> { ["v"] = 1, ["id"] = id };
        foreach (var field in fields.EnumerateObject())
            response.Add(field.Name, field.Value);
        lock (output)
            Console.WriteLine(JsonSerializer.Serialize(response));
    }

    private static object Error(Exception error, string category, string? commit) =>
        commit is null
            ? new
            {
                category,
                nativeType = error.GetType().Name,
                message = error.Message,
            }
            : new
            {
                category,
                commit,
                nativeType = error.GetType().Name,
                message = error.Message,
            };

    private async Task DispatchAsync(JsonElement message)
    {
        string? id = null;
        string? op = null;
        var provider = false;
        CancellationTokenSource? cancellation = null;
        try
        {
            id = String(message, "id");
            if (message.GetProperty("v").GetInt32() != 1 || !seen.Add(id))
                throw new InvalidOperationException(
                    "Invalid protocol version or duplicate request id."
                );
            op = String(message, "op");
            if (op == "cancel")
            {
                var active = operations.TryGetValue(String(message, "target"), out var target);
                // Cancellation and disposal can race with a completed operation.
                if (target is not null)
                    lock (target)
                        if (
                            operations.TryGetValue(String(message, "target"), out var current)
                            && ReferenceEquals(current, target)
                        )
                            target.Cancel();
                Reply(id, new { ok = true, result = new { kind = "cancelled", active } });
                return;
            }
            var storeId = String(message, "store");
            if (op == "create")
            {
                if (stores.ContainsKey(storeId))
                    throw new InvalidOperationException("Duplicate store.");
                var options = Options(message.GetProperty("options"));
                provider = true;
                stores[storeId] = new ZLinkEtcdLocationStore(options);
                Reply(id, new { ok = true, result = new { kind = "created" } });
                return;
            }
            if (!stores.TryGetValue(storeId, out var store))
                throw new InvalidOperationException("Unknown store.");
            object request = op switch
            {
                "read" => new ZLinkStoreKey(String(message, "key")),
                "write" => WriteRequest(message.GetProperty("request")),
                "scan" => ScanRequest(message.GetProperty("request")),
                "dispose" => storeId,
                _ => throw new InvalidOperationException("Unknown operation."),
            };
            cancellation = new CancellationTokenSource();
            operations[id] = cancellation;
            provider = true;
            object result;
            if (op == "dispose")
            {
                await store.DisposeAsync();
                stores.TryRemove(storeId, out _);
                result = new { kind = "disposed" };
            }
            else
            {
                object native = op switch
                {
                    "read" => await store.ReadAsync((ZLinkStoreKey)request, cancellation.Token),
                    "write" => await store.WriteAsync(
                        (ZLinkStoreWriteRequest)request,
                        cancellation.Token
                    ),
                    "scan" => await store.ScanAsync(
                        (ZLinkStoreScanRequest)request,
                        cancellation.Token
                    ),
                    _ => throw new InvalidOperationException("Unknown operation."),
                };
                provider = false;
                result = Result(native);
            }
            Reply(id, new { ok = true, result });
        }
        catch (Exception error)
        {
            var category = provider
                ? error switch
                {
                    ArgumentException => "argument-error",
                    IOException or OperationCanceledException => "store-failure",
                    _ => "adapter-error",
                }
                : "adapter-error";
            Reply(
                id,
                new
                {
                    ok = false,
                    error = Error(
                        error,
                        category,
                        category == "store-failure"
                            // The provider cancels without I/O only before the first RPC.
                            ? op == "write" && error is not OperationCanceledException
                                ? "unknown"
                                : "none"
                            : null
                    ),
                }
            );
        }
        finally
        {
            if (cancellation is not null && id is not null)
                lock (cancellation)
                {
                    operations.TryRemove(id, out _);
                    cancellation.Dispose();
                }
        }
    }

    private static string String(JsonElement value, string name) =>
        value.GetProperty(name).GetString()
        ?? throw new FormatException($"Expected string: {name}.");

    private static long Decimal(JsonElement value)
    {
        var text = value.GetString() ?? throw new FormatException("Expected decimal milliseconds.");
        if (!Regex.IsMatch(text, @"^-?(0|[1-9][0-9]*)$", RegexOptions.CultureInvariant))
            throw new FormatException("Expected decimal milliseconds.");
        return long.Parse(text, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture);
    }

    private static byte[] Bytes(JsonElement value)
    {
        var text = value.GetString() ?? throw new FormatException("Expected base64.");
        var bytes = Convert.FromBase64String(text);
        if (Convert.ToBase64String(bytes) != text)
            throw new FormatException("Expected canonical base64.");
        return bytes;
    }

    private static ZLinkEtcdLocationOptions Options(JsonElement input)
    {
        var options = new ZLinkEtcdLocationOptions { Namespace = String(input, "namespace") };
        foreach (var endpoint in input.GetProperty("endpoints").EnumerateArray())
            options.Endpoints.Add(
                endpoint.GetString() ?? throw new FormatException("Expected endpoint.")
            );
        if (input.TryGetProperty("operationTimeoutMs", out var timeout))
            options.OperationTimeout = TimeSpan.FromMilliseconds(timeout.GetInt64());
        if (input.TryGetProperty("credentials", out var credentials))
            options.Credentials = new()
            {
                User = String(credentials, "user"),
                Password = String(credentials, "password"),
            };
        if (input.TryGetProperty("tls", out var tls))
        {
            options.Tls = new();
            if (tls.TryGetProperty("trustedCaPem", out var ca))
                options.Tls.TrustedCaPem = ca.GetString();
            if (tls.TryGetProperty("clientIdentity", out var identity))
                options.Tls.ClientIdentity = new()
                {
                    CertificatePem = String(identity, "certificatePem"),
                    KeyPem = String(identity, "keyPem"),
                };
        }
        return options;
    }

    private static ZLinkStoreWriteRequest WriteRequest(JsonElement request) =>
        new(
            request
                .GetProperty("conditions")
                .EnumerateArray()
                .Select<JsonElement, ZLinkStoreCondition>(condition =>
                {
                    var key = new ZLinkStoreKey(String(condition, "key"));
                    return String(condition, "kind") switch
                    {
                        "missing" => new ZLinkStoreCondition.Missing(key),
                        "version" => new ZLinkStoreCondition.Version(
                            key,
                            new(String(condition, "version"))
                        ),
                        "value" => new ZLinkStoreCondition.Value(
                            key,
                            Bytes(condition.GetProperty("bytesBase64"))
                        ),
                        _ => throw new FormatException("Unknown condition kind."),
                    };
                })
                .ToArray(),
            request
                .GetProperty("mutations")
                .EnumerateArray()
                .Select<JsonElement, ZLinkStoreMutation>(mutation =>
                {
                    var key = new ZLinkStoreKey(String(mutation, "key"));
                    return String(mutation, "kind") switch
                    {
                        "delete" => new ZLinkStoreMutation.Delete(key),
                        "put" => new ZLinkStoreMutation.Put(
                            key,
                            Bytes(mutation.GetProperty("bytesBase64")),
                            mutation.TryGetProperty("retentionMs", out var retention)
                                ? TimeSpan.FromMilliseconds(Decimal(retention))
                                : null
                        ),
                        _ => throw new FormatException("Unknown mutation kind."),
                    };
                })
                .ToArray()
        );

    private static ZLinkStoreScanRequest ScanRequest(JsonElement request) =>
        new(
            String(request, "prefix"),
            request.TryGetProperty("cursor", out var cursor)
            && cursor.ValueKind != JsonValueKind.Null
                ? new(cursor.GetString() ?? throw new FormatException("Expected cursor."))
                : null,
            request.GetProperty("limit").GetInt32()
        );

    private static string Milliseconds(DateTimeOffset value) =>
        value.ToUnixTimeMilliseconds().ToString(CultureInfo.InvariantCulture);

    private static object Value(ZLinkStoreValue value) =>
        new
        {
            bytesBase64 = Convert.ToBase64String(value.Bytes.Span),
            version = value.Version.Value,
            expiresAtMs = value.ExpiresAt is { } expiry ? Milliseconds(expiry) : null,
            storeNowMs = Milliseconds(value.StoreNow),
        };

    private static object Result(object result) =>
        result switch
        {
            ZLinkStoreReadResult.Found found => new { kind = "found", value = Value(found.Value) },
            ZLinkStoreReadResult.Missing missing => new
            {
                kind = "missing",
                storeNowMs = Milliseconds(missing.StoreNow),
            },
            ZLinkStoreWriteResult.Conflict conflict => new
            {
                kind = "conflict",
                storeNowMs = Milliseconds(conflict.StoreNow),
            },
            ZLinkStoreWriteResult.Applied applied => new
            {
                kind = "applied",
                storeNowMs = Milliseconds(applied.StoreNow),
                putVersions = applied
                    .PutVersions.Select(item => new
                    {
                        key = item.Key.Value,
                        version = item.Value.Value,
                    })
                    .ToArray(),
            },
            ZLinkStoreScanResult.Expired => new { kind = "expired" },
            ZLinkStoreScanResult.Page page => new
            {
                kind = "page",
                value = new
                {
                    storeNowMs = Milliseconds(page.Value.StoreNow),
                    nextCursor = page.Value.NextCursor?.Value,
                    items = page
                        .Value.Items.Select(item => new
                        {
                            key = item.Key.Value,
                            value = Value(item.Value),
                        })
                        .ToArray(),
                },
            },
            _ => throw new InvalidOperationException("Unknown public result."),
        };
}
