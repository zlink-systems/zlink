ARG JDK_IMAGE=eclipse-temurin:25.0.2_10-jdk-noble
ARG JRE_IMAGE=eclipse-temurin:25.0.2_10-jre-noble
ARG DOCKER_CLI_IMAGE=docker:28.5.1-cli
FROM ${JDK_IMAGE} AS build
WORKDIR /template
COPY . .
RUN chmod +x gradlew && ./gradlew --no-daemon installDist :server:lobby:test

FROM ${JRE_IMAGE} AS node
WORKDIR /template
COPY --from=build /template/server /template/server
ENTRYPOINT ["java"]

FROM ${DOCKER_CLI_IMAGE} AS docker-cli
FROM node AS runner
COPY --from=docker-cli /usr/local/bin/docker /usr/local/bin/docker
COPY --from=build /template /template
ENV DOCKER_CONFIG=/template/.logs/docker-config
ENTRYPOINT ["java", "--enable-native-access=ALL-UNNAMED", "-Dstdout.encoding=UTF-8", "-Dstderr.encoding=UTF-8", "-cp", "/template/e2e/build/install/e2e/lib/*", "MyGame.e2e.ProgramKt"]
