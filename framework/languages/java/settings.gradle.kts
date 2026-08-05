pluginManagement {
    repositories {
        gradlePluginPortal()
        mavenCentral()
    }
}

apply(from = generateSequence(settingsDir) { it.parentFile }
    .first { it.resolve("gradle/zlink-local-packages.settings.gradle.kts").isFile }
    .resolve("gradle/zlink-local-packages.settings.gradle.kts"))

rootProject.name = "zlink-framework-java"

if (gradle.parent == null) {
    includeBuild("samples") {
        name = "zlink-framework-java-samples"
    }
}

include(
    "zlink-framework-provider-abstractions",
    "zlink-framework-binding-internal",
    "zlink-framework-core",
    "zlink-framework-codec-protobuf",
    "zlink-framework-codec-msgpack",
    "zlink-framework-locations-redis",
    "zlink-http-client",
    "zlink-framework-spring-boot-starter",
    "zlink-stream-connector",
    "zlink-framework-kotlin",
    "zlink-http-client-kotlin",
    "zlink-framework-testkit",
    //  가이드 교육용 예제. 배포 대상이 아니고 컴파일만 한다.
    "zlink-framework-doc-examples",
)
