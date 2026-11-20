-- The registry currently pins older releases; keep this codec source reproducible.
package("libwebp")
    set_homepage("https://chromium.googlesource.com/webm/libwebp/")
    set_description("WebP image decoder and container parser")
    set_license("BSD-3-Clause")
    add_urls("https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-$(version).tar.gz")
    add_versions("1.6.0", "e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564")
    add_deps("cmake")
    on_load(function (package)
        local prefix = package:is_plat("windows") and "lib" or ""
        package:add("links", prefix .. "webpdemux", prefix .. "webpdecoder")
    end)
    on_install(function (package)
        local configs = {
            "-DCMAKE_BUILD_TYPE=" .. (package:is_debug() and "Debug" or "Release"),
            "-DBUILD_SHARED_LIBS=" .. (package:config("shared") and "ON" or "OFF"),
            "-DWEBP_BUILD_ANIM_UTILS=OFF", "-DWEBP_BUILD_CWEBP=OFF", "-DWEBP_BUILD_DWEBP=OFF",
            "-DWEBP_BUILD_GIF2WEBP=OFF", "-DWEBP_BUILD_IMG2WEBP=OFF", "-DWEBP_BUILD_VWEBP=OFF",
            "-DWEBP_BUILD_WEBPINFO=OFF", "-DWEBP_BUILD_WEBPMUX=OFF", "-DWEBP_BUILD_LIBWEBPMUX=OFF",
            "-DWEBP_BUILD_EXTRAS=OFF", "-DWEBP_BUILD_FUZZTEST=OFF", "-DWEBP_USE_THREAD=OFF"
        }
        import("package.tools.cmake").install(package, configs)
    end)
    on_test(function (package)
        assert(package:has_cfuncs("WebPGetDecoderVersion", {includes = "webp/decode.h"}))
        assert(package:has_cfuncs("WebPGetDemuxVersion", {includes = "webp/demux.h"}))
    end)
package_end()
