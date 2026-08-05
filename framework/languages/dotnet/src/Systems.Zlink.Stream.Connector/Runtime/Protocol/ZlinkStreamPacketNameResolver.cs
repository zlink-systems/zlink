using System.Collections.Concurrent;
using System.Reflection;

namespace Systems.Zlink.Stream.Connector.Runtime.Protocol;

internal sealed class ZlinkStreamPacketNameResolver : IZlinkStreamPacketNameResolver
{
    private static readonly ConcurrentDictionary<Type, string> Cache = new();

    public string Resolve(Type payloadType)
    {
        ArgumentNullException.ThrowIfNull(payloadType);
        return Cache.GetOrAdd(payloadType, static type =>
            type.GetCustomAttribute<ZlinkStreamPacketNameAttribute>(false)?.Name
            ?? type.Name);
    }
}