plugins {
    `java-library`
}

description =
    "가이드에 싣는 교육용 예제. 문서가 인용하는 코드를 실제 컴파일 대상으로 둔다."

dependencies {
    implementation(project(":zlink-framework-core"))
}

//  이 모듈은 배포하지 않는다. 존재 이유는 컴파일뿐이다 — 가이드가 인용하는
//  코드가 계약과 어긋나면 여기서 빌드가 깨진다.
tasks.withType<JavaCompile>().configureEach {
    options.compilerArgs.add("-Xlint:all")
}
