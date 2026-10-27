// The AAR tech.soit.lychee:lychee: the core as liblychee.so (also a prefab
// module, for native callers of the C ABI) with its FFmpeg, and the Kotlin
// LycheePlayer, LycheeFrameDriver and LycheePlayerView over it.
//
//   ./gradlew :lychee:publish   # into build/repo, where the Flutter plugin
//                               # looks for it

plugins {
    id("com.android.library")
    `maven-publish`
}

val repoRoot = rootDir.resolve("../..")
// The same version as the Flutter plugin and the Apple frameworks.
val lycheeVersion = Regex("""(?m)^version: (\S+)""")
    .find(repoRoot.resolve("flutter/pubspec.yaml").readText())!!.groupValues[1]

// -Plychee.ffmpeg=false builds the core without FFmpeg: MP4 files only,
// decoded by MediaCodec (roadmap, FFmpeg-free builds). Releases keep it.
val withFfmpeg = (findProperty("lychee.ffmpeg") as String?)?.toBoolean() ?: true

// FFmpeg comes from tool/build_ffmpeg.sh android-ABI; only the ABIs it was
// built for are built.
val ffmpegAbis = listOf("arm64-v8a", "x86_64")
    .filter { !withFfmpeg || repoRoot.resolve("third_party/ffmpeg/android-$it").isDirectory }
if (ffmpegAbis.isEmpty()) {
    logger.warn("lychee: no Android FFmpeg in third_party/ffmpeg; run tool/build_ffmpeg.sh android-arm64-v8a")
}

android {
    namespace = "tech.soit.lychee"
    compileSdk = 36

    defaultConfig {
        // AAudio.
        minSdk = 26
        ndk {
            abiFilters += ffmpegAbis
        }
        externalNativeBuild {
            cmake {
                arguments += "-DANDROID_STL=c++_shared"
                // Always explicit: CMake's cache would keep the last value.
                arguments += "-DLYCHEE_WITH_FFMPEG=${if (withFfmpeg) "ON" else "OFF"}"
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

    buildFeatures {
        prefabPublishing = true
    }

    prefab {
        create("lychee") {
            headers = repoRoot.resolve("core/include").path
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    publishing {
        singleVariant("release")
    }
}

// The notices for what the AAR carries (FFmpeg, Mbed TLS, code from
// Chromium), as a Java resource, so that they end up in every APK as
// META-INF/lychee/THIRD_PARTY_NOTICES.
abstract class CopyNotices : DefaultTask() {
    @get:InputFile
    abstract val notices: RegularFileProperty

    @get:OutputDirectory
    abstract val output: DirectoryProperty

    @TaskAction
    fun copy() {
        val dir = output.get().asFile.resolve("META-INF/lychee")
        dir.mkdirs()
        notices.get().asFile.copyTo(dir.resolve("THIRD_PARTY_NOTICES"), overwrite = true)
    }
}

val copyNotices = tasks.register<CopyNotices>("copyNotices") {
    notices.set(repoRoot.resolve("THIRD_PARTY_NOTICES"))
}

androidComponents {
    onVariants { variant ->
        variant.sources.resources?.addGeneratedSourceDirectory(copyNotices, CopyNotices::output)
    }
}

// The release ships the AAR and its POM (tool/release.sh); without Gradle
// module metadata, the POM alone describes the dependencies.
tasks.withType<GenerateModuleMetadata>().configureEach {
    enabled = false
}

publishing {
    publications {
        register<MavenPublication>("release") {
            groupId = "tech.soit.lychee"
            artifactId = "lychee"
            version = lycheeVersion
            afterEvaluate {
                from(components["release"])
            }
        }
    }
    repositories {
        maven {
            name = "local"
            url = uri(rootProject.layout.buildDirectory.dir("repo"))
        }
    }
}
