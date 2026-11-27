ARG NODE_IMAGE=node:22.20.0-trixie
ARG DOCKER_CLI_IMAGE=docker:28.5.1-cli
FROM ${NODE_IMAGE} AS build
WORKDIR /template
COPY package.json package-lock.json ./
RUN npm ci
COPY tsconfig.json ./
COPY scripts scripts
COPY server server
COPY e2e e2e
RUN npm run build && npm prune --omit=dev

FROM ${NODE_IMAGE}-slim AS node
RUN apt-get update && apt-get install -y --no-install-recommends libssl3t64 && rm -rf /var/lib/apt/lists/*
WORKDIR /template
COPY --from=build /template/node_modules node_modules
COPY --from=build /template/dist dist
COPY package.json ./
ENTRYPOINT ["node"]

FROM ${DOCKER_CLI_IMAGE} AS docker-cli
FROM node AS runner
COPY --from=docker-cli /usr/local/bin/docker /usr/local/bin/docker
COPY zlink.structure.json docker-compose.yml ./
COPY server server
ENTRYPOINT ["node", "dist/e2e/runner.js"]
