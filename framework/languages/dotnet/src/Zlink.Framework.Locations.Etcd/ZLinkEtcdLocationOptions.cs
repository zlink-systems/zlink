namespace Zlink.Framework.Locations.Etcd;

public sealed class ZLinkEtcdClientIdentity
{
    public required string CertificatePem { get; init; }
    public required string KeyPem { get; init; }
}

public sealed class ZLinkEtcdTlsOptions
{
    public string? TrustedCaPem { get; set; }
    public ZLinkEtcdClientIdentity? ClientIdentity { get; set; }
}

public sealed class ZLinkEtcdCredentials
{
    public required string User { get; init; }
    public required string Password { get; init; }
}

public sealed class ZLinkEtcdLocationOptions
{
    public IList<string> Endpoints { get; } = new List<string>();
    public ZLinkEtcdTlsOptions? Tls { get; set; }
    public ZLinkEtcdCredentials? Credentials { get; set; }
    public string Namespace { get; set; } = string.Empty;
    public TimeSpan OperationTimeout { get; set; } = TimeSpan.FromSeconds(5);
}
