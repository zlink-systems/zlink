plugins {
    java
}

val generatedNativeResourcesDir = layout.buildDirectory.dir(
    "generated/zlink-native-resources/main",
)

repositories {
    mavenCentral()
}

java {
    toolchain {
        languageVersion = JavaLanguageVersion.of(libs.versions.java.get().toInt())
    }
}

sourceSets {
    named("main") {
        java.setSrcDirs(listOf("../src/main/java"))
        resources.setSrcDirs(listOf("../src/main/resources"))
        resources.srcDir(generatedNativeResourcesDir)
    }
}

dependencies {
    compileOnly(libs.netty.buffer)
    runtimeOnly(libs.netty.buffer)
}

tasks.named<ProcessResources>("processResources") {
    duplicatesStrategy = DuplicatesStrategy.EXCLUDE
}
