plugins {
    `java-library`
    `maven-publish`
}

description = "ZLink Java Redis location store extension"

dependencies {
    testImplementation(testFixtures(project(":zlink-framework-core")))
    api(project(":zlink-framework-core"))
    api(project(":zlink-framework-provider-abstractions"))
    api("io.lettuce:lettuce-core:7.6.0.RELEASE")
    implementation("com.fasterxml.jackson.core:jackson-databind:2.17.2")
}

tasks.named<Test>("test") {
    inputs.property("redisEndpoint", System.getenv("ZLINK_REDIS_TEST_ENDPOINT") ?: "")
}
