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
            url: "https://github.com/Pluet-LLC/pluet-mpv/releases/download/binaries-bd4896eccce8/Libass-4257f8256750.xcframework.zip",
            checksum: "071028a36a7d83c98aede0101698cbecddf453ecc8e24599752289cc66c1a225"
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
            url: "https://github.com/Pluet-LLC/pluet-mpv/releases/download/binaries-bd4896eccce8/Libplacebo-7.387.0-50fcab9a44fe.xcframework.zip",
            checksum: "50fcab9a44fed213379a06bc92fe647afd39a37f6924e140fc6764f8115f177b"
        ),

        .binaryTarget(
            name: "Libdav1d",
            url: "https://github.com/corruptmods/mpv-build/releases/download/binaries-apple/Libdav1d-1.5.3-neon.xcframework.zip",
            checksum: "7965ecf274af5448fa830bc1fec4e78257cf1d7509ed1cd95e32023b7bdff965"
        ),

        .binaryTarget(
            name: "Libavcodec",
            url: "https://github.com/Pluet-LLC/pluet-mpv/releases/download/binaries-bd4896eccce8/Libavcodec-e4950928970f.xcframework.zip",
            checksum: "55135556e7074955e58f147687e25e4e585c934166eab98f437fbf74457a8b43"
        ),
        .binaryTarget(
            name: "Libavdevice",
            url: "https://github.com/Pluet-LLC/pluet-mpv/releases/download/binaries-bd4896eccce8/Libavdevice-e4950928970f.xcframework.zip",
            checksum: "37e6987d77b8ebd64d32dbda83ebae9ccd2386c09b1410727427176def67647c"
        ),
        .binaryTarget(
            name: "Libavformat",
            url: "https://github.com/Pluet-LLC/pluet-mpv/releases/download/binaries-bd4896eccce8/Libavformat-e4950928970f.xcframework.zip",
            checksum: "b7af14f9d1f680415669bdfae3f74be36fa9ecba414664dd2fffa8f44270058a"
        ),
        .binaryTarget(
            name: "Libavfilter",
            url: "https://github.com/Pluet-LLC/pluet-mpv/releases/download/binaries-bd4896eccce8/Libavfilter-e4950928970f.xcframework.zip",
            checksum: "6c787a817e0530af5d1ae82253fe36bbfdf596808111f1cc57c8d3e98dd14399"
        ),
        .binaryTarget(
            name: "Libavutil",
            url: "https://github.com/Pluet-LLC/pluet-mpv/releases/download/binaries-bd4896eccce8/Libavutil-e4950928970f.xcframework.zip",
            checksum: "a7e12a7ec596c2a3cd6534f017c9ed48b505c602bfb4abbd47af0f42e3e3426f"
        ),
        .binaryTarget(
            name: "Libswresample",
            url: "https://github.com/Pluet-LLC/pluet-mpv/releases/download/binaries-bd4896eccce8/Libswresample-e4950928970f.xcframework.zip",
            checksum: "cab2482b0982de45e6b4a06e198346fcb2b10390bd6852d99149a8e4a3a8d133"
        ),
        .binaryTarget(
            name: "Libswscale",
            url: "https://github.com/Pluet-LLC/pluet-mpv/releases/download/binaries-bd4896eccce8/Libswscale-e4950928970f.xcframework.zip",
            checksum: "d7ba3e7a4e427dfa97ffd2fb8e7ad56bab0cf7b8a8f85e3171eb6692af4706a7"
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
            url: "https://github.com/Pluet-LLC/pluet-mpv/releases/download/binaries-bd4896eccce8/Libmpv-275f4806b3eb.xcframework.zip",
            checksum: "a5562dc792f9c3b774c8ec8a3b1073ab61a156cedcff5ead8e9cb04042b368f3"
        ),
        //AUTO_GENERATE_TARGETS_END//
    ]
)
