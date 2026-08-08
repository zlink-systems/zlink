plugins {
    `java-library`
    `maven-publish`
}

description = "Internal framework-json-v1 mapper profile shared by JVM packages"

java {
    modularity.inferModulePath.set(true)
}

tasks.withType<JavaCompile>().configureEach {
    options.compilerArgs.add("-Xlint:-module")
}

dependencies {
    api("com.fasterxml.jackson.core:jackson-databind:2.17.2")
    api("com.fasterxml.jackson.datatype:jackson-datatype-jsr310:2.17.2")
}
