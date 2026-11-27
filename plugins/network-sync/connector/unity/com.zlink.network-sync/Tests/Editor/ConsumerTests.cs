// SPDX-License-Identifier: LicenseRef-Zlink
using System;
using NUnit.Framework;
using Systems.Zlink.Stream.Connector.Contracts;

namespace Zlink.NetworkSync.Tests
{
    public sealed class ConsumerTests
    {
        [Test]
        public void AttachesToManagedConnector()
        {
            var connector = ZlinkStreamConnectorFactory.Create(
                new ZlinkStreamConnectorOptions { Endpoint = new Uri("tcp://localhost:1") });
            try
            {
                using var client = new NetworkSyncClient<int, int, int>();
                client.Attach(connector, "unity-edit-mode");
                Assert.That(client.TryGetInterpolation(out _), Is.False);
            }
            finally
            {
                connector.DisposeAsync().AsTask().GetAwaiter().GetResult();
            }
        }
    }
}
