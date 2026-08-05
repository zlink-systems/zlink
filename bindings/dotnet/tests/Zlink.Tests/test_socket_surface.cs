using System;
using System.Linq;
using System.Reflection;
using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_socket_surface
{
    private static MethodInfo[] PublicInstanceMethods(Type type)
    {
        return type.IsInterface
            ? type.GetInterfaces()
                .Append(type)
                .SelectMany(current => current.GetMethods())
                .ToArray()
            : type.GetMethods(BindingFlags.Instance | BindingFlags.Public);
    }

    private static bool HasPublicInstanceMethod(
        Type type,
        string name,
        params Type[] parameterTypes)
    {
        return PublicInstanceMethods(type).Any(method =>
            method.Name == name
            && method.GetParameters().Select(parameter => parameter.ParameterType)
                .SequenceEqual(parameterTypes));
    }

    [Fact]
    public void runtime_implementation_types_are_not_public_contract()
    {
        string[] hiddenRuntimeTypeNames =
        {
            "Systems.Zlink.Context",
            "Systems.Zlink.SocketBase"
        };

        string[] exportedTypeNames = typeof(Zlink).Assembly.GetExportedTypes()
            .Select(type => type.FullName!)
            .ToArray();

        foreach (string typeName in hiddenRuntimeTypeNames)
            Assert.DoesNotContain(typeName, exportedTypeNames);

        Assert.DoesNotContain(exportedTypeNames,
            typeName => typeName.StartsWith(
                "Systems.Zlink.Runtime.",
                StringComparison.Ordinal));
    }

    [Fact]
    public void typed_socket_contract_keeps_direction_specific_methods()
    {
        Assert.DoesNotContain(PublicInstanceMethods(typeof(IPubSocket)),
            method => method.Name == "Recv");
        Assert.DoesNotContain(PublicInstanceMethods(typeof(ISubSocket)),
            method => method.Name == "Publish");
        Assert.True(HasPublicInstanceMethod(
            typeof(IPairSocket),
            "Recv",
            typeof(Received),
            typeof(RecvFlags)));
        Assert.True(HasPublicInstanceMethod(
            typeof(IStreamSocket),
            nameof(IStreamSocket.OnPacket),
            typeof(StreamPacketHandler)));
        Assert.True(HasPublicInstanceMethod(
            typeof(IStreamSocket),
            nameof(IStreamSocket.RecvPart),
            typeof(RoutingId?).MakeByRefType(),
            typeof(Message).MakeByRefType(),
            typeof(bool).MakeByRefType(),
            typeof(RecvFlags)));
    }
}
