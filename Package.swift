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
                "Libmpv", "_FFmpeg", "Libuchardet", "Libbluray", "Libdvdread", "Libdvdnav",
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
                "Libdav1d", "Libuavs3d", "Libzvbi"
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
            url: "https://github.com/Pluet-LLC/pluet-mpv/releases/download/binaries-302f8d7a9ba2/Libass-7ea89a5c8d7a.xcframework.zip",
            checksum: "0c55d5c7f31801e5712045c7d0f4b9d126dfe80722c8faad3773ea37d9a18b60"
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
            url: "https://github.com/Pluet-LLC/pluet-mpv/releases/download/binaries-302f8d7a9ba2/Libplacebo-7.387.0-54ded8dcc5f1.xcframework.zip",
            checksum: "54ded8dcc5f1a03ed22add85c6494122ec45281017944c324b92ea30ca50fae6"
        ),

        .binaryTarget(
            name: "Libdav1d",
            url: "https://github.com/corruptmods/mpv-build/releases/download/binaries-apple/Libdav1d-1.5.3-neon.xcframework.zip",
            checksum: "7965ecf274af5448fa830bc1fec4e78257cf1d7509ed1cd95e32023b7bdff965"
        ),

        .binaryTarget(
            name: "Libavcodec",
            url: "https://github.com/Pluet-LLC/pluet-mpv/releases/download/binaries-302f8d7a9ba2/Libavcodec-67fdadcc4556.xcframework.zip",
            checksum: "87fdd318be557e3581786efbeab7f57a9e0d984d08a70f9367d8c2cae421a5ef"
        ),
        .binaryTarget(
            name: "Libavdevice",
            url: "https://github.com/Pluet-LLC/pluet-mpv/releases/download/binaries-302f8d7a9ba2/Libavdevice-67fdadcc4556.xcframework.zip",
            checksum: "526ccd59625e74fd9c7067ad77ae81a4985c332136b6f504574695a2fae9287d"
        ),
        .binaryTarget(
            name: "Libavformat",
            url: "https://github.com/Pluet-LLC/pluet-mpv/releases/download/binaries-302f8d7a9ba2/Libavformat-67fdadcc4556.xcframework.zip",
            checksum: "be9d4f821f9414af2883b7c18ed770a99fe050da028b960e4d74de0b029699e3"
        ),
        .binaryTarget(
            name: "Libavfilter",
            url: "https://github.com/Pluet-LLC/pluet-mpv/releases/download/binaries-302f8d7a9ba2/Libavfilter-67fdadcc4556.xcframework.zip",
            checksum: "a827cd6e049ab8f587c8b7ad7981c3af603bd1f1cf6c529ba1aa653fc7a7968c"
        ),
        .binaryTarget(
            name: "Libavutil",
            url: "https://github.com/Pluet-LLC/pluet-mpv/releases/download/binaries-302f8d7a9ba2/Libavutil-67fdadcc4556.xcframework.zip",
            checksum: "1e515fb9325d0122ad3a684a94148f5b73d798e21bc2c1e08cbdecadf808fe8c"
        ),
        .binaryTarget(
            name: "Libswresample",
            url: "https://github.com/Pluet-LLC/pluet-mpv/releases/download/binaries-302f8d7a9ba2/Libswresample-67fdadcc4556.xcframework.zip",
            checksum: "d193464b406381af4b1474d5464e2caa3e29c8c8bce39f9f8cc0792feb780060"
        ),
        .binaryTarget(
            name: "Libswscale",
            url: "https://github.com/Pluet-LLC/pluet-mpv/releases/download/binaries-302f8d7a9ba2/Libswscale-67fdadcc4556.xcframework.zip",
            checksum: "b1191aec18612b023fa0c11ae5e7b17c0cd9a7370e0673165e08f2601118ae88"
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
            url: "https://github.com/Pluet-LLC/pluet-mpv/releases/download/binaries-302f8d7a9ba2/Libmpv-ed9c3995dde2.xcframework.zip",
            checksum: "2bc9a4cef36b9ab11977ba68206d422d8ce2a3b24ae74710e8c26170cc8daf6e"
        ),

        .binaryTarget(
            name: "Libdvdnav",
            url: "https://github.com/Pluet-LLC/pluet-mpv/releases/download/binaries-302f8d7a9ba2/Libdvdnav-7.0.0-ceb02b475a8a.xcframework.zip",
            checksum: "ceb02b475a8a5c8cdab9d828fe13860a11d8b8043e78784a828fc9e66b25a569"
        ),

        .binaryTarget(
            name: "Libdvdread",
            url: "https://github.com/Pluet-LLC/pluet-mpv/releases/download/binaries-302f8d7a9ba2/Libdvdread-7.1.1-0988217d8638.xcframework.zip",
            checksum: "0988217d86386090cb7e6d1def97a60c01532bf6835b05d8edb4cc8653e137db"
        ),

        .binaryTarget(
            name: "Libzvbi",
            url: "https://github.com/Pluet-LLC/pluet-mpv/releases/download/binaries-302f8d7a9ba2/Libzvbi-0.2.46-8d356d0453a1.xcframework.zip",
            checksum: "8d356d0453a17a449614f02ad1762fd10b211fa445dcc16a6b55fea5c6e11c51"
        ),
        //AUTO_GENERATE_TARGETS_END//
    ]
)
