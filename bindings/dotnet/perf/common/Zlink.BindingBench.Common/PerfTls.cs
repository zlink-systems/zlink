using System;
using System.IO;
using Systems.Zlink;

public static class PerfTls
{
    public static bool IsSecureTransport(string transport)
    {
        return transport.Equals("tls", StringComparison.OrdinalIgnoreCase)
               || transport.Equals("wss", StringComparison.OrdinalIgnoreCase);
    }

    public static void ConfigureTlsServerIfNeeded(ISocket socket,
        string transport, bool verbose = true)
    {
        if (!IsSecureTransport(transport))
            return;

        if (!TryResolvePerfTlsPaths(out string certPath, out string keyPath,
                out _))
        {
            throw new InvalidOperationException(verbose
                ? "TLS certificate files not found under bindings/dotnet/tests/certs"
                : "TLS certificate files not found.");
        }

        socket.SetTlsServer(certPath, keyPath);
    }

    public static void ConfigureTlsClientIfNeeded(ISocket socket,
        string transport, bool verbose = true)
    {
        if (!IsSecureTransport(transport))
            return;

        if (!TryResolvePerfTlsPaths(out _, out _, out string caPath))
        {
            throw new InvalidOperationException(verbose
                ? "TLS CA file not found under bindings/dotnet/tests/certs"
                : "TLS CA file not found.");
        }

        socket.SetTlsClient(caPath, "localhost");
    }

    public static void ConfigureReceiverTlsServerIfNeeded(ISocket receiver,
        string transport, bool verbose = true)
    {
        ConfigureTlsServerIfNeeded(receiver, transport, verbose);
    }

    public static bool TryResolvePerfTlsPaths(out string certPath,
        out string keyPath, out string caPath)
    {
        certPath = string.Empty;
        keyPath = string.Empty;
        caPath = string.Empty;

        string[] roots =
        {
            AppContext.BaseDirectory,
            Directory.GetCurrentDirectory(),
        };

        foreach (string start in roots)
        {
            var dir = new DirectoryInfo(start);
            while (dir != null)
            {
                string certDir = Path.Combine(dir.FullName, "bindings", "dotnet",
                    "tests", "certs");
                string cert = Path.Combine(certDir, "server.crt");
                string key = Path.Combine(certDir, "server.key");
                string ca = Path.Combine(certDir, "ca.crt");
                if (File.Exists(cert) && File.Exists(key) && File.Exists(ca))
                {
                    certPath = cert;
                    keyPath = key;
                    caPath = ca;
                    return true;
                }

                dir = dir.Parent;
            }
        }

        return false;
    }
}
