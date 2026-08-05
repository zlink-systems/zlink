/* SPDX-License-Identifier: Apache-2.0 */

using System.Security.Cryptography.X509Certificates;

namespace Zlink.HttpClient.Runtime;

internal sealed class HttpTransport(
    SocketsHttpHandler handler,
    IReadOnlyList<X509Certificate2> ownedCertificates) : IDisposable
{
    public SocketsHttpHandler Handler { get; } = handler;

    public void Dispose()
    {
        Handler.Dispose();
        for (var index = ownedCertificates.Count - 1; index >= 0; index--)
            ownedCertificates[index].Dispose();
    }
}
