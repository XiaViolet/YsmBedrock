#include "ImageCodec.h"
#include <algorithm>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <jpeglib.h>
#include <webp/decode.h>
#include <webp/demux.h>
#include <avif/avif.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>

namespace ysm {
static std::size_t textureSize(std::uint32_t width, std::uint32_t height, std::size_t budget, std::string const& path) {
    if (!width || !height || width > 4096 || height > 4096)
        throw std::runtime_error("Invalid or oversized texture dimensions: " + path);
    auto size = static_cast<std::size_t>(width) * height * 4;
    if (size > budget) throw std::runtime_error("Model package exceeds decoded texture memory limit");
    return size;
}
struct JpegError {
    jpeg_error_mgr manager{};
    std::jmp_buf jump;
    char message[JMSG_LENGTH_MAX]{};
};
static void jpegError(j_common_ptr source) {
    auto* error = reinterpret_cast<JpegError*>(source->err);
    source->err->format_message(source, error->message);
    std::longjmp(error->jump, 1);
}
static void jpegMessage(j_common_ptr source, int level) {
    // A truncated scan can otherwise be returned as plausible gray pixels.
    if (level < 0) jpegError(source);
}
struct JpegDecoder {
    jpeg_decompress_struct info{};
    JpegError error;
    bool created = false;
    std::vector<unsigned char> rgba;
    std::vector<JSAMPLE> row8;
    std::vector<J12SAMPLE> row12;
    std::vector<J16SAMPLE> row16;
    ~JpegDecoder() { if (created) jpeg_destroy_decompress(&info); }
};
static TextureImage decodeJpeg(std::string path, std::vector<unsigned char> bytes, std::size_t budget) {
    // All mutable state and buffers live on the heap before setjmp. No C++
    // owner is constructed across a libjpeg call that can longjmp on failure.
    auto decoder = std::make_unique<JpegDecoder>();
    decoder->info.err = jpeg_std_error(&decoder->error.manager);
    decoder->error.manager.error_exit = jpegError;
    decoder->error.manager.emit_message = jpegMessage;
    if (setjmp(decoder->error.jump))
        throw std::runtime_error("Cannot decode JPEG texture: " + path + " (" + decoder->error.message + ")");
    decoder->created = true;
    jpeg_create_decompress(&decoder->info);
    jpeg_mem_src(&decoder->info, bytes.data(), static_cast<unsigned long>(bytes.size()));
    if (jpeg_read_header(&decoder->info, TRUE) != JPEG_HEADER_OK)
        throw std::runtime_error("Invalid JPEG texture header: " + path);
    auto& info = decoder->info;
    auto size = textureSize(info.image_width, info.image_height, budget, path);
    if (info.data_precision < 1 || info.data_precision > 16)
        throw std::runtime_error("Unsupported JPEG data precision: " + path);
    bool cmyk = info.jpeg_color_space == JCS_CMYK || info.jpeg_color_space == JCS_YCCK;
    info.out_color_space = cmyk ? JCS_CMYK : JCS_RGB;
    info.mem->max_memory_to_use = 256 * 1024 * 1024;
    if (!jpeg_start_decompress(&info)) throw std::runtime_error("Incomplete JPEG texture: " + path);
    if (info.output_width != info.image_width || info.output_height != info.image_height
        || info.output_components != (cmyk ? 4 : 3))
        throw std::runtime_error("Unexpected JPEG output layout: " + path);
    decoder->rgba.resize(size);
    auto samples = static_cast<std::size_t>(info.output_width) * info.output_components;
    if (info.data_precision <= 8) decoder->row8.resize(samples);
    else if (info.data_precision <= 12) decoder->row12.resize(samples);
    else decoder->row16.resize(samples);
    unsigned maximum = (1U << info.data_precision) - 1;
    while (info.output_scanline < info.output_height) {
        auto offset = static_cast<std::size_t>(info.output_scanline) * info.output_width * 4;
        unsigned read = 0;
        if (info.data_precision <= 8) {
            JSAMPROW row = decoder->row8.data();
            read = jpeg_read_scanlines(&info, &row, 1);
        } else if (info.data_precision <= 12) {
            J12SAMPROW row = decoder->row12.data();
            read = jpeg12_read_scanlines(&info, &row, 1);
        } else {
            J16SAMPROW row = decoder->row16.data();
            read = jpeg16_read_scanlines(&info, &row, 1);
        }
        if (read != 1) throw std::runtime_error("Incomplete JPEG texture: " + path);
        for (std::size_t x = 0; x < info.output_width; ++x) {
            auto sample = [&](std::size_t channel) -> unsigned {
                auto index = x * info.output_components + channel;
                if (info.data_precision <= 8) return decoder->row8[index];
                if (info.data_precision <= 12) return decoder->row12[index];
                return decoder->row16[index];
            };
            for (std::size_t channel = 0; channel < 3; ++channel) {
                unsigned value = sample(channel);
                if (cmyk) {
                    auto black = sample(3);
                    value = info.saw_Adobe_marker ? (value * black + maximum / 2) / maximum
                        : ((maximum - value) * (maximum - black) + maximum / 2) / maximum;
                }
                decoder->rgba[offset + x * 4 + channel] = static_cast<unsigned char>((value * 255 + maximum / 2) / maximum);
            }
            decoder->rgba[offset + x * 4 + 3] = 255;
        }
    }
    if (!jpeg_finish_decompress(&info)) throw std::runtime_error("Incomplete JPEG texture: " + path);
    return {std::move(path), info.output_width, info.output_height, std::move(bytes), std::move(decoder->rgba), "JPEG"};
}
static std::uint32_t little32(unsigned char const* value) {
    return static_cast<std::uint32_t>(value[0]) | (static_cast<std::uint32_t>(value[1]) << 8)
        | (static_cast<std::uint32_t>(value[2]) << 16) | (static_cast<std::uint32_t>(value[3]) << 24);
}
static TextureImage decodeWebp(std::string path, std::vector<unsigned char> bytes, std::size_t budget) {
    auto invalid = [&] { throw std::runtime_error("Invalid or incomplete WebP container: " + path); };
    // The decoder also accepts headerless VP8 fragments. Model assets require
    // a complete RIFF file, including metadata chunks after the pixel stream.
    auto riffSize = little32(bytes.data() + 4);
    if (riffSize < 4 || (riffSize & 1) || riffSize > bytes.size() - 8) invalid();
    auto end = static_cast<std::size_t>(riffSize) + 8;
    unsigned images = 0, extended = 0, alpha = 0;
    for (std::size_t offset = 12; offset < end;) {
        if (end - offset < 8) invalid();
        auto const* chunk = bytes.data() + offset;
        auto length = static_cast<std::size_t>(little32(chunk + 4));
        auto available = end - offset - 8;
        if (length > available || ((length & 1) && length == available)) invalid();
        if ((length & 1) && chunk[8 + length] != 0) invalid();
        if (!std::memcmp(chunk, "ANIM", 4) || !std::memcmp(chunk, "ANMF", 4))
            throw std::runtime_error("Animated WebP textures are not supported: " + path);
        if (!std::memcmp(chunk, "VP8 ", 4) || !std::memcmp(chunk, "VP8L", 4)) ++images;
        if (!std::memcmp(chunk, "VP8X", 4) && (++extended > 1 || offset != 12 || length != 10)) invalid();
        if (!std::memcmp(chunk, "ALPH", 4) && ++alpha > 1) invalid();
        offset += 8 + length + (length & 1);
    }
    if (images != 1) invalid();
    WebPBitstreamFeatures features{};
    if (WebPGetFeatures(bytes.data(), end, &features) != VP8_STATUS_OK) invalid();
    if (features.has_animation) throw std::runtime_error("Animated WebP textures are not supported: " + path);
    auto size = textureSize(features.width, features.height, budget, path);
    WebPData data{bytes.data(), end};
    std::unique_ptr<WebPDemuxer, decltype(&WebPDemuxDelete)> container(WebPDemux(&data), WebPDemuxDelete);
    if (!container || WebPDemuxGetI(container.get(), WEBP_FF_FRAME_COUNT) != 1
        || WebPDemuxGetI(container.get(), WEBP_FF_CANVAS_WIDTH) != static_cast<unsigned>(features.width)
        || WebPDemuxGetI(container.get(), WEBP_FF_CANVAS_HEIGHT) != static_cast<unsigned>(features.height)) invalid();
    WebPIterator frame{};
    if (!WebPDemuxGetFrame(container.get(), 1, &frame)) invalid();
    bool complete = frame.complete && frame.x_offset == 0 && frame.y_offset == 0
        && frame.width == features.width && frame.height == features.height;
    WebPDemuxReleaseIterator(&frame);
    if (!complete) invalid();
    std::vector<unsigned char> rgba(size);
    if (!WebPDecodeRGBAInto(bytes.data(), end, rgba.data(), rgba.size(), features.width * 4))
        throw std::runtime_error("Cannot decode WebP texture: " + path);
    return {std::move(path), static_cast<std::uint32_t>(features.width), static_cast<std::uint32_t>(features.height),
        std::move(bytes), std::move(rgba), "WEBP"};
}
static std::uint64_t bigEndian(unsigned char const* value, unsigned count) {
    std::uint64_t result = 0;
    for (unsigned i = 0; i < count; ++i) result = (result << 8) | value[i];
    return result;
}
static void checkAvifContainer(std::vector<unsigned char> const& bytes, std::string const& path) {
    auto invalid = [&] { throw std::runtime_error("Invalid or incomplete AVIF container: " + path); };
    unsigned types = 0;
    // libavif may finish after locating its image items. Check every top-level
    // box so a complete image cannot conceal a truncated trailing box.
    for (std::size_t offset = 0; offset < bytes.size();) {
        auto remaining = bytes.size() - offset;
        if (remaining < 8) invalid();
        auto const* box = bytes.data() + offset;
        auto size = bigEndian(box, 4);
        std::size_t header = 8;
        if (size == 1) {
            if (remaining < 16) invalid();
            header = 16;
            size = bigEndian(box + 8, 8);
        } else if (!size) size = remaining;
        if (size < header || size > remaining) invalid();
        // A missing avis brand must not make a movie track disappear when the
        // decoder is explicitly selecting the primary still item.
        if (!std::memcmp(box + 4, "moov", 4))
            throw std::runtime_error("Animated AVIF textures are not supported: " + path);
        if (!std::memcmp(box + 4, "ftyp", 4)) {
            if (++types != 1 || offset || size < header + 8 || ((size - header) & 3)) invalid();
            for (std::size_t brand = header; brand < size; brand += 4) {
                if (brand == header + 4) continue; // minor version, not a brand
                if (!std::memcmp(box + brand, "avis", 4))
                    throw std::runtime_error("Animated AVIF textures are not supported: " + path);
            }
        }
        offset += static_cast<std::size_t>(size);
    }
    if (types != 1) invalid();
}
static void transformAvif(TextureImage& image, avifImage const& source, std::size_t budget) {
    auto flags = source.transformFlags;
    if (!(flags & (AVIF_TRANSFORM_CLAP | AVIF_TRANSFORM_IROT | AVIF_TRANSFORM_IMIR))) return;
    avifCropRect crop{0, 0, image.width, image.height};
    avifDiagnostics diagnostics{};
    if ((flags & AVIF_TRANSFORM_CLAP)
        && !avifCropRectFromCleanApertureBox(&crop, &source.clap, image.width, image.height, &diagnostics))
        throw std::runtime_error("Invalid AVIF clean aperture: " + image.path);
    unsigned angle = (flags & AVIF_TRANSFORM_IROT) ? source.irot.angle : 0;
    bool mirror = flags & AVIF_TRANSFORM_IMIR;
    if (angle > 3 || (mirror && source.imir.axis > 1)) throw std::runtime_error("Invalid AVIF orientation: " + image.path);
    auto width = angle & 1 ? crop.height : crop.width;
    auto height = angle & 1 ? crop.width : crop.height;
    std::vector<unsigned char> transformed(textureSize(width, height, budget, image.path));
    // MIAF: crop first, then counterclockwise rotation, then mirror. Convert to
    // RGBA before cropping so odd offsets in subsampled chroma remain correct.
    for (std::uint32_t y = 0; y < crop.height; ++y) for (std::uint32_t x = 0; x < crop.width; ++x) {
        auto dx = x, dy = y;
        if (angle == 1) { dx = y; dy = crop.width - 1 - x; }
        else if (angle == 2) { dx = crop.width - 1 - x; dy = crop.height - 1 - y; }
        else if (angle == 3) { dx = crop.height - 1 - y; dy = x; }
        if (mirror) {
            if (source.imir.axis == 0) dy = height - 1 - dy;
            else dx = width - 1 - dx;
        }
        auto from = (static_cast<std::size_t>(y + crop.y) * image.width + x + crop.x) * 4;
        auto to = (static_cast<std::size_t>(dy) * width + dx) * 4;
        std::copy_n(image.rgba.data() + from, 4, transformed.data() + to);
    }
    image.width = width;
    image.height = height;
    image.rgba = std::move(transformed);
}
static TextureImage decodeAvif(std::string path, std::vector<unsigned char> bytes, std::size_t budget) {
    checkAvifContainer(bytes, path);
    std::unique_ptr<avifDecoder, decltype(&avifDecoderDestroy)> decoder(avifDecoderCreate(), avifDecoderDestroy);
    if (!decoder) throw std::runtime_error("Cannot allocate AVIF decoder: " + path);
    auto check = [&](avifResult result) {
        if (result != AVIF_RESULT_OK)
            throw std::runtime_error("Cannot decode AVIF texture: " + path + " (" + avifResultToString(result) + ")");
    };
    decoder->imageDimensionLimit = 4096;
    decoder->imageSizeLimit = static_cast<std::uint32_t>(std::min<std::size_t>(4096 * 4096, budget / 4));
    if (!decoder->imageSizeLimit) throw std::runtime_error("Model package exceeds decoded texture memory limit");
    decoder->imageCountLimit = 1;
    decoder->allowIncremental = AVIF_FALSE;
    decoder->allowProgressive = AVIF_FALSE; // decode the final quality of a progressive still
    decoder->imageContentToDecode = AVIF_IMAGE_CONTENT_COLOR_AND_ALPHA | AVIF_IMAGE_CONTENT_SAMPLE_TRANSFORMS;
    check(avifDecoderSetSource(decoder.get(), AVIF_DECODER_SOURCE_PRIMARY_ITEM));
    check(avifDecoderSetIOMemory(decoder.get(), bytes.data(), bytes.size()));
    check(avifDecoderParse(decoder.get()));
    if (decoder->imageCount != 1 || decoder->imageSequenceTrackPresent)
        throw std::runtime_error("Animated AVIF textures are not supported: " + path);
    auto width = decoder->image->width, height = decoder->image->height;
    auto size = textureSize(width, height, budget, path);
    check(avifDecoderNextImage(decoder.get()));
    if (decoder->image->width != width || decoder->image->height != height)
        throw std::runtime_error("Decoded AVIF dimensions differ from header: " + path);
    std::vector<unsigned char> rgba(size);
    avifRGBImage output{};
    avifRGBImageSetDefaults(&output, decoder->image);
    output.depth = 8;
    output.format = AVIF_RGB_FORMAT_RGBA;
    output.alphaPremultiplied = AVIF_FALSE;
    output.pixels = rgba.data();
    output.rowBytes = width * 4;
    check(avifImageYUVToRGB(decoder->image, &output));
    TextureImage image{std::move(path), width, height, std::move(bytes), std::move(rgba), "AVIF"};
    transformAvif(image, *decoder->image, budget);
    return image;
}
TextureImage decodeTexture(std::string path, std::vector<unsigned char> bytes, std::size_t decodedBudget) {
    static constexpr unsigned char Signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    bool png = bytes.size() >= 24 && std::equal(std::begin(Signature), std::end(Signature), bytes.begin());
    bool jpeg = bytes.size() >= 3 && bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff;
    bool webp = bytes.size() >= 12 && !std::memcmp(bytes.data(), "RIFF", 4) && !std::memcmp(bytes.data() + 8, "WEBP", 4);
    avifROData input{bytes.data(), bytes.size()};
    bool avif = avifPeekCompatibleFileType(&input);
    if (!png && !jpeg && !webp && !avif) throw std::runtime_error("Unsupported or invalid texture format (PNG/JPEG/WEBP/AVIF): " + path);
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("Encoded texture too large: " + path);
    if (jpeg) return decodeJpeg(std::move(path), std::move(bytes), decodedBudget);
    if (webp) return decodeWebp(std::move(path), std::move(bytes), decodedBudget);
    if (avif) return decodeAvif(std::move(path), std::move(bytes), decodedBudget);
    int width = 0, height = 0, channels = 0;
    if (!stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels))
        throw std::runtime_error("Invalid PNG texture header: " + path);
    auto size = textureSize(width, height, decodedBudget, path);
    auto expectedWidth = width, expectedHeight = height;
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
        stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels, 4), stbi_image_free);
    if (!pixels) throw std::runtime_error("Cannot decode PNG texture: " + path);
    if (width != expectedWidth || height != expectedHeight)
        throw std::runtime_error("Decoded texture dimensions differ from header: " + path);
    return {std::move(path), static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
        std::move(bytes), std::vector<unsigned char>(pixels.get(), pixels.get() + size), "PNG"};
}
}
