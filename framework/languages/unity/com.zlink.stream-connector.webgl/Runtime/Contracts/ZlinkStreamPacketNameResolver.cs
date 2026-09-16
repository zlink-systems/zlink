using System;
using System.Collections.Generic;
using System.Reflection;

namespace Systems.Zlink.Stream.Connector.Contracts
{
    /// <summary>
    ///     Default packet name resolver: the <see cref="ZlinkStreamPacketNameAttribute" />
    ///     value when present, otherwise the payload type name.
    /// </summary>
    public sealed class ZlinkStreamPacketNameResolver : IZlinkStreamPacketNameResolver
    {
        private readonly Dictionary<Type, string> _cache = new Dictionary<Type, string>();

        public string Resolve(Type payloadType)
        {
            if (payloadType is null) throw new ArgumentNullException(nameof(payloadType));
            lock (_cache)
            {
                if (_cache.TryGetValue(payloadType, out var cached)) return cached;
                var attribute = payloadType.GetCustomAttribute<ZlinkStreamPacketNameAttribute>(false);
                var name = attribute is null ? payloadType.Name : attribute.Name;
                _cache[payloadType] = name;
                return name;
            }
        }
    }
}
