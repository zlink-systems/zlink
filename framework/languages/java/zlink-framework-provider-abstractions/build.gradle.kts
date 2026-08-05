plugins {
    `java-library`
    `maven-publish`
}

description = "Minimal provider contracts for ZLink framework stores"

java {
    modularity.inferModulePath.set(true)
}

val consumerTest by sourceSets.creating {
    java.srcDir("src/consumerTest/java")
    compileClasspath += sourceSets.main.get().output
}

configurations.named(consumerTest.implementationConfigurationName) {
    extendsFrom(configurations.implementation.get())
}

tasks.register("consumerTest") {
    description =
        "Compiles an external provider against only the provider abstraction artifact."
    group = LifecycleBasePlugin.VERIFICATION_GROUP
    dependsOn(tasks.named(consumerTest.compileJavaTaskName))
}

tasks.named("check") {
    dependsOn("consumerTest")
}
