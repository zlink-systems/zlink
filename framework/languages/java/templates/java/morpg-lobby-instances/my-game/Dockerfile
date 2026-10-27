ARG JDK_IMAGE=eclipse-temurin:25.0.2_10-jdk-noble
ARG JRE_IMAGE=eclipse-temurin:25.0.2_10-jre-noble
ARG DOCKER_CLI_IMAGE=docker:28.5.1-cli
FROM ${JDK_IMAGE} AS build
WORKDIR /src
COPY . .
RUN chmod +x gradlew && ./gradlew --no-daemon installDist :server:lobby:test \
    && mkdir /out && for module in server/*; do \
         if [ -d "$module/build/install" ]; then cp -r "$module/build/install/"* /out/; fi; \
       done && cp -r e2e/build/install/e2e /out/e2e

FROM ${JRE_IMAGE} AS node
WORKDIR /app
COPY --from=build /out /app
ENTRYPOINT ["java", "--enable-native-access=ALL-UNNAMED"]

FROM ${DOCKER_CLI_IMAGE} AS docker-cli
FROM node AS runner
COPY --from=docker-cli /usr/local/bin/docker /usr/local/bin/docker
WORKDIR /template
COPY . .
ENTRYPOINT ["java", "--enable-native-access=ALL-UNNAMED", "-Dstdout.encoding=UTF-8", "-Dstderr.encoding=UTF-8", "-cp", "/app/e2e/lib/*", "MyGame.e2e.Program"]
