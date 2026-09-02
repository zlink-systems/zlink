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
        languageVersion = JavaLanguageVersion.of(22)
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
    compileOnly("io.netty:netty-buffer:4.1.100.Final")
    runtimeOnly("io.netty:netty-buffer:4.1.100.Final")
}

tasks.named<ProcessResources>("processResources") {
    duplicatesStrategy = DuplicatesStrategy.EXCLUDE
}
