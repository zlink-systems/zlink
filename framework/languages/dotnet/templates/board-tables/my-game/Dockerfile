ARG SDK_IMAGE=mcr.microsoft.com/dotnet/sdk:8.0.415-bookworm-slim
ARG RUNTIME_IMAGE=mcr.microsoft.com/dotnet/aspnet:8.0.21-bookworm-slim
ARG DOCKER_CLI_IMAGE=docker:28.5.1-cli
FROM ${SDK_IMAGE} AS build
WORKDIR /src
COPY Directory.Build.props Directory.Packages.props nuget.config ./
COPY server/MyGame.sln server/MyGame.sln
COPY server/MyGame.Session/MyGame.Session.csproj server/MyGame.Session/
COPY server/MyGame.Shared/MyGame.Shared.csproj server/MyGame.Shared/
COPY server/MyGame.Table/MyGame.Table.csproj server/MyGame.Table/
COPY e2e/MyGame.E2E/MyGame.E2E.csproj e2e/MyGame.E2E/
RUN dotnet restore server/MyGame.sln
COPY server server
COPY e2e e2e
RUN dotnet publish server/MyGame.Session/MyGame.Session.csproj --no-restore -c Release -o /out/session \
    && dotnet publish server/MyGame.Table/MyGame.Table.csproj --no-restore -c Release -o /out/table \
    && dotnet publish e2e/MyGame.E2E/MyGame.E2E.csproj --no-restore -c Release -o /out/e2e

FROM ${RUNTIME_IMAGE} AS node
WORKDIR /app
COPY --from=build /out /app
ENTRYPOINT ["dotnet"]

FROM ${DOCKER_CLI_IMAGE} AS docker-cli
FROM node AS runner
COPY --from=docker-cli /usr/local/bin/docker /usr/local/bin/docker
WORKDIR /template
COPY Directory.Packages.props zlink.structure.json docker-compose.yml ./
COPY server/config ./server/config
ENTRYPOINT ["dotnet", "/app/e2e/MyGame.E2E.dll", "--root", "/template"]
