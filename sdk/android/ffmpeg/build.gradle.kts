import org.jetbrains.dokka.gradle.tasks.DokkaGeneratePublicationTask

plugins {
    id("com.android.library")
    `maven-publish`
    id("org.jetbrains.dokka")
}

val repoRoot = rootDir.resolve("../..")
val lycheeVersion = Regex("""(?m)^version: (\S+)""")
    .find(repoRoot.resolve("flutter/pubspec.yaml").readText())!!.groupValues[1]

android {
    namespace = "tech.soit.lychee.ffmpeg"
    compileSdk = 36
    defaultConfig {
        minSdk = 26
        ndk { abiFilters += listOf("armeabi-v7a", "arm64-v8a", "x86_64") }
        externalNativeBuild {
            cmake {
                arguments += "-DANDROID_STL=c++_shared"
                arguments += "-DLYCHEE_SDK_VERSION=$lycheeVersion"
            }
        }
        consumerProguardFiles("consumer-rules.pro")
    }
    externalNativeBuild {
        cmake {
            path = file("CMakeLists.txt")
            version = "3.22.1"
        }
    }
    // libc++ comes from the base AAR. Packaging it twice is a duplicate JNI
    // library error in consuming apps, even when the bytes are identical.
    packaging { jniLibs.excludes += "**/libc++_shared.so" }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    publishing { singleVariant("release") { withSourcesJar() } }
}

dependencies {
    api(project(":lychee"))
}

// Separate resource path so both AARs' notices survive APK merging.
abstract class CopyFFmpegNotices : DefaultTask() {
    @get:InputFile
    abstract val notices: RegularFileProperty
    @get:OutputDirectory
    abstract val output: DirectoryProperty
    @TaskAction
    fun copy() {
        val dir = output.get().asFile.resolve("META-INF/lychee-ffmpeg")
        dir.mkdirs()
        notices.get().asFile.copyTo(dir.resolve("THIRD_PARTY_NOTICES"), overwrite = true)
    }
}
val copyNotices = tasks.register<CopyFFmpegNotices>("copyNotices") {
    notices.set(repoRoot.resolve("THIRD_PARTY_NOTICES"))
}
androidComponents {
    onVariants { variant ->
        variant.sources.resources?.addGeneratedSourceDirectory(copyNotices, CopyFFmpegNotices::output)
    }
}
tasks.withType<GenerateModuleMetadata>().configureEach { enabled = false }
dokka {
    moduleName.set("Lychee FFmpeg")
    moduleVersion.set(lycheeVersion)
    dokkaPublications.named("html") {
        offlineMode.set(true)
        failOnWarning.set(true)
    }
    dokkaSourceSets.configureEach {
        suppress.set(name != "release")
        if (name != "release") return@configureEach
        sourceRoots.from(file("src/main/kotlin"))
        classpath.from(androidComponents.sdkComponents.bootClasspath)
        jdkVersion.set(17)
        sourceLink {
            localDirectory.set(file("src/main/kotlin"))
            remoteUrl("https://github.com/boyan01/lychee_player/blob/v$lycheeVersion/sdk/android/${project.name}/src/main/kotlin")
            remoteLineSuffix.set("#L")
        }
    }
}
val apiDocumentationJar = tasks.register<Jar>("apiDocumentationJar") {
    archiveClassifier.set("javadoc")
    from(tasks.named<DokkaGeneratePublicationTask>("dokkaGeneratePublicationHtml").flatMap { it.outputDirectory })
    from(repoRoot.resolve("core/include/lychee/lychee.h")) { into("c-api") }
    from(repoRoot.resolve("README.md"))
}

apply(from = rootProject.file("publishing.gradle.kts"))

publishing {
    publications {
        register<MavenPublication>("release") {
            groupId = "io.github.boyan01.lychee"
            artifactId = "lychee-ffmpeg"
            version = lycheeVersion
            afterEvaluate { from(components["release"]) }
            artifact(tasks.named("apiDocumentationJar"))
        }
    }
    repositories {
        maven {
            name = "local"
            url = uri(rootProject.layout.buildDirectory.dir("repo"))
        }
    }
}
