// SPDX-License-Identifier: LicenseRef-Zlink
using System.Reflection;
using System.Text;
using Systems.Zlink.Stream.Connector;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Contracts.Calls;

namespace Zlink.NetworkSync.Tests;

public class TestConnector : DispatchProxy
{
    private readonly Dictionary<string, Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, CancellationToken, ValueTask>>
        _handlers = new();

    public List<(string Name, ZlinkStreamEncodedPayload Payload)> Sent { get; } = new();
    public int Subscriptions => _handlers.Count;
    private ZlinkStreamConnectorOptions Options { get; } = new() { Endpoint = new Uri("tcp://localhost:1") };

    public static (IZlinkStreamConnector Connector, TestConnector Transport) Create()
    {
        var connector = Create<IZlinkStreamConnector, TestConnector>();
        return (connector, (TestConnector)(object)connector);
    }

    public ValueTask Receive(string name, string json)
    {
        if (!_handlers.TryGetValue(name, out var handler))
            return default;
        return handler(new ZlinkStreamMessage<ZlinkStreamEncodedPayload>(
            name, default!, new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Json, Encoding.UTF8.GetBytes(json))), default);
    }

    protected override object? Invoke(MethodInfo? method, object?[]? args)
    {
        switch (method!.Name)
        {
            case "get_Options":
                return Options;
            case "Send":
                return new SendCall(this, (ZlinkStreamEncodedPayload)args![0]!);
            case "On":
                var name = (string)args![0]!;
                var handler = (Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, CancellationToken, ValueTask>)args[1]!;
                _handlers.Add(name, handler);
                return new Registration(() => _handlers.Remove(name));
            default:
                throw new NotSupportedException(method.Name);
        }
    }

    private sealed class Registration(Action unregister) : IDisposable
    {
        public void Dispose() => unregister();
    }

    private sealed class SendCall(TestConnector transport, ZlinkStreamEncodedPayload payload) : IZlinkStreamSendCall
    {
        private string? _name;
        public IZlinkStreamSendCall PacketName(string name)
        {
            _name = name;
            return this;
        }

        public ValueTask Async(CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            transport.Sent.Add((_name ?? transport.Options.NameResolver.Resolve(payload.MessageType!), payload));
            return default;
        }

        public IZlinkStreamSendCall Metadata(string key, string value) => throw new NotSupportedException();
        public IZlinkStreamSendCall Metadata(ZlinkStreamMetadata metadata) => throw new NotSupportedException();
        public IZlinkStreamSendCall Compress() => throw new NotSupportedException();
    }
}
