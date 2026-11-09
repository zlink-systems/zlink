using System.Net.Security;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using Etcdserverpb;
using Grpc.Core;
using Grpc.Net.Client;

namespace Zlink.Framework.Locations.Etcd;

internal enum EtcdErrorKind
{
    Compaction,
    NoSpace,
    Authentication,
    StoreFailure,
}

internal sealed class EtcdCallException(EtcdErrorKind kind)
    : IOException("The Location Store operation failed; its write outcome may be unknown.")
{
    internal EtcdErrorKind Kind { get; } = kind;
}

internal sealed class EtcdOperation(DateTime deadline, CancellationToken cancellationToken)
{
    internal DateTime Deadline { get; } = deadline;
    internal CancellationToken CancellationToken { get; } = cancellationToken;
    internal bool Started { get; set; }

    internal void Check()
    {
        if (CancellationToken.IsCancellationRequested)
        {
            if (Started)
                throw EtcdEncoding.Failure();
            CancellationToken.ThrowIfCancellationRequested();
        }
        if (DateTime.UtcNow >= Deadline)
            throw EtcdEncoding.Failure();
    }
}

internal sealed class EtcdClient : IDisposable
{
    private readonly GrpcChannel[] channels;
    private readonly KV.KVClient[] kv;
    private readonly Auth.AuthClient[] auth;
    private readonly ZLinkEtcdCredentials? credentials;
    private readonly X509Certificate2Collection certificates = new();
    private int endpoint;
    private string? token;

    internal EtcdClient(ZLinkEtcdLocationOptions options)
    {
        ArgumentNullException.ThrowIfNull(options);
        if (options.Namespace is null || options.Namespace.Contains('\0'))
            throw new ArgumentException("Invalid Store namespace.", nameof(options));
        var ns = EtcdEncoding.Utf8.GetByteCount(options.Namespace);
        if (
            ns is < 1 or > 64
            || options.Endpoints.Count == 0
            || options.OperationTimeout <= TimeSpan.Zero
        )
            throw new ArgumentException("Invalid Store options.", nameof(options));
        var endpoints = options
            .Endpoints.Select(value =>
            {
                if (
                    !Uri.TryCreate(value, UriKind.Absolute, out var uri)
                    || uri.Scheme is not ("http" or "https")
                    || uri.Host.Length == 0
                    || uri.UserInfo.Length != 0
                    || uri.Query.Length != 0
                    || uri.Fragment.Length != 0
                    || uri.AbsolutePath != "/"
                )
                    throw new ArgumentException("Invalid Store endpoint.", nameof(options));
                return uri;
            })
            .ToArray();
        if (
            endpoints.Any(uri => uri.Scheme != endpoints[0].Scheme)
            || (options.Tls is not null && endpoints[0].Scheme != "https")
            || (
                options.Credentials is { } c
                && (string.IsNullOrEmpty(c.User) || string.IsNullOrEmpty(c.Password))
            )
        )
            throw new ArgumentException("Invalid Store transport options.", nameof(options));
        credentials = options.Credentials;
        var handler = new SocketsHttpHandler { EnableMultipleHttp2Connections = true };
        try
        {
            if (options.Tls?.TrustedCaPem is { } ca)
            {
                var firstRoot = certificates.Count;
                certificates.ImportFromPem(ca);
                var roots = new X509Certificate2Collection(
                    certificates.Cast<X509Certificate2>().Skip(firstRoot).ToArray()
                );
                if (roots.Count == 0)
                    throw new CryptographicException();
                handler.SslOptions.RemoteCertificateValidationCallback = (
                    _,
                    certificate,
                    peerChain,
                    errors
                ) =>
                {
                    if (
                        certificate is null
                        || (errors & SslPolicyErrors.RemoteCertificateNameMismatch) != 0
                    )
                        return false;
                    using var chain = new X509Chain();
                    chain.ChainPolicy.TrustMode = X509ChainTrustMode.CustomRootTrust;
                    chain.ChainPolicy.CustomTrustStore.AddRange(roots);
                    chain.ChainPolicy.RevocationMode = X509RevocationMode.NoCheck;
                    chain.ChainPolicy.ApplicationPolicy.Add(new Oid("1.3.6.1.5.5.7.3.1"));
                    if (peerChain is not null)
                        foreach (var element in peerChain.ChainElements)
                            chain.ChainPolicy.ExtraStore.Add(element.Certificate);
                    using var peer = new X509Certificate2(certificate);
                    return chain.Build(peer);
                };
            }
            if (options.Tls?.ClientIdentity is { } identity)
            {
                if (
                    string.IsNullOrEmpty(identity.CertificatePem)
                    || string.IsNullOrEmpty(identity.KeyPem)
                )
                    throw new CryptographicException();
                using var pem = X509Certificate2.CreateFromPem(
                    identity.CertificatePem,
                    identity.KeyPem
                );
                var certificate = new X509Certificate2(pem.Export(X509ContentType.Pkcs12));
                certificates.Add(certificate);
                var firstChainCertificate = certificates.Count;
                certificates.ImportFromPem(identity.CertificatePem);
                var chain = new X509Certificate2Collection(
                    certificates.Cast<X509Certificate2>().Skip(firstChainCertificate).ToArray()
                );
                handler.SslOptions.ClientCertificateContext = SslStreamCertificateContext.Create(
                    certificate,
                    chain,
                    offline: true
                );
            }
        }
        catch (CryptographicException)
        {
            handler.Dispose();
            foreach (var certificate in certificates)
                certificate.Dispose();
            throw new ArgumentException("Invalid Store TLS material.", nameof(options));
        }
        // .NET 8 enables cleartext HTTP/2 for an http GrpcChannel without a global switch.
        channels = endpoints
            .Select(uri =>
                GrpcChannel.ForAddress(
                    uri,
                    new GrpcChannelOptions
                    {
                        HttpHandler = handler,
                        DisposeHttpClient = false,
                        MaxSendMessageSize = 16 * 1024 * 1024,
                        MaxReceiveMessageSize = 8 * 1024 * 1024,
                    }
                )
            )
            .ToArray();
        transport = handler;
        kv = channels.Select(channel => new KV.KVClient(channel)).ToArray();
        auth = channels.Select(channel => new Auth.AuthClient(channel)).ToArray();
    }

    private readonly SocketsHttpHandler transport;

    internal static EtcdErrorKind Classify(StatusCode code, string description) =>
        (code, description) switch
        {
            (StatusCode.OutOfRange, var text)
                when text.Contains(
                    "required revision has been compacted",
                    StringComparison.Ordinal
                ) => EtcdErrorKind.Compaction,
            (StatusCode.ResourceExhausted, var text)
                when text.Contains("database space exceeded", StringComparison.Ordinal) =>
                EtcdErrorKind.NoSpace,
            (StatusCode.Unauthenticated, var text)
                when text.Contains("invalid auth token", StringComparison.Ordinal) =>
                EtcdErrorKind.Authentication,
            (StatusCode.InvalidArgument, var text)
                when text.Contains("revision of auth store is old", StringComparison.Ordinal) =>
                EtcdErrorKind.Authentication,
            _ => EtcdErrorKind.StoreFailure,
        };

    private async Task<T> Invoke<T>(
        EtcdOperation operation,
        Func<int, Metadata, AsyncUnaryCall<T>> call
    )
    {
        operation.Check();
        var index = Volatile.Read(ref endpoint);
        var headers = new Metadata();
        var currentToken = Volatile.Read(ref token);
        if (currentToken is not null)
            headers.Add("token", currentToken);
        operation.Started = true;
        try
        {
            using var pending = call(index, headers);
            return await pending.ResponseAsync.ConfigureAwait(false);
        }
        catch (RpcException error)
        {
            if (error.StatusCode is StatusCode.Unavailable or StatusCode.DeadlineExceeded)
                Interlocked.CompareExchange(ref endpoint, (index + 1) % channels.Length, index);
            throw new EtcdCallException(Classify(error.StatusCode, error.Status.Detail));
        }
        catch (OperationCanceledException)
        {
            throw EtcdEncoding.Failure();
        }
    }

    private async Task Authenticate(EtcdOperation operation)
    {
        if (credentials is null)
            return;
        var result = await Invoke(
                operation,
                (index, _) =>
                    auth[index]
                        .AuthenticateAsync(
                            new AuthenticateRequest
                            {
                                Name = credentials.User,
                                Password = credentials.Password,
                            },
                            deadline: operation.Deadline,
                            cancellationToken: operation.CancellationToken
                        )
            )
            .ConfigureAwait(false);
        Volatile.Write(ref token, result.Token);
    }

    private async Task<T> Call<T>(
        EtcdOperation operation,
        Func<int, Metadata, AsyncUnaryCall<T>> call
    )
    {
        if (credentials is not null && Volatile.Read(ref token) is null)
            await Authenticate(operation).ConfigureAwait(false);
        try
        {
            return await Invoke(operation, call).ConfigureAwait(false);
        }
        catch (EtcdCallException error)
            when (error.Kind == EtcdErrorKind.Authentication && credentials is not null)
        {
            await Authenticate(operation).ConfigureAwait(false);
            return await Invoke(operation, call).ConfigureAwait(false);
        }
    }

    internal Task<TxnResponse> Txn(TxnRequest request, EtcdOperation operation) =>
        Call(
            operation,
            (index, headers) =>
                kv[index]
                    .TxnAsync(request, headers, operation.Deadline, operation.CancellationToken)
        );

    internal Task<RangeResponse> Range(RangeRequest request, EtcdOperation operation) =>
        Call(
            operation,
            (index, headers) =>
                kv[index]
                    .RangeAsync(request, headers, operation.Deadline, operation.CancellationToken)
        );

    public void Dispose()
    {
        foreach (var channel in channels)
            channel.Dispose();
        transport.Dispose();
        foreach (var certificate in certificates)
            certificate.Dispose();
    }
}
