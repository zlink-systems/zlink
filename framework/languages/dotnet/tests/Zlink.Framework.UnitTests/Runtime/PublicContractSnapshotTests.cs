namespace Zlink.Framework.UnitTests;

public sealed class PublicContractSnapshotTests
{
    [Fact]
    public void Renderer_Preserves_CSharp_PublicContract_Distinctions()
    {
        var snapshot = PublicContractSnapshot.RenderTypes(
        [
            typeof(ContractFixture<>),
            typeof(IVariantContract<,>),
            typeof(ReadOnlyContract),
            typeof(ByRefContract),
            typeof(SnapshotFixtureExtensions),
            typeof(SnapshotFixtureAttribute)
        ]);

        Assert.Contains("required System.String Name { get; init; }", snapshot);
        Assert.Contains("System.String? NullableField", snapshot);
        Assert.Contains("event System.Func<System.String?, System.String?>? Changed", snapshot);
        Assert.Contains("generic T variance=invariant", snapshot);
        Assert.Contains("where T : unmanaged", snapshot);
        Assert.Contains("generic TInput variance=in", snapshot);
        Assert.Contains("generic TOutput variance=out", snapshot);
        Assert.Contains("where TOutput : class?", snapshot);
        Assert.Contains("type readonly-struct", snapshot);
        Assert.Contains("type ref-struct", snapshot);
        Assert.Contains("ref readonly System.Int32 Read", snapshot);
        Assert.Contains("out System.String? output", snapshot);
        Assert.Contains("in System.Int32 input", snapshot);
        Assert.Contains("this System.String value", snapshot);
        Assert.Contains("System.Threading.CancellationToken cancellationToken = default", snapshot);
        Assert.Contains("attribute-usage targets=Class, Struct allow-multiple=true inherited=false", snapshot);
    }

    public sealed class ContractFixture<T>
        where T : unmanaged
    {
        public required string Name { get; init; }

        public string? NullableField;

        public event Func<string?, string?>? Changed;

        public void Raise() => _ = Changed?.Invoke(null);
    }

    public interface IVariantContract<in TInput, out TOutput>
        where TInput : notnull
        where TOutput : class?
    {
        TOutput Convert(TInput input);
    }

    public readonly struct ReadOnlyContract;

    public ref struct ByRefContract
    {
        private static int _value;

        public static ref readonly int Read() => ref _value;

        public static void Parameters(in int input, out string? output)
        {
            output = input.ToString();
        }
    }
}

public static class SnapshotFixtureExtensions
{
    public static void Apply(
        this string value,
        CancellationToken cancellationToken = default)
    {
        _ = value;
        _ = cancellationToken;
    }
}

[AttributeUsage(AttributeTargets.Class | AttributeTargets.Struct, AllowMultiple = true, Inherited = false)]
public sealed class SnapshotFixtureAttribute : Attribute;
