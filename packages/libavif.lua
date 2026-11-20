package("libavif")
    set_homepage("https://github.com/AOMediaCodec/libavif")
    set_description("AVIF container parser and image converter")
    set_license("BSD-2-Clause")
    add_urls("https://codeload.github.com/AOMediaCodec/libavif/tar.gz/refs/tags/v$(version)", {filename = "libavif-1.4.2.tar.gz"})
    add_versions("1.4.2", "2b645287340ba5a631d268b551dc2d72bd73ac33335962dd36dcdb6d8366921d")
    add_deps("cmake")
    add_deps("dav1d 1.5.2", {configs = {shared = false}})
    on_install(function (package)
        -- clang-cl also identifies as Clang, but its COFF librarian accepts
        -- lib.exe arguments rather than the GNU ar script selected upstream.
        io.replace("cmake/Modules/merge_static_libs.cmake",
            'elseif(CMAKE_C_COMPILER_ID MATCHES "^(Clang|GNU|Intel|IntelLLVM)$")',
            'elseif(NOT MSVC AND CMAKE_C_COMPILER_ID MATCHES "^(Clang|GNU|Intel|IntelLLVM)$")',
            {plain = true})
        io.replace("cmake/Modules/merge_static_libs.cmake", "if(CMAKE_LIBTOOL)", "if(CMAKE_AR)", {plain = true})
        io.replace("cmake/Modules/merge_static_libs.cmake",
            "set(BUNDLE_TOOL ${CMAKE_LIBTOOL})", "set(BUNDLE_TOOL ${CMAKE_AR})", {plain = true})
        local configs = {
            "-DCMAKE_BUILD_TYPE=" .. (package:is_debug() and "Debug" or "Release"),
            "-DBUILD_SHARED_LIBS=" .. (package:config("shared") and "ON" or "OFF"),
            "-DAVIF_CODEC_DAV1D=SYSTEM", "-DAVIF_CODEC_AOM=OFF", "-DAVIF_CODEC_LIBGAV1=OFF",
            "-DAVIF_CODEC_RAV1E=OFF", "-DAVIF_CODEC_SVT=OFF", "-DAVIF_CODEC_AVM=OFF",
            -- Use libavif's precise built-in conversion for every supported pixel layout.
            "-DAVIF_LIBYUV=OFF", "-DAVIF_LIBSHARPYUV=OFF", "-DAVIF_LIBXML2=OFF",
            "-DAVIF_ZLIBPNG=OFF", "-DAVIF_JPEG=OFF", "-DAVIF_BUILD_APPS=OFF", "-DAVIF_BUILD_TESTS=OFF"
        }
        import("package.tools.cmake").install(package, configs)
    end)
    on_test(function (package)
        assert(package:has_cfuncs("avifDecoderCreate", {includes = "avif/avif.h"}))
    end)
package_end()
