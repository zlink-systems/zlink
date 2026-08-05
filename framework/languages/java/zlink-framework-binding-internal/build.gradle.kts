plugins {
    `java-library`
    `maven-publish`
}

description = "ZLink Framework JVM raw binding implementation"

java {
    modularity.inferModulePath.set(true)
}

tasks.withType<JavaCompile>().configureEach {
    // Friend modules are reverse dependencies and are absent while this
    // implementation module itself compiles.
    options.compilerArgs.add("-Xlint:-module")
}

dependencies {
    implementation(zlinkLibs.zlink.bindings)
}
