plugins {
    `java-library`
    id("org.jetbrains.kotlin.jvm")
}

dependencies {
    api(zlinkLibs.zlink.framework.core)
    implementation(kotlin("stdlib"))
}
