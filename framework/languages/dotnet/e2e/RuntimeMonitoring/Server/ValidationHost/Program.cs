using Microsoft.Extensions.Configuration;

using Zlink.Framework.E2E.Configuration;

var validationCase = E2eConfiguration.Load<ValidationOptions>(args).Case;
throw new ArgumentException(
    $"Monitoring event registration is not a public contract: '{validationCase}'.");

internal sealed record ValidationOptions(string Case);
