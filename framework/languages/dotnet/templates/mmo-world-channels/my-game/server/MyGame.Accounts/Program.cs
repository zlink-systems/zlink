using Microsoft.AspNetCore.Hosting.Server;
using Microsoft.AspNetCore.Hosting.Server.Features;
using MyGame.Shared;

var settings = NodeSettings.Load(args);
var builder = WebApplication.CreateBuilder();
GameHosting.ConfigureAccounts(builder);
builder.WebHost.UseUrls(settings.HttpEndpoint ?? throw new InvalidDataException("HttpEndpoint is required."));
await using var app = builder.Build();
app.MapGet("/worlds", () => settings.Worlds ?? []);
await NodeControl.RunAccountsAsync(app, () => new NodeReady(settings.Id, "",
    HttpEndpoint: app.Services.GetRequiredService<IServer>().Features.Get<IServerAddressesFeature>()!
        .Addresses.Single()));
