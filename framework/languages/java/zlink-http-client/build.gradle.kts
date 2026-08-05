plugins {
    `java-library`
    `maven-publish`
    jacoco
}

description = "ZLink Java HTTP client (fluent wrapper over java.net.http)"

java {
    modularity.inferModulePath.set(true)
}

// Single version source: HttpClientVersion.java (also drives the User-Agent token).
version = Regex("VERSION = \"([^\"]+)\"")
    .find(file("src/main/java/systems/zlink/httpclient/internal/HttpClientVersion.java").readText())!!
    .groupValues[1]

dependencies {
    api(project(":zlink-framework-core"))
    api("com.fasterxml.jackson.core:jackson-databind:2.17.2")
}

tasks.named<Test>("test") {
    finalizedBy(tasks.named("jacocoTestReport"))
}

tasks.named<JacocoReport>("jacocoTestReport") {
    dependsOn(tasks.named("test"))
    reports {
        xml.required.set(true)
        html.required.set(true)
    }
}

tasks.named<JacocoCoverageVerification>("jacocoTestCoverageVerification") {
    dependsOn(tasks.named("test"))
    violationRules {
        rule {
            limit {
                counter = "LINE"
                minimum = "0.80".toBigDecimal()
            }
        }
    }
}

tasks.named("check") {
    dependsOn(tasks.named("jacocoTestCoverageVerification"))
}

// The shared publishing config stamps the publication version before this script runs;
// restate it so the published artifact uses the module version above.
publishing {
    publications.withType<MavenPublication>().configureEach {
        version = project.version.toString()
    }
}
