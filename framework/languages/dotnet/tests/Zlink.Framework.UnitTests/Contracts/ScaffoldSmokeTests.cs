using System.Reflection;
using Zlink.Framework.AspNetCore;

namespace Zlink.Framework.UnitTests;

public sealed class ScaffoldSmokeTests
{
    private static readonly string[] PublicTypeDeclarationTokens =
    [
        "public interface ",
        "public class ",
        "public sealed class ",
        "public abstract class ",
        "public record ",
        "public sealed record ",
        "public enum ",
        "public struct ",
        "public readonly struct ",
        "public delegate ",
        "public static class "
    ];

    private static readonly string[] InternalTypeDeclarationTokens =
    [
        "internal interface ",
        "internal class ",
        "internal sealed class ",
        "internal abstract class ",
        "internal record ",
        "internal sealed record ",
        "internal enum ",
        "internal struct ",
        "internal readonly struct ",
        "internal delegate ",
        "internal static class "
    ];

    private static readonly HashSet<Type> AllowedBackendTypes =
    [
        typeof(Message),
        typeof(RoutingId),
        typeof(SendFlags),
        typeof(ActorRef)
    ];

    [Fact]
    public void FrameworkRoot_IsDiscoverable_FromTestRuntime()
    {
        var frameworkRoot = Common.FrameworkTestEnvironment.GetFrameworkRoot();
        var solutionPath = Path.Combine(frameworkRoot, "Zlink.Framework.sln");

        Assert.True(File.Exists(solutionPath), $"Missing framework solution at '{solutionPath}'.");
    }

    [Fact]
    public void PublicSurface_DoesNotExpose_BackendConcreteTypes()
    {
        AssertBackendLeakageFree(typeof(IZLinkActor).Assembly);
        AssertBackendLeakageFree(typeof(ServiceCollectionExtensions).Assembly);
    }

    [Fact]
    public void PublicSurface_Removes_DirectRouteContracts_And_Exposes_ActorContracts()
    {
        AssertMissingMethod(typeof(IZLinkSpotOutbound), "SendTo");
        AssertMissingMethod(typeof(IZLinkSpotOutbound), "RequestTo");
        Assert.Null(typeof(IZLinkFrameworkOptions).GetMethod("AddActorFactory"));
        Assert.Null(typeof(IZLinkMeshNodeBuilder).GetMethod("AddActorFactory"));
        Assert.NotNull(typeof(IZLinkMeshObjectServerBuilder).GetMethod("AddActorFactory"));
        Assert.Contains(typeof(IZLinkActor), typeof(IZLinkActor).Assembly.GetExportedTypes());
        Assert.Contains(typeof(IZLinkActorManager), typeof(IZLinkActor).Assembly.GetExportedTypes());
        Assert.Contains(typeof(IZLinkSession), typeof(IZLinkActor).Assembly.GetExportedTypes());
        Assert.Contains(typeof(IZLinkSessionContext), typeof(IZLinkActor).Assembly.GetExportedTypes());
    }

    [Fact]
    public void PublicSurface_Removes_ActorReply_And_StreamClientContracts()
    {
        AssertMissingMethod(typeof(IZLinkActorContext), "Reply");
        Assert.DoesNotContain(
            typeof(IZLinkActor).Assembly.GetExportedTypes(),
            static type => type.FullName == "Zlink.Framework.Contracts.Actors.IZLinkActorReplyCall"
                           || type.FullName == "Zlink.Framework.Contracts.Actors.IZLinkActorStreamClient");
    }

    [Fact]
    public void FrameworkPublicTypeSourceFiles_LiveUnderContracts()
    {
        var frameworkRoot = Common.FrameworkTestEnvironment.GetFrameworkRoot();
        var sourceRoot = Path.Combine(frameworkRoot, "src", "Zlink.Framework");
        var violations = Directory
            .EnumerateFiles(sourceRoot, "*.cs", SearchOption.AllDirectories)
            .Where(static path => !path.Contains($"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}",
                                      StringComparison.Ordinal)
                                  && !path.Contains($"{Path.DirectorySeparatorChar}bin{Path.DirectorySeparatorChar}",
                                      StringComparison.Ordinal))
            .Where(path => HasPublicTypeDeclaration(File.ReadAllText(path)))
            .Where(path => !Path.GetRelativePath(sourceRoot, path)
                .StartsWith($"Contracts{Path.DirectorySeparatorChar}", StringComparison.Ordinal))
            .Select(path => Path.GetRelativePath(sourceRoot, path))
            .Order(StringComparer.Ordinal)
            .ToArray();

        Assert.Empty(violations);
    }

    [Fact]
    public void FrameworkContracts_DoNotContain_InternalTypeDeclarations()
    {
        var frameworkRoot = Common.FrameworkTestEnvironment.GetFrameworkRoot();
        var contractsRoot = Path.Combine(frameworkRoot, "src", "Zlink.Framework", "Contracts");
        var violations = Directory
            .EnumerateFiles(contractsRoot, "*.cs", SearchOption.AllDirectories)
            .Where(path => HasInternalTypeDeclaration(File.ReadAllText(path)))
            .Select(path => Path.GetRelativePath(contractsRoot, path))
            .Order(StringComparer.Ordinal)
            .ToArray();

        Assert.Empty(violations);
    }

    [Fact]
    public void FrameworkExportedTypes_UseContractsNamespace()
    {
        var violations = typeof(IZLinkActor).Assembly
            .GetExportedTypes()
            .Where(static type => type != typeof(ActorRef))
            .Where(static type => type.Namespace is null
                                  || (!type.Namespace.Equals("Zlink.Framework.Contracts", StringComparison.Ordinal)
                                      && !type.Namespace.StartsWith("Zlink.Framework.Contracts.",
                                          StringComparison.Ordinal)))
            .Select(static type => type.FullName)
            .Order(StringComparer.Ordinal)
            .ToArray();

        Assert.Empty(violations);
        Assert.Same(typeof(IZLinkActor).Assembly, typeof(ActorRef).Assembly);
        Assert.Equal("Systems.Zlink.ActorRef", typeof(ActorRef).FullName);
    }

    private static void AssertBackendLeakageFree(Assembly assembly)
    {
        foreach (var type in assembly.GetExportedTypes())
        {
            AssertPublicBoundary(type);

            foreach (var member in type.GetMembers(BindingFlags.Instance | BindingFlags.Static | BindingFlags.Public))
                switch (member)
                {
                    case MethodInfo method:
                        AssertTypeAllowed(method.ReturnType);

                        foreach (var parameter in method.GetParameters()) AssertTypeAllowed(parameter.ParameterType);

                        break;
                    case PropertyInfo property:
                        AssertTypeAllowed(property.PropertyType);
                        break;
                    case FieldInfo field:
                        AssertTypeAllowed(field.FieldType);
                        break;
                    case EventInfo @event:
                        AssertTypeAllowed(@event.EventHandlerType);
                        break;
                }
        }
    }

    private static void AssertPublicBoundary(Type type)
    {
        AssertTypeAllowed(type);

        if (type.BaseType is not null) AssertTypeAllowed(type.BaseType);

        foreach (var implementedInterface in type.GetInterfaces()) AssertTypeAllowed(implementedInterface);
    }

    private static void AssertTypeAllowed(Type? type)
    {
        if (type is null) return;

        if (type.IsGenericType)
            foreach (var genericArgument in type.GetGenericArguments())
                AssertTypeAllowed(genericArgument);

        if (type.HasElementType)
        {
            AssertTypeAllowed(type.GetElementType());
            return;
        }

        if (type.Assembly != typeof(Systems.Zlink.Zlink).Assembly) return;

        Assert.Contains(type, AllowedBackendTypes);
    }

    private static void AssertMissingMethod(Type type, string methodName)
    {
        Assert.Null(type.GetMethod(methodName));
    }

    private static bool HasPublicTypeDeclaration(string source)
    {
        return source
            .Split('\n')
            .Select(static line => line.TrimEnd('\r'))
            .Any(static line =>
                PublicTypeDeclarationTokens.Any(token => line.StartsWith(token, StringComparison.Ordinal)));
    }

    private static bool HasInternalTypeDeclaration(string source)
    {
        return source
            .Split('\n')
            .Select(static line => line.TrimEnd('\r'))
            .Any(static line =>
                InternalTypeDeclarationTokens.Any(token => line.StartsWith(token, StringComparison.Ordinal)));
    }
}
