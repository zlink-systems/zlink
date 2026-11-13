using System.Runtime.CompilerServices;

[assembly: InternalsVisibleTo("Zlink.Framework.UnitTests")]
[assembly: InternalsVisibleTo("Zlink.Framework.ContractTests")]
[assembly: InternalsVisibleTo("Zlink.Framework.Codecs.Protobuf")]
[assembly: InternalsVisibleTo("Zlink.Framework.Locations.Redis")]
[assembly: InternalsVisibleTo("Zlink.Framework.Locations.Redis.Tests")]
[assembly: InternalsVisibleTo("Zlink.Framework.AspNetCore")]
[assembly: InternalsVisibleTo("SpotActorTransfer.ActorNode")]
#if ZLINK_DOTNET_LADDER_L1 || ZLINK_DOTNET_LADDER_L2
[assembly: InternalsVisibleTo("WithGrpcBench.ZLinkServer")]
[assembly: InternalsVisibleTo("WithGrpcBench.Client")]
#endif
