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

tasks.withType<Jar>().configureEach {
    from(layout.projectDirectory.file("LICENSE")) {
        into("META-INF")
    }
}

dependencies {
    implementation(zlinkLibs.zlink.bindings)
    api("com.fasterxml.jackson.core:jackson-databind:2.17.2")
    api("com.fasterxml.jackson.datatype:jackson-datatype-jsr310:2.17.2")
    // The typed JSON profile reads @Nullable by reflection, which drops annotations whose class
    // is absent at runtime. Every user of this profile, including a stream connector client
    // without the framework core, gets JSpecify on both classpaths (spec 05 5).
    api("org.jspecify:jspecify:1.0.0")
}
