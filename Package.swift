// swift-tools-version:5.8

import PackageDescription

let package = Package(
    name: "MPVKit",
    platforms: [.macOS(.v12), .iOS(.v15), .tvOS(.v15)],
    products: [
        .library(
            name: "MPVKit",
            targets: ["_MPVKit"]
        ),
    ],
    targets: [
        .target(
            name: "_MPVKit",
            dependencies: [
                "Libmpv", "_FFmpeg", "Libuchardet", "Libbluray",
                .target(name: "Libluajit", condition: .when(platforms: [.macOS])),
            ],
            path: "Sources/_MPVKit",
            // The licence texts of every component the package links (scripts/licenses.py apple). Every app that
            // links MPVKit embeds them in MPVKit__MPVKit.bundle under licenses/<component>/.
            resources: [.copy("licenses")],
            linkerSettings: [
                .linkedFramework("Accelerate"),
                .linkedFramework("AVFoundation"),
                .linkedFramework("CoreImage"),
                .linkedFramework("CoreMedia"),
                .linkedFramework("CoreVideo"),
                .linkedFramework("Metal"),
                .linkedFramework("QuartzCore"),
                .linkedFramework("AppKit", .when(platforms: [.macOS])),
                .linkedFramework("UIKit", .when(platforms: [.iOS, .tvOS])),
                .linkedFramework("CoreAudio"),
            ]
        ),
        .target(
            name: "_FFmpeg",
            dependencies: [
                "Libavcodec", "Libavdevice", "Libavfilter", "Libavformat", "Libavutil", "Libswresample", "Libswscale",
                "Libssl", "Libcrypto", "Libass", "Libfreetype", "Libfribidi", "Libharfbuzz",
                "MoltenVK", "Libshaderc_combined", "lcms2", "Libplacebo", "Libdovi", "Libunibreak",
                "Libdav1d", "Libuavs3d"
            ],
            path: "Sources/_FFmpeg",
            linkerSettings: [
                .linkedFramework("AudioToolbox"),
                .linkedFramework("CoreVideo"),
                .linkedFramework("CoreFoundation"),
                .linkedFramework("CoreMedia"),
                .linkedFramework("CoreText"),
                .linkedFramework("Metal"),
                .linkedFramework("Security"),
                .linkedFramework("VideoToolbox"),
                .linkedLibrary("bz2"),
                .linkedLibrary("iconv"),
                .linkedLibrary("expat"),
                .linkedLibrary("resolv"),
                .linkedLibrary("xml2"),
                .linkedLibrary("z"),
                .linkedLibrary("c++"),
            ]
        ),

        //AUTO_GENERATE_TARGETS_BEGIN//

        .binaryTarget(
            name: "Libunibreak",
            url: "https://github.com/mpvkit/libass-build/releases/download/0.17.4/Libunibreak.xcframework.zip",
            checksum: "001087c0e927ae00f604422b539898b81eb77230ea7700597b70393cd51e946c"
        ),

        .binaryTarget(
            name: "Libfreetype",
            url: "https://github.com/mpvkit/libass-build/releases/download/0.17.4/Libfreetype.xcframework.zip",
            checksum: "f2840aba1ce35e51c0595557eee82c908dac8e32108ecc0661301c06061e051c"
        ),

        .binaryTarget(
            name: "Libfribidi",
            url: "https://github.com/mpvkit/libass-build/releases/download/0.17.4/Libfribidi.xcframework.zip",
            checksum: "4a55513792ef7a17893875f74cc84c56f3657e8768c07a7a96f563a11dc4b743"
        ),

        .binaryTarget(
            name: "Libharfbuzz",
            url: "https://github.com/mpvkit/libass-build/releases/download/0.17.4/Libharfbuzz.xcframework.zip",
            checksum: "91558d8497d9d97bc11eeef8b744d104315893bfee8f17483d8002e14565f84b"
        ),

        .binaryTarget(
            name: "Libass",
            url: "https://github.com/Pluet-LLC/pluet-mpv/releases/download/binaries-12772408717e/Libass-4257f8256750.xcframework.zip",
            checksum: "71fc50e22eab0208725f1bd48b64d71abc948a7e58eab59339f6d29e6864ed1a"
        ),

        .binaryTarget(
            name: "Libbluray",
            url: "https://github.com/mpvkit/libbluray-build/releases/download/1.4.0/Libbluray.xcframework.zip",
            checksum: "bc037d34e2b0b5ab7f202fb371f5fb298136cc66fdf406c2172185d06f53f18d"
        ),

        .binaryTarget(
            name: "Libcrypto",
            url: "https://github.com/mpvkit/openssl-build/releases/download/3.3.5/Libcrypto.xcframework.zip",
            checksum: "593283be2a90f7fd66f6e6ed331b2f099cf403e0926fe3b4ac09a7062b793965"
        ),
        .binaryTarget(
            name: "Libssl",
            url: "https://github.com/mpvkit/openssl-build/releases/download/3.3.5/Libssl.xcframework.zip",
            checksum: "ff5ffd43d015d7285fd37e4a3145b25cbd8d2842740bd629a711c299a20e226a"
        ),

        .binaryTarget(
            name: "Libuavs3d",
            url: "https://github.com/mpvkit/libuavs3d-build/releases/download/1.2.1-fix/Libuavs3d.xcframework.zip",
            checksum: "bd5256081486d16c51c868d755bf70266c424b54c895269580de44ec6707f789"
        ),

        .binaryTarget(
            name: "Libdovi",
            url: "https://github.com/mpvkit/libdovi-build/releases/download/3.3.2/Libdovi.xcframework.zip",
            checksum: "e693e239808350868e79c5448ef9f02e2716bc822dd8632a41a368a1eae5ca7d"
        ),

        .binaryTarget(
            name: "MoltenVK",
            url: "https://github.com/mpvkit/moltenvk-build/releases/download/1.4.1/MoltenVK.xcframework.zip",
            checksum: "9bd1ca1e4563bacd25d6e55d37b10341d50b2601bc2684bc332188e79daa2b79"
        ),

        .binaryTarget(
            name: "Libshaderc_combined",
            url: "https://github.com/mpvkit/libshaderc-build/releases/download/2025.5.0/Libshaderc_combined.xcframework.zip",
            checksum: "758047b615708575b580eb960a2d083f760a29dc462d6eaa360416c946ce433b"
        ),

        .binaryTarget(
            name: "lcms2",
            url: "https://github.com/mpvkit/lcms2-build/releases/download/2.17.0/lcms2.xcframework.zip",
            checksum: "dc0dce0606f6ab6841a8ec5a6bd4448e2f3ef00661a050460f806c9393dc6982"
        ),

        .binaryTarget(
            name: "Libplacebo",
            url: "https://github.com/Pluet-LLC/pluet-mpv/releases/download/binaries-12772408717e/Libplacebo-7.387.0-37a3c8cd7cf1.xcframework.zip",
            checksum: "37a3c8cd7cf130fe48aaf57e83adecf4c414667831c4c6abdf0fc6a4477ec483"
        ),

        .binaryTarget(
            name: "Libdav1d",
            url: "https://github.com/corruptmods/mpv-build/releases/download/binaries-apple/Libdav1d-1.5.3-neon.xcframework.zip",
            checksum: "7965ecf274af5448fa830bc1fec4e78257cf1d7509ed1cd95e32023b7bdff965"
        ),

        .binaryTarget(
            name: "Libavcodec",
            url: "https://github.com/Pluet-LLC/pluet-mpv/releases/download/binaries-12772408717e/Libavcodec-e4950928970f.xcframework.zip",
            checksum: "8f7d67d6b6d6dde78d9e79ba446794a04822f28bc8fd1cd8fb1ab9d08419336d"
        ),
        .binaryTarget(
            name: "Libavdevice",
            url: "https://github.com/Pluet-LLC/pluet-mpv/releases/download/binaries-12772408717e/Libavdevice-e4950928970f.xcframework.zip",
            checksum: "2114d053570e06b06440e86a5f2cfd7ace3bfb4f1f69b3a27d65a1261cfb841d"
        ),
        .binaryTarget(
            name: "Libavformat",
            url: "https://github.com/Pluet-LLC/pluet-mpv/releases/download/binaries-12772408717e/Libavformat-e4950928970f.xcframework.zip",
            checksum: "15adf7a1b9ce6c21c730bb2671dbe2724d4f2be896c5be7555eae8e318306c4e"
        ),
        .binaryTarget(
            name: "Libavfilter",
            url: "https://github.com/Pluet-LLC/pluet-mpv/releases/download/binaries-12772408717e/Libavfilter-e4950928970f.xcframework.zip",
            checksum: "bf5c078c2e39d576e807d6309c82ecf42364dbb1d5ec94a9085d942e84a027bf"
        ),
        .binaryTarget(
            name: "Libavutil",
            url: "https://github.com/Pluet-LLC/pluet-mpv/releases/download/binaries-12772408717e/Libavutil-e4950928970f.xcframework.zip",
            checksum: "8bddf5b0fb7ec12ee608d2d0e52bd55222fca0a42686623be18682dc38745d17"
        ),
        .binaryTarget(
            name: "Libswresample",
            url: "https://github.com/Pluet-LLC/pluet-mpv/releases/download/binaries-12772408717e/Libswresample-e4950928970f.xcframework.zip",
            checksum: "c366431dd8c3679a3268b476e8b54d000da113e3ad4bfa80d95e58beb21594fb"
        ),
        .binaryTarget(
            name: "Libswscale",
            url: "https://github.com/Pluet-LLC/pluet-mpv/releases/download/binaries-12772408717e/Libswscale-e4950928970f.xcframework.zip",
            checksum: "2f5362f7f93212aca2d1cce4e44665b3473478f4c8b04a15b17741b699d2f3ce"
        ),

        .binaryTarget(
            name: "Libuchardet",
            url: "https://github.com/mpvkit/libuchardet-build/releases/download/0.0.8-xcode/Libuchardet.xcframework.zip",
            checksum: "503202caa0dafb6996b2443f53408a713b49f6c2d4a26d7856fd6143513a50d7"
        ),

        .binaryTarget(
            name: "Libluajit",
            url: "https://github.com/mpvkit/libluajit-build/releases/download/2.1.0-xcode/Libluajit.xcframework.zip",
            checksum: "8e76f267ee100ff5f3bbde7641b2240566df722241cdf8e135be7ef3d29e237a"
        ),

        .binaryTarget(
            name: "Libmpv",
            url: "https://github.com/Pluet-LLC/pluet-mpv/releases/download/binaries-12772408717e/Libmpv-275f4806b3eb.xcframework.zip",
            checksum: "0c52f187c2bb0d2edec745d7ce8f0ac6f48820fa3b133681758dbcbe68f5680b"
        ),
        //AUTO_GENERATE_TARGETS_END//
    ]
)
