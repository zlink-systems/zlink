using System.Reflection;
using System.Text.Json;
using Microsoft.Extensions.Configuration;

namespace Zlink.Framework.E2E.Configuration;

internal static class E2eConfiguration
{
    public static string Write(string directory, string name, object options)
    {
        Directory.CreateDirectory(directory);
        var path = Path.Combine(directory, $"{name}.json");
        File.WriteAllText(path, JsonSerializer.Serialize(new { Options = options }));
        if (!OperatingSystem.IsWindows())
            File.SetUnixFileMode(path, UnixFileMode.UserRead | UnixFileMode.UserWrite);
        return path;
    }

    public static T Load<T>(string[] args)
        where T : class
    {
        if (args.Length != 2
            || !string.Equals(args[0], "--config", StringComparison.Ordinal)
            || string.IsNullOrWhiteSpace(args[1]))
            throw new ArgumentException("Usage: --config PATH");

        var path = Path.GetFullPath(args[1]);
        if (!File.Exists(path))
            throw new FileNotFoundException($"Configuration file was not found: {path}", path);

        var fileConfiguration = new ConfigurationBuilder()
            .AddJsonFile(path, optional: false, reloadOnChange: false)
            .Build();
        var section = fileConfiguration.GetRequiredSection("Options");
        ValidatePresence<T>(section);
        var options = section.Get<T>()
                      ?? throw new InvalidOperationException(
                          $"Options could not be bound to {typeof(T).Name}.");
        Validate(options);
        return options;
    }

    private static void ValidatePresence<T>(IConfigurationSection section)
        where T : class
    {
        var constructor = typeof(T).GetConstructors().SingleOrDefault();
        if (constructor is null) return;

        var nullability = new NullabilityInfoContext();
        foreach (var parameter in constructor.GetParameters())
        {
            if (parameter.HasDefaultValue) continue;
            if (!parameter.ParameterType.IsValueType
                && nullability.Create(parameter).ReadState == NullabilityState.Nullable)
                continue;
            if (!section.GetSection(parameter.Name!).Exists())
                throw new InvalidOperationException($"Options.{parameter.Name} is required.");
        }
    }

    private static void Validate<T>(T options)
        where T : class
    {
        var nullability = new NullabilityInfoContext();
        foreach (var property in typeof(T).GetProperties(BindingFlags.Instance | BindingFlags.Public))
        {
            var value = property.GetValue(options);
            if (value is string text)
            {
                if (string.IsNullOrWhiteSpace(text))
                {
                    if (nullability.Create(property).ReadState == NullabilityState.NotNull)
                        throw new InvalidOperationException($"Options.{property.Name} must not be empty.");
                    continue;
                }
                ValidateEndpoint(property.Name, text);
                continue;
            }

            if (value is null
                && property.PropertyType.IsClass
                && nullability.Create(property).ReadState == NullabilityState.NotNull)
                throw new InvalidOperationException($"Options.{property.Name} is required.");

            if (value is int number
                && (property.Name.EndsWith("TimeoutMs", StringComparison.Ordinal)
                    || property.Name.EndsWith("IntervalMs", StringComparison.Ordinal)
                    || property.Name.EndsWith("TtlMs", StringComparison.Ordinal))
                && number <= 0)
                throw new InvalidOperationException($"Options.{property.Name} must be greater than zero.");
        }
    }

    private static void ValidateEndpoint(string name, string value)
    {
        if (!name.EndsWith("Endpoint", StringComparison.Ordinal)
            && !name.EndsWith("Url", StringComparison.Ordinal)
            && !name.EndsWith("BaseUrl", StringComparison.Ordinal))
            return;

        var separator = value.LastIndexOf(':');
        var isHostPort = separator > 0
                         && separator + 1 < value.Length
                         && int.TryParse(value[(separator + 1)..], out var port)
                         && port is > 0 and <= 65535;
        if (!Uri.TryCreate(value, UriKind.Absolute, out _) && !isHostPort)
            throw new InvalidOperationException($"Options.{name} is not an absolute endpoint: {value}");
    }
}
