using System.Reflection;
using System.Text;
using System.Text.RegularExpressions;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Workers;
using Zlink.Framework.ContractTests.Support;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Codecs.MessagePack;
using Zlink.Framework.Codecs.Protobuf;
using Zlink.Framework.Locations.Redis;
using Zlink.HttpClient;

namespace Zlink.Framework.ContractTests.Coverage;

public sealed class ContractSurfaceCoverage
{
    [Fact]
    public void Frozen_public_surface_excludes_replaced_contracts()
    {
        var assembly = typeof(IZLinkFrameworkOptions).Assembly;
        Assert.Null(assembly.GetType("Zlink.Framework.Contracts.Channels.IZLinkYieldRequestCall"));
        Assert.Null(assembly.GetType("Zlink.Framework.Contracts.Actors.IZLinkActorYieldJoinCall"));
        Assert.Null(assembly.GetType("Zlink.Framework.Contracts.Dispatch.ZLinkDispatchMode"));
        Assert.Null(assembly.GetType("Zlink.Framework.Contracts.Locations.SpotRef"));
        Assert.Null(assembly.GetType("Zlink.Framework.Contracts.Locations.IZLinkSpotRefResolver"));
        Assert.Null(assembly.GetType("Zlink.Framework.Contracts.Locations.IZLinkActorAddressResolver"));
        Assert.Null(assembly.GetType("Zlink.Framework.Contracts.Assembly.ZLinkFrameworkAssemblyMarker"));
        Assert.Null(assembly.GetType("Zlink.Framework.Contracts.Codecs.Json.ZLinkJsonCodecNamespace"));
        Assert.Null(assembly.GetType("Zlink.Framework.Contracts.Handlers.ZLinkStreamRawAttribute"));
        Assert.Null(assembly.GetType("Zlink.Framework.Contracts.Configuration.IZLinkDrainControl"));
        Assert.Null(assembly.GetType("Zlink.Framework.Contracts.Configuration.ZLinkDrainResult"));
        Assert.Null(assembly.GetType("Zlink.Framework.Contracts.Configuration.ZLinkMeshDrainResult"));
        Assert.Null(assembly.GetType("Zlink.Framework.Contracts.Configuration.ZLinkMeshDrainSnapshot"));

        Assert.DoesNotContain(typeof(IZLinkSendCall).GetMethods(), method => method.Name == "PacketName");
        Assert.DoesNotContain(typeof(IZLinkRequestCall).GetMethods(), method => method.Name == "PacketName");
        Assert.Contains(typeof(IZLinkRequestCall).GetMethods(), method => method.Name == "Yield");
        Assert.DoesNotContain(typeof(IZLinkActorSendCall).GetMethods(), method => method.Name == "PacketName");
        Assert.Contains(typeof(IZLinkActorSendCall).GetMethods(), method => method.Name == "Async");
        Assert.DoesNotContain(typeof(IZLinkActorRequestCall).GetMethods(), method => method.Name == "PacketName");
        Assert.Contains(typeof(IZLinkActorCreateCall).GetMethods(), method => method.Name == "Yield");
        Assert.Contains(typeof(IZLinkActorGetOrCreateCall).GetMethods(), method => method.Name == "Yield");
        Assert.Equal(
            new[] { "Defer" },
            typeof(IZLinkActorDeferredJoinCall).GetMethods().Select(method => method.Name).ToArray());
        Assert.DoesNotContain(typeof(IZLinkActorContext).GetMembers(), member => member.Name is "IsJoined" or "GetSpot");
        Assert.Contains(typeof(IZLinkWorkerCall<>).GetMethods(), method => method.Name == "Yield");
        Assert.Contains(typeof(IZLinkWorkerCall<>).GetMethods(), method => method.Name == "Submit");
        Assert.Contains(typeof(IZLinkSpotCreateCall).GetMethods(), method => method.Name == "Yield");
        Assert.Contains(typeof(IZLinkSpotGetOrCreateCall).GetMethods(), method => method.Name == "Yield");
        Assert.DoesNotContain(typeof(IZLinkDispatchOptions).GetProperties(), property => property.Name.EndsWith("DispatchMode", StringComparison.Ordinal));

        Assert.True(typeof(ZLinkActorJoinCompletion).IsAbstract);
        Assert.True(typeof(ZLinkActorJoinCompletion.Accepted).IsSealed);
        Assert.True(typeof(ZLinkActorJoinCompletion.Rejected).IsSealed);
        Assert.True(typeof(ZLinkActorJoinCompletion.Failed).IsSealed);
        Assert.Null(typeof(IZLinkSpotClient).Assembly.GetType(
            "Zlink.Framework.Contracts.Locations.SpotHandle"));
        Assert.Equal(
            new[] { "Connect", "Disconnect", "ListConnections" },
            typeof(IZLinkEndpointConnections).GetMethods().Select(method => method.Name).Order().ToArray());
    }

    [Fact]
    public void Closed_result_and_event_roots_cannot_be_subclassed_outside_the_framework_assembly()
    {
        Type[] roots = [typeof(ZLinkActorJoinCompletion)];

        foreach (var root in roots)
        {
            var constructors = root.GetConstructors(BindingFlags.Instance | BindingFlags.NonPublic)
                .Where(constructor => constructor.GetParameters() is not [{ ParameterType: var parameterType }]
                                      || parameterType != root)
                .ToArray();
            Assert.NotEmpty(constructors);
            Assert.All(constructors, constructor => Assert.True(constructor.IsFamilyAndAssembly));
        }
    }

    [Fact]
    public void Redis_Extension_Remains_A_Separate_Package_Without_A_Backend_Specific_Registration_API()
    {
        var framework = typeof(IZLinkFrameworkOptions).Assembly;
        var redis = typeof(ZLinkRedisLocationStore).Assembly;

        Assert.NotSame(framework, redis);
        Assert.DoesNotContain(
            framework.GetReferencedAssemblies(),
            reference => reference.Name == "StackExchange.Redis");
        Assert.DoesNotContain(
            framework.GetExportedTypes(),
            type => type.Namespace?.Contains("Redis", StringComparison.Ordinal) == true);
        Assert.DoesNotContain(
            framework.GetExportedTypes().SelectMany(static type => type.GetMethods()),
            method => method.Name.Contains("Redis", StringComparison.Ordinal));
    }

    [Fact]
    public void Every_public_contract_interface_has_a_scenario_example()
    {
        var exportedContractTypes = new[]
            {
                typeof(IZLinkFrameworkOptions).Assembly,
                typeof(Zlink.Framework.Contracts.Codecs.IZLinkCodecExtension).Assembly,
                typeof(Zlink.Framework.LocationProvider.IZLinkLocationStore).Assembly
            }
            .Distinct()
            .SelectMany(static assembly => assembly.GetExportedTypes())
            .Where(static type => type.Namespace is not null
                                  && (type.Namespace.StartsWith(
                                          "Zlink.Framework.Contracts",
                                          StringComparison.Ordinal)
                                      || type.Namespace == "Zlink.Framework.LocationProvider"))
            .ToHashSet();

        // Every exported interface must have a worked example. Closed-union abstract records
        // (ZLinkActorCreateResult, ZLinkActorJoinCompletion, ...) are equally part of the public
        // contract surface and may be cited by an example, but citing them is not mandatory --
        // so they widen the "is this a real contract type" check without widening the mandate.
        var exportedContracts = exportedContractTypes
            .Where(static type => type.IsInterface)
            .OrderBy(static type => type.FullName, StringComparer.Ordinal)
            .ToArray();

        var coveredContracts = typeof(ContractExampleAttribute).Assembly
            .GetTypes()
            .SelectMany(type => type.GetMethods(
                BindingFlags.Instance |
                BindingFlags.Public |
                BindingFlags.NonPublic |
                BindingFlags.Static))
            .Where(method => method.GetCustomAttribute<FactAttribute>() is not null)
            .SelectMany(method => method.GetCustomAttributes<ContractExampleAttribute>())
            .SelectMany(attribute => attribute.Contracts)
            .ToHashSet();

        var unknown = coveredContracts
            .Where(type => !exportedContractTypes.Contains(type))
            .OrderBy(type => type.FullName, StringComparer.Ordinal)
            .Select(type => type.FullName)
            .ToArray();

        var missing = exportedContracts
            .Where(type => !coveredContracts.Contains(type))
            .Select(type => type.FullName)
            .ToArray();

        Assert.Empty(unknown);
        Assert.Empty(missing);
    }

    [Fact]
    public void Basic_business_message_contracts_do_not_expose_binding_messages()
    {
        var bindingMessage = typeof(Message);
        var frameworkMessage = typeof(ZLinkMessage);

        AssertMethodParameter(
            typeof(IZLinkSession),
            nameof(IZLinkSession.OnDispatchAsync),
            "payload",
            frameworkMessage,
            bindingMessage);
        AssertMethodParameterIsNot(
            typeof(IZLinkSessionPacketHandler<,>),
            nameof(IZLinkSessionPacketHandler<IZLinkSessionContext, ZLinkMessage>.HandleAsync),
            "message",
            bindingMessage);
        AssertMethodParameter(
            typeof(IZLinkActorContext),
            nameof(IZLinkActorContext.JoinSpot),
            "request",
            frameworkMessage,
            bindingMessage);
        AssertMethodParameter(
            typeof(IZLinkActorContext),
            nameof(IZLinkActorContext.JoinEntrySpot),
            "request",
            frameworkMessage,
            bindingMessage);
        AssertMethodParameter(
            typeof(IZLinkSpot),
            nameof(IZLinkSpot.OnCreateAsync),
            "request",
            frameworkMessage,
            bindingMessage);
        AssertMethodParameter(
            typeof(IZLinkSpot<>),
            nameof(IZLinkSpot<IZLinkActor>.OnActorJoinAsync),
            "request",
            frameworkMessage,
            bindingMessage);

        Assert.DoesNotContain(
            typeof(ZLinkSpotCreateResponse).GetMethods(BindingFlags.Public | BindingFlags.Static),
            method => method.GetParameters().Any(parameter => parameter.ParameterType == bindingMessage));
    }

    [Fact]
    public void Fixed_spec_snapshot_matches_every_exported_contract_signature()
    {
        var repositoryRoot = FindRepositoryRoot();
        // 기계 판독용 계약 snapshot은 문서 트리가 아니라 .NET 코드 옆(framework/languages/dotnet/contract)에 둔다.
        var contractRoot = Path.Combine(
            repositoryRoot,
            "framework",
            "languages",
            "dotnet",
            "contract");
        var assemblies = new[]
            {
                typeof(IZLinkFrameworkOptions).Assembly,
                typeof(Zlink.Framework.Contracts.Codecs.IZLinkCodecExtension).Assembly,
                typeof(ServiceCollectionExtensions).Assembly,
                typeof(ZLinkMessagePackCodec).Assembly,
                typeof(ZLinkProtobufCodec).Assembly,
                typeof(ZLinkRedisLocationStore).Assembly,
                typeof(Zlink.Framework.LocationProvider.IZLinkLocationStore).Assembly,
                typeof(ZLinkHttpClient).Assembly,
                typeof(IZlinkStreamConnector).Assembly
            }
            .Distinct()
            .ToArray();
        var snapshotRoot = Path.Combine(contractRoot, "api");
        var expected = string.Concat(assemblies
            .Select(static assembly => assembly.GetName().Name!)
            .Order(StringComparer.Ordinal)
            .Select(name => File.ReadAllText(Path.Combine(snapshotRoot, $"{name}.api.txt"))));
        var actual = PublicContractSnapshot.Render(assemblies);

        Assert.Equal(NormalizeLines(expected), NormalizeLines(actual));
    }

    [Fact]
    public void DotNetExactInterfaceDeclarations_Match_Source_And_Package_Exports()
    {
        var repositoryRoot = FindRepositoryRoot();
        var interfaceRoot = Path.Combine(
            repositoryRoot,
            "framework",
            "doc",
            "framework",
            "common",
            "spec",
            "server",
            "languages",
            "dotnet",
            "interfaces");
        var sourceRoot = Path.Combine(repositoryRoot, "framework", "languages", "dotnet", "src");
        var sourceAssemblies = GetContractAssemblies();
        var exactInterfaceAssemblies = GetServerContractAssemblies();
        var packageApiRoot = Path.Combine(
            repositoryRoot,
            "framework",
            "languages",
            "dotnet",
            "contract",
            "api");
        var packageTypes = ExtractPackageTypes(packageApiRoot, sourceAssemblies);
        var declarations = ResolveDocumentOwners(
            ExtractExactInterfaceDeclarations(interfaceRoot),
            packageTypes);
        Assert.NotEmpty(declarations);

        var exactInterfaceAssemblyNames = exactInterfaceAssemblies
            .Select(static assembly => assembly.GetName().Name!)
            .ToHashSet(StringComparer.Ordinal);
        var sourceDeclarations = ExtractSourceDeclarations(sourceRoot)
            .Where(declaration => declaration.AssemblyName is not null
                                 && exactInterfaceAssemblyNames.Contains(declaration.AssemblyName))
            .ToArray();
        var sourceByKey = sourceDeclarations
            .GroupBy(static declaration => declaration.QualifiedOwner, StringComparer.Ordinal)
            .ToDictionary(
                static group => group.Key,
                static group => group.ToArray(),
                StringComparer.Ordinal);
        var documentedByKey = declarations
            .GroupBy(static declaration => declaration.QualifiedOwner, StringComparer.Ordinal)
            .ToDictionary(
                static group => group.Key,
                static group => group.ToArray(),
                StringComparer.Ordinal);

        var missingFromSource = declarations
            .Where(declaration => !ContainsDeclaration(sourceByKey, declaration))
            .Select(declaration =>
            {
                var candidates = sourceByKey.TryGetValue(declaration.QualifiedOwner, out var ownerCandidates)
                    ? string.Join(" | ", ownerCandidates
                        .Where(candidate => candidate.Kind == declaration.Kind)
                        .Select(static candidate => candidate.Signature)
                        .Order(StringComparer.Ordinal))
                    : "<no source owner>";
                return $"{declaration} [source: {candidates}]";
            })
            .Order(StringComparer.Ordinal)
            .ToArray();
        var extraFromSource = sourceDeclarations
            .Where(declaration => !ContainsDeclaration(documentedByKey, declaration))
            .Where(declaration => declaration.Kind != DeclarationKind.Type
                                 || !documentedByKey.ContainsKey(declaration.QualifiedOwner))
            .Where(declaration => !IsCoveredByDocumentedInterface(
                declaration,
                documentedByKey,
                sourceAssemblies))
            .Select(static declaration => declaration.ToString())
            .Order(StringComparer.Ordinal)
            .ToArray();

        Assert.True(
            missingFromSource.Length == 0
            && extraFromSource.Length == 0,
            "Exact interface declarations differ from source/package contract. "
            + $"Missing: {string.Join(", ", missingFromSource)}; "
            + $"Undocumented source declarations: {string.Join(", ", extraFromSource)}");

        AssertRecordProjectionExports(interfaceRoot, packageApiRoot, sourceAssemblies, packageTypes);

        var snapshotRoot = Path.Combine(repositoryRoot, "framework", "languages", "dotnet", "contract", "api");
        var expected = string.Concat(sourceAssemblies
            .Select(static assembly => assembly.GetName().Name!)
            .Order(StringComparer.Ordinal)
            .Select(name => File.ReadAllText(Path.Combine(snapshotRoot, $"{name}.api.txt"))));
        Assert.Equal(
            NormalizeLines(expected),
            NormalizeLines(PublicContractSnapshot.Render(sourceAssemblies)));
    }

    private static void AssertRecordProjectionExports(
        string interfaceRoot,
        string packageApiRoot,
        IReadOnlyCollection<Assembly> assemblies,
        IReadOnlyCollection<PackageType> packageTypes)
    {
        foreach (var projection in ExtractRecordProjectionContracts(interfaceRoot, packageTypes))
        {
            var type = assemblies
                .SelectMany(static assembly => assembly.GetExportedTypes())
                .SingleOrDefault(candidate => string.Equals(
                    candidate.Assembly.GetName().Name,
                    projection.AssemblyName,
                    StringComparison.Ordinal)
                    && string.Equals(
                        GetReflectionTypeFullName(candidate),
                        projection.FullName,
                        StringComparison.Ordinal));
            Assert.NotNull(type);

            var constructor = type!.GetConstructors(BindingFlags.Instance | BindingFlags.Public)
                .SingleOrDefault(candidate =>
                {
                    var parameters = candidate.GetParameters();
                    return parameters.Length == projection.Parameters.Length
                           && parameters.Select(static parameter => parameter.Name)
                               .SequenceEqual(
                                   projection.Parameters,
                                   StringComparer.Ordinal);
                });
            Assert.NotNull(constructor);

            var constructorParameters = constructor!.GetParameters();
            foreach (var (parameterName, constructorParameter) in projection.Parameters.Zip(constructorParameters))
            {
                var property = type.GetProperty(
                    parameterName,
                    BindingFlags.Instance | BindingFlags.Public);
                Assert.NotNull(property);
                Assert.Equal(constructorParameter.ParameterType, property!.PropertyType);
                Assert.NotNull(property.GetMethod);
                Assert.True(property.GetMethod!.IsPublic);
                Assert.NotNull(property.SetMethod);
                Assert.True(property.SetMethod!.IsPublic);
                Assert.Contains(
                    property.SetMethod.ReturnParameter.GetRequiredCustomModifiers(),
                    modifier => modifier == typeof(System.Runtime.CompilerServices.IsExternalInit));
            }

            var hasMatchingDeconstruct = type.GetMethods(BindingFlags.Instance | BindingFlags.Public)
                .Where(method => method.Name == "Deconstruct" && method.ReturnType == typeof(void))
                .Any(method =>
                {
                    var parameters = method.GetParameters();
                    return parameters.Length == constructorParameters.Length
                           && parameters.Zip(constructorParameters).Select((pair, index) =>
                               (pair, index)).All(item =>
                               item.pair.First.Name == projection.Parameters[item.index]
                               && item.pair.First.IsOut
                               && item.pair.First.ParameterType.IsByRef
                               && item.pair.First.ParameterType.GetElementType() == item.pair.Second.ParameterType);
                });
            Assert.True(
                hasMatchingDeconstruct,
                $"Record projection {projection.AssemblyName}::{projection.FullName} must export a matching Deconstruct method.");

            var packageBlock = ReadPackageTypeBlock(
                packageApiRoot,
                projection.AssemblyName,
                projection.FullName);
            foreach (var parameterName in projection.Parameters)
            {
                Assert.True(
                    Regex.IsMatch(
                        packageBlock,
                        $"^    property .* {Regex.Escape(parameterName)} \\{{ get; init; \\}}",
                        RegexOptions.Multiline | RegexOptions.CultureInvariant),
                    $"Package export {projection.AssemblyName}::{projection.FullName} must expose property '{parameterName}'.");
            }

            Assert.Contains("    method System.Void Deconstruct(", packageBlock, StringComparison.Ordinal);
            foreach (var parameterName in projection.Parameters)
            {
                Assert.Contains($" {parameterName} [", packageBlock, StringComparison.Ordinal);
            }
        }
    }

    private static string ReadPackageTypeBlock(
        string packageApiRoot,
        string assemblyName,
        string fullName)
    {
        var path = Path.Combine(packageApiRoot, $"{assemblyName}.api.txt");
        Assert.True(File.Exists(path), $"Missing package API snapshot for {assemblyName}.");
        var lines = File.ReadAllLines(path);
        var displayName = Regex.Replace(
            fullName,
            @"`\d+",
            string.Empty,
            RegexOptions.CultureInvariant);
        var start = Array.FindIndex(
            lines,
            line => line.StartsWith("  type ", StringComparison.Ordinal)
                    && line.Contains($" {displayName}", StringComparison.Ordinal));
        Assert.True(start >= 0, $"Missing package API type {assemblyName}::{fullName}.");

        var end = start + 1;
        while (end < lines.Length
               && !lines[end].StartsWith("  type ", StringComparison.Ordinal)
               && !lines[end].StartsWith("assembly ", StringComparison.Ordinal))
            end++;
        return string.Join('\n', lines[start..end]);
    }

    private static IReadOnlyList<RecordProjectionContract> ExtractRecordProjectionContracts(
        string interfaceRoot,
        IReadOnlyCollection<PackageType> packageTypes)
    {
        var projections = new List<RecordProjectionContract>();
        var parseOptions = new CSharpParseOptions(LanguageVersion.Preview);
        var documents = Directory.EnumerateFiles(interfaceRoot, "*.ko.md", SearchOption.TopDirectoryOnly)
            .Where(path => !string.Equals(
                Path.GetFileName(path),
                "README.ko.md",
                StringComparison.Ordinal))
            .SelectMany(path => ExtractCSharpBlocks(path).Select(body => (
                Document: Path.GetFileName(path),
                Body: body,
                AssemblyName: (string?)null)))
            .Select((document, index) => new SyntaxDocument(
                document.Document,
                document.Body,
                document.AssemblyName,
                ExactInterface: true,
                Ordinal: index))
            .ToArray();
        var binding = SemanticBindingContext.Create(documents, parseOptions);
        foreach (var document in documents)
        {
            var tree = binding.GetTree(document.Ordinal);
            var resolver = binding.GetResolver(tree);
            foreach (var record in tree.GetRoot().DescendantNodes().OfType<RecordDeclarationSyntax>())
            {
                if (!IsPublicType(record) || record.ParameterList is null)
                    continue;

                var owner = GetTypeKey(record);
                var expected = ResolveExpectedTypeIdentity(
                    new ContractDeclaration(
                        document.Document,
                        DeclarationKind.Type,
                        owner,
                        TypeSignature(record, resolver)));
                var parameters = record.ParameterList.Parameters
                    .Select(parameter => parameter.Type is null
                        ? throw new InvalidOperationException(
                            $"A positional record parameter must have a type: {document.Document}:{owner}:{parameter}")
                        : parameter.Identifier.Text)
                    .ToArray();
                projections.Add(new RecordProjectionContract(
                    document.Document,
                    expected.AssemblyName,
                    expected.FullName,
                    parameters));
            }
        }

        return projections
            .GroupBy(
                static projection => $"{projection.AssemblyName}::{projection.FullName}",
                StringComparer.Ordinal)
            .Select(static group => group.First())
            .OrderBy(static projection => projection.AssemblyName, StringComparer.Ordinal)
            .ThenBy(static projection => projection.FullName, StringComparer.Ordinal)
            .ToArray();
    }

    [Fact]
    public void DotNetExactInterfaceTypes_Have_A_Single_DocumentOwner()
    {
        var interfaceRoot = Path.Combine(
            FindRepositoryRoot(),
            "framework",
            "doc",
            "framework",
            "common",
            "spec",
            "server",
            "languages",
            "dotnet",
            "interfaces");
        var repositoryRoot = FindRepositoryRoot();
        var packageTypes = ExtractPackageTypes(
            Path.Combine(repositoryRoot, "framework", "languages", "dotnet", "contract", "api"),
            GetContractAssemblies());
        var serverAssemblyNames = GetServerContractAssemblies()
            .Select(static assembly => assembly.GetName().Name!)
            .ToHashSet(StringComparer.Ordinal);
        var declarations = ResolveDocumentOwners(
            ExtractExactInterfaceDeclarations(interfaceRoot),
            packageTypes);
        var duplicateOwners = declarations
            .Where(static declaration => declaration.Kind == DeclarationKind.Type)
            .GroupBy(static declaration => declaration.QualifiedOwner, StringComparer.Ordinal)
            .Where(static group => group.Select(static declaration => declaration.Document).Distinct(StringComparer.Ordinal).Count() != 1)
            .Select(static group => $"{group.Key}: {string.Join(", ", group.Select(static declaration => declaration.Document).Distinct(StringComparer.Ordinal).Order(StringComparer.Ordinal))}")
            .Order(StringComparer.Ordinal)
            .ToArray();
        var documentedTypeOwners = declarations
            .Where(static declaration => declaration.Kind == DeclarationKind.Type)
            .Select(static declaration => declaration.QualifiedOwner)
            .ToHashSet(StringComparer.Ordinal);
        var missingOwners = packageTypes
            .Where(type => serverAssemblyNames.Contains(type.AssemblyName))
            .Where(type => !documentedTypeOwners.Contains(type.Identity))
            .Select(static type => type.Identity)
            .Order(StringComparer.Ordinal)
            .ToArray();

        Assert.True(
            duplicateOwners.Length == 0 && missingOwners.Length == 0,
            "Exact interface exported types must have one document owner. "
            + $"Duplicate owners: {string.Join("; ", duplicateOwners)}; "
            + $"Missing owners: {string.Join(", ", missingOwners)}");
    }

    private static string NormalizeLines(string value) =>
        value.Replace("\r\n", "\n", StringComparison.Ordinal);

    private static Assembly[] GetContractAssemblies() =>
        new[]
            {
                typeof(IZLinkFrameworkOptions).Assembly,
                typeof(Zlink.Framework.Contracts.Codecs.IZLinkCodecExtension).Assembly,
                typeof(ServiceCollectionExtensions).Assembly,
                typeof(ZLinkMessagePackCodec).Assembly,
                typeof(ZLinkProtobufCodec).Assembly,
                typeof(ZLinkRedisLocationStore).Assembly,
                typeof(Zlink.Framework.LocationProvider.IZLinkLocationStore).Assembly,
                typeof(ZLinkHttpClient).Assembly,
                typeof(IZlinkStreamConnector).Assembly
            }
            .Distinct()
            .ToArray();

    private static Assembly[] GetServerContractAssemblies() =>
        GetContractAssemblies()
            .Where(static assembly => assembly != typeof(ZLinkHttpClient).Assembly
                                      && assembly != typeof(IZlinkStreamConnector).Assembly)
            .ToArray();

    private static bool IsCoveredByDocumentedInterface(
        ContractDeclaration declaration,
        IReadOnlyDictionary<string, ContractDeclaration[]> documentedByKey,
        IReadOnlyCollection<Assembly> assemblies)
    {
        if (declaration.Kind != DeclarationKind.Member || declaration.AssemblyName is null)
            return false;

        var owner = assemblies
            .SelectMany(static assembly => assembly.GetExportedTypes())
            .SingleOrDefault(type => string.Equals(
                type.Assembly.GetName().Name,
                declaration.AssemblyName,
                StringComparison.Ordinal)
                                      && string.Equals(
                                          GetReflectionTypeFullName(type),
                                          declaration.Owner,
                                          StringComparison.Ordinal));
        if (owner is null) return false;

        return owner.GetInterfaces()
            .Where(static type => type.FullName is not null)
            .Select(type => $"{type.Assembly.GetName().Name}::{GetReflectionTypeFullName(type)}")
            .Where(documentedByKey.ContainsKey)
            .SelectMany(key => documentedByKey[key])
            .Any(candidate => candidate.Kind == DeclarationKind.Member
                              && candidate.Signature == declaration.Signature);
    }

    private static IReadOnlyList<PackageType> ExtractPackageTypes(
        string packageApiRoot,
        IReadOnlyCollection<Assembly> assemblies)
    {
        var snapshotIdentities = new HashSet<string>(StringComparer.Ordinal);
        foreach (var path in Directory.EnumerateFiles(
                     packageApiRoot,
                     "*.api.txt",
                     SearchOption.TopDirectoryOnly))
        {
            string? assemblyName = null;
            foreach (var line in File.ReadLines(path))
            {
                if (line.StartsWith("assembly ", StringComparison.Ordinal))
                {
                    assemblyName = line["assembly ".Length..].Trim();
                    continue;
                }

                if (assemblyName is null) continue;
                if (TryReadSnapshotTypeName(line) is { } typeName)
                    snapshotIdentities.Add(
                        $"{assemblyName}::{NormalizeSnapshotTypeName(typeName)}");
            }
        }

        var actualTypes = assemblies
            .SelectMany(static assembly => assembly.GetExportedTypes())
            .Select(type => new PackageType(
                type.Assembly.GetName().Name!,
                GetReflectionTypeFullName(type),
                GetReflectionTypeSimpleKey(type)))
            .GroupBy(static type => type.Identity, StringComparer.Ordinal)
            .Select(static group => group.First())
            .OrderBy(static type => type.Identity, StringComparer.Ordinal)
            .ToArray();
        var actualIdentities = actualTypes
            .Select(static type => type.Identity)
            .ToHashSet(StringComparer.Ordinal);
        var missingFromSnapshot = actualIdentities
            .Except(snapshotIdentities, StringComparer.Ordinal)
            .Order(StringComparer.Ordinal)
            .ToArray();
        var extraInSnapshot = snapshotIdentities
            .Except(actualIdentities, StringComparer.Ordinal)
            .Order(StringComparer.Ordinal)
            .ToArray();

        Assert.True(
            missingFromSnapshot.Length == 0 && extraInSnapshot.Length == 0,
            "Package API snapshot type identities differ from compiled exports. "
            + $"Missing: {string.Join(", ", missingFromSnapshot)}; "
            + $"Unexpected: {string.Join(", ", extraInSnapshot)}");
        return actualTypes;
    }

    private static string NormalizeSnapshotTypeName(string name)
    {
        var normalized = new StringBuilder(name.Length);
        for (var index = 0; index < name.Length; index++)
        {
            if (name[index] != '<')
            {
                normalized.Append(name[index]);
                continue;
            }

            var depth = 1;
            var arity = 1;
            for (index++; index < name.Length && depth > 0; index++)
            {
                switch (name[index])
                {
                    case '<':
                        depth++;
                        break;
                    case '>':
                        depth--;
                        break;
                    case ',' when depth == 1:
                        arity++;
                        break;
                }
            }

            if (depth != 0)
                throw new InvalidOperationException($"Malformed package type name '{name}'.");
            normalized.Append((char)96).Append(arity);
            index--;
        }

        return normalized.ToString();
    }

    private static string? TryReadSnapshotTypeName(string line)
    {
        var text = line.Trim();
        if (!text.StartsWith("type ", StringComparison.Ordinal)) return null;

        var kindEnd = text.IndexOf(' ', "type ".Length);
        if (kindEnd < 0 || kindEnd + 1 >= text.Length) return null;

        var nameStart = kindEnd + 1;
        var depth = 0;
        for (var index = nameStart; index < text.Length; index++)
        {
            switch (text[index])
            {
                case '<':
                    depth++;
                    break;
                case '>':
                    depth--;
                    break;
                case ':' when depth == 0:
                case ' ' when depth == 0:
                case '\t' when depth == 0:
                    return text[nameStart..index];
            }
        }

        return text[nameStart..];
    }

    private static IReadOnlyList<ContractDeclaration> ResolveDocumentOwners(
        IReadOnlyList<ContractDeclaration> declarations,
        IReadOnlyList<PackageType> packageTypes)
    {
        return declarations
            .Select(declaration =>
            {
                var expected = ResolveExpectedTypeIdentity(declaration);
                var matches = packageTypes
                    .Where(type => string.Equals(type.Identity, expected.Identity, StringComparison.Ordinal))
                    .ToArray();
                Assert.True(
                    matches.Length == 1,
                    $"Exact interface owner '{declaration.Document}:{declaration.Owner}' must export the "
                    + $"expected assembly/FQN '{expected.Identity}'. "
                    + $"Compiled candidates: {string.Join(", ", packageTypes
                        .Where(type => string.Equals(type.SimpleKey, declaration.Owner, StringComparison.Ordinal))
                        .Select(static type => type.Identity)
                        .Order(StringComparer.Ordinal))}");
                return declaration with
                {
                    Owner = expected.FullName,
                    AssemblyName = expected.AssemblyName
                };
            })
            .ToArray();
    }

    private static PackageType ResolveExpectedTypeIdentity(ContractDeclaration declaration)
    {
        var document = declaration.Document;
        var ownerKey = $"{document}|{declaration.Owner}";
        var assemblyName = ExpectedAssemblyOverrides.TryGetValue(ownerKey, out var assemblyOverride)
            ? assemblyOverride
            : ExpectedAssemblyByDocument.TryGetValue(document, out var documentAssembly)
                ? documentAssembly
                : throw new InvalidOperationException(
                    $"No exact-interface assembly owner is registered for '{document}'.");

        var fullName = FullyQualifiedDocumentOwner(declaration.Owner)
            ? declaration.Owner
            : ExpectedNamespaceOverrides.TryGetValue(ownerKey, out var namespaceOverride)
                ? $"{namespaceOverride}.{declaration.Owner}"
                : ExpectedNamespaceByDocument.TryGetValue(document, out var documentNamespace)
                    ? $"{documentNamespace}.{declaration.Owner}"
                    : throw new InvalidOperationException(
                        $"No exact-interface namespace owner is registered for '{ownerKey}'.");

        return new PackageType(assemblyName, fullName, declaration.Owner);
    }

    private static bool FullyQualifiedDocumentOwner(string owner) =>
        owner.StartsWith("Zlink.", StringComparison.Ordinal)
        || owner.StartsWith("Systems.", StringComparison.Ordinal);

    // Exact-interface owner metadata is deliberately fixed here. The compiled export is only
    // checked against this expected identity; it is never used to choose a replacement owner.
    private static readonly IReadOnlyDictionary<string, string> ExpectedAssemblyByDocument =
        new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["01-common-runtime.ko.md"] = "Zlink.Framework",
            ["02-configuration-host.ko.md"] = "Zlink.Framework.AspNetCore",
            ["03-configuration-topology.ko.md"] = "Zlink.Framework",
            ["04-channel-messaging.ko.md"] = "Zlink.Framework",
            ["05-spots.ko.md"] = "Zlink.Framework",
            ["06-actors.ko.md"] = "Zlink.Framework",
            ["07-bound-stream-session.ko.md"] = "Zlink.Framework",
            ["07-stream-session.ko.md"] = "Zlink.Framework",
            ["08-authority-relocation.ko.md"] = "Zlink.Framework.Provider.Abstractions",
            ["08-location-maintenance.ko.md"] = "Zlink.Framework",
            ["08-location-provider-redis.ko.md"] = "Zlink.Framework.Locations.Redis",
            ["10-monitoring-errors.ko.md"] = "Zlink.Framework.Contracts",
            ["10-topology-monitoring.ko.md"] = "Zlink.Framework",
            ["11-serialization.ko.md"] = "Zlink.Framework.Contracts"
        };

    private static readonly IReadOnlyDictionary<string, string> ExpectedNamespaceByDocument =
        new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["01-common-runtime.ko.md"] = "Zlink.Framework.Contracts.Channels",
            ["03-configuration-topology.ko.md"] = "Zlink.Framework.Contracts.Configuration",
            ["04-channel-messaging.ko.md"] = "Zlink.Framework.Contracts.Channels",
            ["05-spots.ko.md"] = "Zlink.Framework.Contracts.Spots",
            ["06-actors.ko.md"] = "Zlink.Framework.Contracts.Actors",
            ["07-bound-stream-session.ko.md"] = "Zlink.Framework.Contracts.Streams",
            ["07-stream-session.ko.md"] = "Zlink.Framework.Contracts.Streams",
            ["08-location-maintenance.ko.md"] = "Zlink.Framework.Contracts.Locations",
            ["10-monitoring-errors.ko.md"] = "Zlink.Framework.Contracts.Errors",
            ["10-topology-monitoring.ko.md"] = "Zlink.Framework.Contracts.Configuration",
            ["11-serialization.ko.md"] = "Zlink.Framework.Contracts.Codecs"
        };

    private static readonly IReadOnlyDictionary<string, string> ExpectedNamespaceOverrides =
        new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["01-common-runtime.ko.md|ZLinkMessage"] = "Zlink.Framework.Contracts.Messaging",
            ["01-common-runtime.ko.md|ZLinkMessageMetadata"] = "Zlink.Framework.Contracts.Streams",
            ["01-common-runtime.ko.md|IZLinkWorkerCall`1"] = "Zlink.Framework.Contracts.Workers",
            ["01-common-runtime.ko.md|IZLinkWorkerOptions"] = "Zlink.Framework.Contracts.Workers",
            ["01-common-runtime.ko.md|ZLinkHandlerGroupAttribute"] = "Zlink.Framework.Contracts.Handlers",
            ["01-common-runtime.ko.md|ZLinkRequestAttribute"] = "Zlink.Framework.Contracts.Handlers",
            ["01-common-runtime.ko.md|ZLinkSendAttribute"] = "Zlink.Framework.Contracts.Handlers",
            ["01-common-runtime.ko.md|ZLinkPublishAttribute"] = "Zlink.Framework.Contracts.Handlers",
            ["01-common-runtime.ko.md|ZLinkPacketAttribute"] = "Zlink.Framework.Contracts.Handlers",
            ["01-common-runtime.ko.md|ZLinkSpotRequestAttribute"] = "Zlink.Framework.Contracts.Handlers",
            ["01-common-runtime.ko.md|ZLinkSpotPacketHandlerAttribute"] = "Zlink.Framework.Contracts.Handlers",
            ["01-common-runtime.ko.md|ZLinkSpotRequestHandlerAttribute"] = "Zlink.Framework.Contracts.Handlers",
            ["01-common-runtime.ko.md|ZLinkSpotSubscriptionAttribute"] = "Zlink.Framework.Contracts.Handlers",
            ["01-common-runtime.ko.md|ZLinkSpotSubscriptionHandlerAttribute"] = "Zlink.Framework.Contracts.Handlers",
            ["01-common-runtime.ko.md|ZLinkSpotActorSendAttribute"] = "Zlink.Framework.Contracts.Handlers",
            ["01-common-runtime.ko.md|ZLinkSpotActorSendHandlerAttribute"] = "Zlink.Framework.Contracts.Handlers",
            ["01-common-runtime.ko.md|ZLinkSpotActorRequestAttribute"] = "Zlink.Framework.Contracts.Handlers",
            ["01-common-runtime.ko.md|ZLinkSpotActorRequestHandlerAttribute"] = "Zlink.Framework.Contracts.Handlers",
            ["01-common-runtime.ko.md|ZLinkSpotTimerHandlerAttribute"] = "Zlink.Framework.Contracts.Handlers",
            ["01-common-runtime.ko.md|ZLinkStreamPacketAttribute"] = "Zlink.Framework.Contracts.Handlers",
            ["03-configuration-topology.ko.md|ZLinkHandlerDispatchKind"] = "Zlink.Framework.Contracts.Handlers",
            ["03-configuration-topology.ko.md|IZLinkHandlerFilterContext"] = "Zlink.Framework.Contracts.Handlers",
            ["03-configuration-topology.ko.md|ZLinkHandlerFilterNext"] = "Zlink.Framework.Contracts.Handlers",
            ["03-configuration-topology.ko.md|IZLinkHandlerFilter"] = "Zlink.Framework.Contracts.Handlers",
            ["03-configuration-topology.ko.md|IZLinkMessageContext"] = "Zlink.Framework.Contracts.Handlers",
            ["03-configuration-topology.ko.md|IZLinkMetadataCall`1"] = "Zlink.Framework.Contracts.Channels",
            ["04-channel-messaging.ko.md|IZLinkMessageContext"] = "Zlink.Framework.Contracts.Handlers",
            ["04-channel-messaging.ko.md|IZLinkSendHandler`1"] = "Zlink.Framework.Contracts.Handlers",
            ["04-channel-messaging.ko.md|IZLinkRequestHandler`2"] = "Zlink.Framework.Contracts.Handlers",
            ["04-channel-messaging.ko.md|IZLinkFanoutHandler`1"] = "Zlink.Framework.Contracts.Handlers",
            ["05-spots.ko.md|IZLinkTimer"] = "Zlink.Framework.Contracts.Timers",
            ["05-spots.ko.md|ZLinkTimerOptions"] = "Zlink.Framework.Contracts.Timers",
            ["05-spots.ko.md|ZLinkTimerOverrunPolicy"] = "Zlink.Framework.Contracts.Timers",
            ["05-spots.ko.md|ZLinkTimerTick"] = "Zlink.Framework.Contracts.Timers",
            ["06-actors.ko.md|ActorRef"] = "Systems.Zlink",
            ["06-actors.ko.md|IZLinkActorHandlerRegistry"] = "Zlink.Framework.Contracts.Spots",
            ["10-topology-monitoring.ko.md|ZLinkUnhandledDispatchAction"] = "Zlink.Framework.Contracts.Dispatch",
            ["10-topology-monitoring.ko.md|IZLinkUnhandledDispatchOptions"] = "Zlink.Framework.Contracts.Dispatch",
            ["10-topology-monitoring.ko.md|ZLinkDiagnosticsLevel"] = "Zlink.Framework.Contracts.Dispatch",
            ["10-topology-monitoring.ko.md|IZLinkDiagnosticsOptions"] = "Zlink.Framework.Contracts.Dispatch",
            ["10-topology-monitoring.ko.md|IZLinkDispatchOptions"] = "Zlink.Framework.Contracts.Dispatch",
            ["10-topology-monitoring.ko.md|IZLinkDiagnosticsRuntime"] = "Zlink.Framework.Contracts.Dispatch",
            ["02-configuration-host.ko.md|ServiceCollectionExtensions"] = "Zlink.Framework.AspNetCore",
            ["11-serialization.ko.md|ZLinkMessagePackCodec"] = "Zlink.Framework.Codecs.MessagePack",
            ["11-serialization.ko.md|ZLinkProtobufCodec"] = "Zlink.Framework.Codecs.Protobuf"
        };

    private static readonly IReadOnlyDictionary<string, string> ExpectedAssemblyOverrides =
        new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["02-configuration-host.ko.md|ServiceCollectionExtensions"] = "Zlink.Framework.AspNetCore",
            ["11-serialization.ko.md|ZLinkMessagePackCodec"] = "Zlink.Framework.Codecs.MessagePack",
            ["11-serialization.ko.md|ZLinkProtobufCodec"] = "Zlink.Framework.Codecs.Protobuf"
        };

    private static bool ContainsDeclaration(
        IReadOnlyDictionary<string, ContractDeclaration[]> declarations,
        ContractDeclaration candidate) =>
        declarations.TryGetValue(candidate.QualifiedOwner, out var ownerDeclarations)
        && ownerDeclarations.Any(existing =>
            existing.Kind == candidate.Kind
            && existing.Signature == candidate.Signature);

    private static IReadOnlyList<ContractDeclaration> ExtractExactInterfaceDeclarations(string interfaceRoot) =>
        ExtractSyntaxDeclarations(
            Directory
                .EnumerateFiles(interfaceRoot, "*.ko.md", SearchOption.TopDirectoryOnly)
                .Where(path => !string.Equals(
                    Path.GetFileName(path),
                    "README.ko.md",
                    StringComparison.Ordinal))
                .SelectMany(path => ExtractCSharpBlocks(path).Select(body => (
                    Document: Path.GetFileName(path),
                    Body: body,
                    AssemblyName: (string?)null))),
            includeOnlyPublic: true,
            exactInterfaceDocuments: true);

    private static IReadOnlyList<ContractDeclaration> ExtractSourceDeclarations(string sourceRoot) =>
        ExtractSyntaxDeclarations(
            Directory
                .EnumerateFiles(sourceRoot, "*.cs", SearchOption.AllDirectories)
                .Where(path => !path.Contains(
                                   $"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}",
                                   StringComparison.Ordinal)
                               && !path.Contains(
                                   $"{Path.DirectorySeparatorChar}bin{Path.DirectorySeparatorChar}",
                                   StringComparison.Ordinal))
                .Select(path => (
                    Document: Path.GetRelativePath(sourceRoot, path),
                    Body: File.ReadAllText(path),
                    AssemblyName: SourceAssemblyName(sourceRoot, path))),
            includeOnlyPublic: true,
            exactInterfaceDocuments: false);

    private static string? SourceAssemblyName(string sourceRoot, string path)
    {
        var relative = Path.GetRelativePath(sourceRoot, path);
        var projectName = relative.Split(Path.DirectorySeparatorChar)[0];
        return string.Equals(projectName, "Shared", StringComparison.Ordinal)
            ? null
            : projectName;
    }

    private static IEnumerable<string> ExtractCSharpBlocks(string path)
    {
        var text = File.ReadAllText(path);
        foreach (Match block in Regex.Matches(
                     text,
                     @"\x60{3}csharp\s*(?<body>.*?)\x60{3}",
                     RegexOptions.Singleline | RegexOptions.CultureInvariant))
            yield return block.Groups["body"].Value;
    }

    private static IReadOnlyList<ContractDeclaration> ExtractSyntaxDeclarations(
        IEnumerable<(string Document, string Body, string? AssemblyName)> documents,
        bool includeOnlyPublic,
        bool exactInterfaceDocuments)
    {
        var declarations = new List<ContractDeclaration>();
        var parseOptions = new CSharpParseOptions(LanguageVersion.Preview);

        var syntaxDocuments = documents
            .Select((document, index) => new SyntaxDocument(
                document.Document,
                document.Body,
                document.AssemblyName,
                exactInterfaceDocuments,
                index))
            .ToArray();

        foreach (var documentGroup in syntaxDocuments.GroupBy(
                     document => (
                         document.AssemblyName,
                         document.ExactInterface,
                         Document: document.ExactInterface ? document.Document : null),
                     EqualityComparer<(string? AssemblyName, bool ExactInterface, string? Document)>.Default))
        {
            var binding = SemanticBindingContext.Create(documentGroup, parseOptions);
            foreach (var syntaxDocument in documentGroup)
            {
                var document = syntaxDocument.Document;
                var assemblyName = syntaxDocument.AssemblyName;
                var tree = binding.GetTree(syntaxDocument.Ordinal);
                var syntaxErrors = tree.GetDiagnostics()
                    .Where(static diagnostic => diagnostic.Severity == DiagnosticSeverity.Error)
                    .Select(static diagnostic => diagnostic.ToString())
                    .ToArray();
                Assert.True(
                    syntaxErrors.Length == 0,
                    $"C# contract syntax is invalid in {document}: {string.Join("; ", syntaxErrors)}");

                var resolver = binding.GetResolver(tree);
                var root = tree.GetRoot();
                foreach (var type in root.DescendantNodes().OfType<BaseTypeDeclarationSyntax>())
                {
                    if (includeOnlyPublic && !IsPublicType(type)) continue;

                    var owner = GetTypeKey(type);
                    declarations.Add(new ContractDeclaration(
                        document,
                        DeclarationKind.Type,
                        owner,
                        TypeSignature(type, resolver),
                        assemblyName));

                    if (type is TypeDeclarationSyntax primaryType
                        && primaryType.ParameterList is { } primaryParameterList)
                    {
                        declarations.Add(new ContractDeclaration(
                            document,
                            DeclarationKind.Constructor,
                            owner,
                            ConstructorSignature(type.Identifier.Text, primaryParameterList, default, resolver),
                            assemblyName));

                        if (primaryType is RecordDeclarationSyntax)
                            declarations.AddRange(RecordProjectionDeclarations(
                                document,
                                owner,
                                primaryParameterList,
                                assemblyName,
                                resolver));
                    }

                    if (type is TypeDeclarationSyntax typeDeclaration)
                    {
                        foreach (var member in typeDeclaration.Members)
                        {
                            if (member is BaseTypeDeclarationSyntax or DelegateDeclarationSyntax) continue;
                            if (includeOnlyPublic && !IsPublicMember(type, member)) continue;
                            if (MemberSignature(member, resolver) is { } signature)
                                declarations.Add(new ContractDeclaration(
                                    document,
                                    member is ConstructorDeclarationSyntax
                                        ? DeclarationKind.Constructor
                                        : DeclarationKind.Member,
                                    owner,
                                    signature,
                                    assemblyName));
                        }
                    }

                    if (type is EnumDeclarationSyntax enumDeclaration)
                        foreach (var member in enumDeclaration.Members)
                            declarations.Add(new ContractDeclaration(
                                document,
                                DeclarationKind.Member,
                                owner,
                                NormalizeSyntax(member.ToFullString()),
                                assemblyName));
                }

                foreach (var @delegate in root.DescendantNodes().OfType<DelegateDeclarationSyntax>())
                {
                    if (includeOnlyPublic && !@delegate.Modifiers.Any(SyntaxKind.PublicKeyword)) continue;
                    declarations.Add(new ContractDeclaration(
                        document,
                        DeclarationKind.Type,
                        GetDelegateKey(@delegate),
                        DelegateSignature(@delegate, resolver),
                        assemblyName));
                }
            }
        }

        return declarations
            .Distinct()
            .OrderBy(static declaration => declaration.Owner, StringComparer.Ordinal)
            .ThenBy(static declaration => declaration.Kind)
            .ThenBy(static declaration => declaration.Signature, StringComparer.Ordinal)
            .ToArray();
    }

    private static IEnumerable<ContractDeclaration> RecordProjectionDeclarations(
        string document,
        string owner,
        ParameterListSyntax parameters,
        string? assemblyName,
        SemanticTypeIdentityResolver resolver)
    {
        foreach (var parameter in parameters.Parameters)
        {
            if (parameter.Type is null)
                throw new InvalidOperationException(
                    $"A positional record parameter must have a type: {document}:{owner}:{parameter}");

            yield return new ContractDeclaration(
                document,
                DeclarationKind.Member,
                owner,
                NormalizeSyntax(
                    $"{CanonicalTypeText(parameter.Type, resolver)} {parameter.Identifier}{{get;init;}}"),
                assemblyName);
        }

        var deconstructParameters = parameters.Parameters.Select(parameter =>
        {
            if (parameter.Type is null)
                throw new InvalidOperationException(
                    $"A positional record parameter must have a type: {document}:{owner}:{parameter}");

            return $"out {CanonicalTypeText(parameter.Type, resolver)} {parameter.Identifier.Text}";
        });
        yield return new ContractDeclaration(
            document,
            DeclarationKind.Member,
            owner,
            NormalizeSyntax($"System.Void Deconstruct({string.Join(",", deconstructParameters)})"),
            assemblyName);
    }

    private static bool IsPublicType(BaseTypeDeclarationSyntax type) =>
        type.Modifiers.Any(SyntaxKind.PublicKeyword)
        && type.Ancestors()
            .OfType<BaseTypeDeclarationSyntax>()
            .All(static ancestor => ancestor.Modifiers.Any(SyntaxKind.PublicKeyword));

    private static bool IsPublicMember(BaseTypeDeclarationSyntax owner, MemberDeclarationSyntax member)
    {
        if (member is MethodDeclarationSyntax { ExplicitInterfaceSpecifier: not null }
            or PropertyDeclarationSyntax { ExplicitInterfaceSpecifier: not null }
            or IndexerDeclarationSyntax { ExplicitInterfaceSpecifier: not null })
            return false;
        if (owner is InterfaceDeclarationSyntax) return true;
        if (member is BaseTypeDeclarationSyntax or DelegateDeclarationSyntax) return false;

        var modifiers = GetModifiers(member);
        return modifiers.Any(SyntaxKind.PublicKeyword)
               || modifiers.Any(SyntaxKind.ProtectedKeyword)
                  && modifiers.Any(SyntaxKind.PrivateKeyword);
    }

    private static string GetTypeKey(BaseTypeDeclarationSyntax type) =>
        JoinNamespaceAndTypeKey(
            type,
            type.Ancestors()
                .OfType<BaseTypeDeclarationSyntax>()
                .Reverse()
                .Select(NameWithArity)
                .Append(NameWithArity(type)));

    private static string GetDelegateKey(DelegateDeclarationSyntax @delegate) =>
        JoinNamespaceAndTypeKey(
            @delegate,
            @delegate.Ancestors()
                .OfType<BaseTypeDeclarationSyntax>()
                .Reverse()
                .Select(NameWithArity)
                .Append(NameWithArity(@delegate)));

    private static string JoinNamespaceAndTypeKey(
        SyntaxNode node,
        IEnumerable<string> typeParts)
    {
        var typeKey = string.Join(".", typeParts);
        var namespaceKey = string.Join(
            ".",
            node.Ancestors()
                .OfType<BaseNamespaceDeclarationSyntax>()
                .Reverse()
                .Select(static declaration => declaration.Name.ToString()));
        return namespaceKey.Length == 0 ? typeKey : $"{namespaceKey}.{typeKey}";
    }

    private static string NameWithArity(BaseTypeDeclarationSyntax type)
    {
        var typeParameterList = (type as TypeDeclarationSyntax)?.TypeParameterList;
        return $"{type.Identifier.Text}{(typeParameterList?.Parameters.Count > 0 ? $"{(char)96}{typeParameterList.Parameters.Count}" : string.Empty)}";
    }

    private static string NameWithArity(DelegateDeclarationSyntax @delegate) =>
        $"{@delegate.Identifier.Text}{(@delegate.TypeParameterList?.Parameters.Count > 0 ? $"{(char)96}{@delegate.TypeParameterList.Parameters.Count}" : string.Empty)}";

    private static string TypeSignature(
        BaseTypeDeclarationSyntax type,
        SemanticTypeIdentityResolver resolver)
    {
        var keyword = type switch
        {
            InterfaceDeclarationSyntax => "interface",
            ClassDeclarationSyntax => "class",
            StructDeclarationSyntax => "struct",
            EnumDeclarationSyntax => "enum",
            RecordDeclarationSyntax record => record.ClassOrStructKeyword.IsKind(SyntaxKind.StructKeyword)
                ? "record struct"
                : "record",
            _ => throw new InvalidOperationException($"Unsupported type declaration: {type.Kind()}")
        };
        var baseList = type.BaseList is null
            ? string.Empty
            : $" {CanonicalBaseList(type.BaseList, resolver)}";
        var constraints = type is TypeDeclarationSyntax declaredType
            ? string.Concat(declaredType.ConstraintClauses.Select(clause => $" {CanonicalSyntax(clause, resolver)}"))
            : string.Empty;
        return NormalizeSyntax(
            $"{NormalizeModifiers(type.Modifiers)} {keyword} {NameWithArity(type)}{baseList}{constraints}");
    }

    private static string CanonicalBaseList(
        BaseListSyntax baseList,
        SemanticTypeIdentityResolver resolver) =>
        $": {string.Join(",", baseList.Types.Select(baseType => baseType switch
        {
            SimpleBaseTypeSyntax simple => CanonicalTypeText(simple.Type, resolver),
            PrimaryConstructorBaseTypeSyntax primary => CanonicalTypeText(primary.Type, resolver),
            _ => baseType.ToString()
        }))}";

    private static string DelegateSignature(
        DelegateDeclarationSyntax @delegate,
        SemanticTypeIdentityResolver resolver) =>
        NormalizeSyntax(
            $"{NormalizeModifiers(@delegate.Modifiers)} delegate {CanonicalTypeText(@delegate.ReturnType, resolver)} "
            + $"{NameWithArity(@delegate)}{CanonicalSyntax(@delegate.ParameterList, resolver)}"
            + $"{string.Concat(@delegate.ConstraintClauses.Select(clause => $" {CanonicalSyntax(clause, resolver)}"))}");

    private static string ConstructorSignature(
        string name,
        ParameterListSyntax parameters,
        SyntaxTokenList modifiers,
        SemanticTypeIdentityResolver resolver) =>
        NormalizeSyntax($"{NormalizeModifiers(modifiers)} {name}{CanonicalSyntax(parameters, resolver)}");

    private static string GetReflectionTypeFullName(Type type) =>
        type.FullName?.Replace('+', '.')
        ?? throw new InvalidOperationException($"Exported type {type} has no full name.");

    private static string GetReflectionTypeSimpleKey(Type type)
    {
        var parts = new Stack<string>();
        for (var current = type; current is not null; current = current.DeclaringType)
            parts.Push($"{current.Name.Split((char)96)[0]}{(current.IsGenericTypeDefinition ? $"{(char)96}{current.GetGenericArguments().Length}" : string.Empty)}");
        return string.Join(".", parts);
    }

    private static string? MemberSignature(
        MemberDeclarationSyntax member,
        SemanticTypeIdentityResolver resolver) => member switch
    {
        ConstructorDeclarationSyntax constructor => ConstructorSignature(
            constructor.Identifier.Text,
            constructor.ParameterList,
            constructor.Modifiers,
            resolver),
        MethodDeclarationSyntax method => NormalizeSyntax(
            $"{NormalizeModifiers(method.Modifiers)} {CanonicalTypeText(method.ReturnType, resolver)} {method.Identifier}{method.TypeParameterList}"
            + $"{CanonicalSyntax(method.ParameterList, resolver)}"
            + $"{string.Concat(method.ConstraintClauses.Select(clause => $" {CanonicalSyntax(clause, resolver)}"))}"),
        PropertyDeclarationSyntax property => NormalizeSyntax(
            $"{NormalizeModifiers(property.Modifiers)} {CanonicalTypeText(property.Type, resolver)} "
            + $"{CanonicalSyntax(property.ExplicitInterfaceSpecifier, resolver)}"
            + $"{property.Identifier}{PropertyAccessorSignature(property)}"),
        IndexerDeclarationSyntax indexer => NormalizeSyntax(
            $"{NormalizeModifiers(indexer.Modifiers)} {CanonicalTypeText(indexer.Type, resolver)} "
            + $"{CanonicalSyntax(indexer.ExplicitInterfaceSpecifier, resolver)}"
            + $"this{CanonicalSyntax(indexer.ParameterList, resolver)}{indexer.AccessorList}"),
        FieldDeclarationSyntax field => NormalizeSyntax(
            $"{NormalizeModifiers(field.Modifiers)} {CanonicalSyntax(field.Declaration, resolver)}"),
        EventFieldDeclarationSyntax eventField => NormalizeSyntax(
            $"{NormalizeModifiers(eventField.Modifiers)} event {CanonicalSyntax(eventField.Declaration, resolver)}"),
        EventDeclarationSyntax @event => NormalizeSyntax(
            $"{NormalizeModifiers(@event.Modifiers)} event {CanonicalTypeText(@event.Type, resolver)} "
            + $"{CanonicalSyntax(@event.ExplicitInterfaceSpecifier, resolver)}"
            + $"{@event.Identifier}{@event.AccessorList}"),
        OperatorDeclarationSyntax operatorDeclaration => NormalizeSyntax(
            $"{NormalizeModifiers(operatorDeclaration.Modifiers)} {CanonicalTypeText(operatorDeclaration.ReturnType, resolver)}"
            + $" operator {operatorDeclaration.OperatorToken}{CanonicalSyntax(operatorDeclaration.ParameterList, resolver)}"),
        ConversionOperatorDeclarationSyntax conversion => NormalizeSyntax(
            $"{NormalizeModifiers(conversion.Modifiers)} {conversion.ImplicitOrExplicitKeyword} operator"
            + $" {CanonicalTypeText(conversion.Type, resolver)}{CanonicalSyntax(conversion.ParameterList, resolver)}"),
        _ => null
    };

    private static string PropertyAccessorSignature(PropertyDeclarationSyntax property)
    {
        if (property.AccessorList is null)
            return "{get;}";

        return $"{{{string.Join(
            string.Empty,
            property.AccessorList.Accessors.Select(accessor =>
                $"{NormalizeModifiers(accessor.Modifiers)}{accessor.Keyword.Text};"))}}}";
    }

    private static SyntaxTokenList GetModifiers(MemberDeclarationSyntax member) => member switch
    {
        ConstructorDeclarationSyntax constructor => constructor.Modifiers,
        MethodDeclarationSyntax method => method.Modifiers,
        PropertyDeclarationSyntax property => property.Modifiers,
        IndexerDeclarationSyntax indexer => indexer.Modifiers,
        FieldDeclarationSyntax field => field.Modifiers,
        EventFieldDeclarationSyntax eventField => eventField.Modifiers,
        EventDeclarationSyntax @event => @event.Modifiers,
        OperatorDeclarationSyntax operatorDeclaration => operatorDeclaration.Modifiers,
        ConversionOperatorDeclarationSyntax conversion => conversion.Modifiers,
        _ => default
    };

    private static string NormalizeModifiers(SyntaxTokenList modifiers) =>
        string.Join(
            " ",
            modifiers
                .Where(static modifier => modifier.Kind() is not (
                    SyntaxKind.PublicKeyword
                    or SyntaxKind.PrivateKeyword
                    or SyntaxKind.ProtectedKeyword
                    or SyntaxKind.InternalKeyword
                    or SyntaxKind.PartialKeyword
                    or SyntaxKind.AsyncKeyword))
                .Select(static modifier => modifier.Text));

    private static string CanonicalTypeText(
        TypeSyntax type,
        SemanticTypeIdentityResolver resolver) =>
        NormalizeSyntax(resolver.Resolve(type));

    private static string CanonicalSyntax(
        SyntaxNode? node,
        SemanticTypeIdentityResolver resolver)
    {
        if (node is null)
            return string.Empty;

        var text = node.ToFullString();
        var replacements = node.DescendantNodesAndSelf()
            .OfType<TypeSyntax>()
            // `notnull` is a constraint keyword, not a type symbol. Roslyn
            // represents it in the constraint syntax as an identifier-like
            // TypeSyntax, but semantic binding cannot resolve it as a type.
            // Keep the exact constraint text in the canonical signature.
            .Where(type => !string.Equals(
                               type.ToString(),
                               "notnull",
                               StringComparison.Ordinal)
                           && !type.Ancestors().Any(
                               static ancestor => ancestor is TypeSyntax))
            .OrderByDescending(static type => type.SpanStart)
            .ToArray();
        foreach (var type in replacements)
        {
            var start = type.SpanStart - node.FullSpan.Start;
            text = text.Remove(start, type.Span.Length)
                .Insert(start, resolver.Resolve(type));
        }

        return text;
    }

    private static string NormalizeSyntax(
        SyntaxNode node,
        SemanticTypeIdentityResolver resolver) =>
        NormalizeSyntax(CanonicalSyntax(node, resolver));

    private static string NormalizeSyntax(string text)
    {
        var normalized = Regex.Replace(text, @"\s+", " ", RegexOptions.CultureInvariant).Trim().TrimEnd(';');
        normalized = normalized.Replace("global::", string.Empty, StringComparison.Ordinal);
        normalized = Regex.Replace(
            normalized,
            @"\s*([<>,()\[\]{}?:;=])\s*",
            "$1",
            RegexOptions.CultureInvariant);
        return normalized;
    }

    private sealed record SyntaxDocument(
        string Document,
        string Body,
        string? AssemblyName,
        bool ExactInterface,
        int Ordinal);

    private sealed class SemanticBindingContext
    {
        private readonly IReadOnlyDictionary<int, SyntaxTree> _trees;
        private readonly IReadOnlyDictionary<SyntaxTree, SemanticTypeIdentityResolver> _resolvers;

        private SemanticBindingContext(
            IReadOnlyDictionary<int, SyntaxTree> trees,
            IReadOnlyDictionary<SyntaxTree, SemanticTypeIdentityResolver> resolvers)
        {
            _trees = trees;
            _resolvers = resolvers;
        }

        public static SemanticBindingContext Create(
            IEnumerable<SyntaxDocument> documents,
            CSharpParseOptions parseOptions)
        {
            var materialized = documents.OrderBy(static document => document.Ordinal).ToArray();
            if (materialized.Length == 0)
                throw new InvalidOperationException("Semantic binding requires at least one syntax document.");

            var trees = materialized.ToDictionary(
                document => document.Ordinal,
                document => CSharpSyntaxTree.ParseText(
                    document.ExactInterface
                        ? ExactInterfaceBindingPrefix + document.Body
                        : CommonSourceBindingPrefix + document.Body,
                    parseOptions,
                    path: $"{document.Document}#{document.Ordinal}"));
            var compilation = CSharpCompilation.Create(
                materialized[0].ExactInterface
                    ? "Zlink.Framework.ExactInterfaceContracts"
                    : materialized[0].AssemblyName ?? "Zlink.Framework.SharedSource",
                trees.Values,
                MetadataReferences(),
                new CSharpCompilationOptions(
                    OutputKind.DynamicallyLinkedLibrary,
                    nullableContextOptions: NullableContextOptions.Enable));
            var resolvers = trees.Values.ToDictionary(
                tree => tree,
                tree => new SemanticTypeIdentityResolver(
                    compilation.GetSemanticModel(tree, true),
                    materialized[0].ExactInterface ? materialized[0].Document : null));
            return new SemanticBindingContext(
                trees,
                resolvers);
        }

        public SyntaxTree GetTree(int ordinal) => _trees[ordinal];

        public SemanticTypeIdentityResolver GetResolver(SyntaxTree tree) => _resolvers[tree];

        private static IEnumerable<MetadataReference> MetadataReferences()
        {
            var paths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var assembly in ReferencedAssemblies())
            {
                if (string.IsNullOrWhiteSpace(assembly.Location)
                    || !paths.Add(assembly.Location))
                    continue;

                yield return MetadataReference.CreateFromFile(assembly.Location);
            }
        }

        private static IEnumerable<Assembly> ReferencedAssemblies()
        {
            var assemblies = new Dictionary<string, Assembly>(StringComparer.Ordinal);
            var pending = new Queue<Assembly>(GetContractAssemblies()
                .Concat(
                [
                    typeof(object).Assembly,
                    typeof(Task).Assembly,
                    typeof(System.Linq.Enumerable).Assembly,
                    typeof(System.Collections.Generic.IReadOnlyList<>).Assembly,
                    typeof(System.Threading.CancellationToken).Assembly
                ])
                .Concat(AppDomain.CurrentDomain.GetAssemblies()));
            while (pending.Count > 0)
            {
                var assembly = pending.Dequeue();
                var key = assembly.FullName ?? assembly.GetName().Name;
                if (key is null || !assemblies.TryAdd(key, assembly))
                    continue;

                foreach (var reference in assembly.GetReferencedAssemblies())
                {
                    try
                    {
                        pending.Enqueue(Assembly.Load(reference));
                    }
                    catch (FileNotFoundException)
                    {
                    }
                    catch (FileLoadException)
                    {
                    }
                }
            }

            return assemblies.Values;
        }
    }

    private sealed class SemanticTypeIdentityResolver
    {
        private static readonly SymbolDisplayFormat TypeDisplayFormat = new(
            globalNamespaceStyle: SymbolDisplayGlobalNamespaceStyle.Omitted,
            typeQualificationStyle: SymbolDisplayTypeQualificationStyle.NameAndContainingTypesAndNamespaces,
            genericsOptions: SymbolDisplayGenericsOptions.IncludeTypeParameters,
            miscellaneousOptions: SymbolDisplayMiscellaneousOptions.IncludeNullableReferenceTypeModifier);

        private readonly SemanticModel _model;
        private readonly string? _exactInterfaceDocument;

        public SemanticTypeIdentityResolver(
            SemanticModel model,
            string? exactInterfaceDocument)
        {
            _model = model;
            _exactInterfaceDocument = exactInterfaceDocument;
        }

        public string Resolve(TypeSyntax type)
        {
            var symbol = _model.GetTypeInfo(type).Type;
            if (symbol is null || symbol.Kind == SymbolKind.ErrorType)
            {
                var diagnostics = _model.GetDiagnostics(type.Span)
                    .Where(static diagnostic => diagnostic.Severity == DiagnosticSeverity.Error)
                    .Select(static diagnostic => diagnostic.ToString())
                    .ToArray();
                throw new InvalidOperationException(
                    $"Semantic type binding failed for '{type}' at {type.SyntaxTree?.FilePath}: "
                    + string.Join("; ", diagnostics));
            }

            if (symbol is ITypeParameterSymbol)
                return DisplayName(symbol);

            var (assemblyName, displayName) = ResolveIdentity(symbol);

            return $"{assemblyName}::{displayName}";
        }

        private (string AssemblyName, string DisplayName) ResolveIdentity(ITypeSymbol symbol)
        {
            if (_exactInterfaceDocument is not null
                && symbol is INamedTypeSymbol named
                && named.ContainingAssembly?.Identity.Name == "Zlink.Framework.ExactInterfaceContracts")
            {
                var localOwner = LocalOwnerKey(named);
                var containingNamespace = named.ContainingNamespace?.IsGlobalNamespace == false
                    ? $"{named.ContainingNamespace}.{localOwner}"
                    : localOwner;
                var expected = ResolveExpectedTypeIdentity(
                    new ContractDeclaration(
                        _exactInterfaceDocument,
                        DeclarationKind.Type,
                        containingNamespace,
                        string.Empty));
                var displayName = Regex.Replace(
                    expected.FullName,
                    @"`\d+",
                    string.Empty,
                    RegexOptions.CultureInvariant);
                if (named.TypeArguments.Length > 0)
                {
                    displayName += $"<{string.Join(",", named.TypeArguments.Select(DisplayName))}>";
                }

                return (expected.AssemblyName, displayName);
            }

            var display = DisplayName(symbol);
            var assemblyName = ContainingAssembly(symbol)?.Identity.Name
                               ?? throw new InvalidOperationException(
                                   $"Type '{display}' has no containing assembly.");
            return (assemblyName, display);
        }

        private string DisplayName(ITypeSymbol symbol)
        {
            if (_exactInterfaceDocument is not null
                && symbol is INamedTypeSymbol named
                && named.ContainingAssembly?.Identity.Name == "Zlink.Framework.ExactInterfaceContracts")
            {
                return ResolveIdentity(named).DisplayName;
            }

            if (symbol is INamedTypeSymbol namedType && !namedType.IsTupleType)
            {
                var containingName = namedType.ContainingType is not null
                    ? DisplayName(namedType.ContainingType)
                    : namedType.ContainingNamespace?.IsGlobalNamespace == false
                        ? namedType.ContainingNamespace.ToDisplayString()
                        : string.Empty;
                var displayName = string.IsNullOrEmpty(containingName)
                    ? namedType.Name
                    : $"{containingName}.{namedType.Name}";
                if (namedType.TypeArguments.Length > 0)
                {
                    displayName += $"<{string.Join(",", namedType.TypeArguments.Select(DisplayName))}>";
                }

                if (namedType.IsReferenceType
                    && namedType.NullableAnnotation == NullableAnnotation.Annotated)
                    displayName += "?";
                return displayName;
            }

            if (symbol is IArrayTypeSymbol array)
            {
                var rank = array.Rank == 1 ? "[]" : $"[{new string(',', array.Rank - 1)}]";
                return DisplayName(array.ElementType) + rank;
            }

            if (symbol is IPointerTypeSymbol pointer)
                return $"{DisplayName(pointer.PointedAtType)}*";

            return symbol.ToDisplayString(TypeDisplayFormat).Replace(
                "global::",
                string.Empty,
                StringComparison.Ordinal);
        }

        private static string LocalOwnerKey(INamedTypeSymbol type)
        {
            var parts = new Stack<string>();
            for (var current = type; current is not null; current = current.ContainingType)
            {
                parts.Push($"{current.Name}{(current.Arity > 0 ? $"{(char)96}{current.Arity}" : string.Empty)}");
            }

            return string.Join(".", parts);
        }

        private static IAssemblySymbol? ContainingAssembly(ITypeSymbol symbol) => symbol switch
        {
            IArrayTypeSymbol array => ContainingAssembly(array.ElementType),
            IPointerTypeSymbol pointer => ContainingAssembly(pointer.PointedAtType),
            IFunctionPointerTypeSymbol functionPointer =>
                ContainingAssembly(functionPointer.Signature.ReturnType),
            _ => symbol.ContainingAssembly
        };
    }

    private const string ExactInterfaceBindingPrefix = """
using System;
using System.Buffers;
using System.Collections.Generic;
using System.Collections.Immutable;
using System.Diagnostics;
using System.Diagnostics.Metrics;
using System.Net;
using System.Reflection;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Diagnostics.HealthChecks;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Google.Protobuf;
using StackExchange.Redis;
using Systems.Zlink;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Codecs.MessagePack;
using Zlink.Framework.Codecs.Protobuf;
using Zlink.Framework.Contracts;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Codecs;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Contracts.Streams;
using Zlink.Framework.Contracts.Timers;
using Zlink.Framework.Contracts.Workers;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Locations.Redis;
using Zlink.HttpClient;
""";

    private const string CommonSourceBindingPrefix = """
using System;
using System.Buffers;
using System.Collections.Generic;
using System.Collections.Immutable;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
""";

    private enum DeclarationKind
    {
        Type,
        Constructor,
        Member
    }

    private sealed record ContractDeclaration(
        string Document,
        DeclarationKind Kind,
        string Owner,
        string Signature,
        string? AssemblyName = null)
    {
        public string QualifiedOwner =>
            AssemblyName is null ? Owner : $"{AssemblyName}::{Owner}";

        public override string ToString() => $"{Document}:{Owner}:{Kind}:{Signature}";
    }

    private sealed record PackageType(
        string AssemblyName,
        string FullName,
        string SimpleKey)
    {
        public string Identity => $"{AssemblyName}::{FullName}";
    }

    private sealed record RecordProjectionContract(
        string Document,
        string AssemblyName,
        string FullName,
        string[] Parameters);

    private static string FindRepositoryRoot()
    {
        var current = new DirectoryInfo(AppContext.BaseDirectory);
        while (current is not null)
        {
            if (File.Exists(Path.Combine(current.FullName, "AGENTS.md")))
                return current.FullName;
            current = current.Parent;
        }

        throw new DirectoryNotFoundException("Could not locate the repository root containing AGENTS.md.");
    }

    private static void AssertMethodParameter(
        Type contractType,
        string methodName,
        string parameterName,
        Type requiredParameterType,
        Type disallowedParameterType)
    {
        var matchingMethods = EnumerateInterfaceMethods(contractType)
            .Where(method => method.Name == methodName
                             && method.GetParameters().Any(parameter => parameter.Name == parameterName))
            .ToArray();

        Assert.NotEmpty(matchingMethods);

        Assert.Contains(
            matchingMethods,
            method => method.GetParameters()
                .Single(parameter => parameter.Name == parameterName)
                .ParameterType == requiredParameterType);

        foreach (var method in matchingMethods)
        {
            var parameter = Assert.Single(
                method.GetParameters(),
                parameter => parameter.Name == parameterName);
            Assert.NotEqual(disallowedParameterType, parameter.ParameterType);
        }
    }

    private static void AssertMethodParameterIsNot(
        Type contractType,
        string methodName,
        string parameterName,
        Type disallowedParameterType)
    {
        var matchingMethods = EnumerateInterfaceMethods(contractType)
            .Where(method => method.Name == methodName
                             && method.GetParameters().Any(parameter => parameter.Name == parameterName))
            .ToArray();

        Assert.NotEmpty(matchingMethods);

        foreach (var method in matchingMethods)
        {
            var parameter = Assert.Single(
                method.GetParameters(),
                parameter => parameter.Name == parameterName);
            Assert.NotEqual(disallowedParameterType, parameter.ParameterType);
        }
    }

    private static IEnumerable<MethodInfo> EnumerateInterfaceMethods(Type interfaceType)
    {
        foreach (var method in interfaceType.GetMethods())
            yield return method;

        foreach (var inheritedInterface in interfaceType.GetInterfaces())
        {
            foreach (var method in inheritedInterface.GetMethods())
                yield return method;
        }
    }
}
