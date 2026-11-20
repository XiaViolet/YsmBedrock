#include "ModelAudio.h"
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <memory>
#include <cstring>
#include <opus/opusfile.h>

namespace ysm {
std::optional<GameSound> resolveGameSound(std::string_view selector) {
    auto colon = selector.find(':');
    if (colon == std::string_view::npos) return {};
    auto space = selector.substr(0, colon);
    auto name = selector.substr(colon + 1);
    auto valid = [](std::string_view value, bool path) {
        return std::all_of(value.begin(), value.end(), [path](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
                || c == '_' || c == '-' || c == '.' || (path && c == '/');
        });
    };
    if (name.empty() || !valid(space, false) || !valid(name, true)) return {};
    if (!space.empty() && space != "minecraft") return GameSound{std::string(selector)};
    struct Mapping { std::string_view java, bedrock; float pitch; };
    static constexpr Mapping mappings[]{
#include "JavaSoundMappings.inc"
    };
    auto found = std::lower_bound(std::begin(mappings), std::end(mappings), name,
        [](auto const& entry, auto const& key) { return entry.java < key; });
    if (found != std::end(mappings) && found->java == name)
        return GameSound{std::string(found->bedrock), found->pitch};
    return GameSound{std::string(name)};
}
namespace {
using Bytes = std::span<unsigned char const>;
bool starts(Bytes bytes, std::string_view prefix) {
    return bytes.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), bytes.begin());
}
void requireAudio(bool ok, char const* message) {
    if (!ok) throw std::runtime_error(message);
}
std::uint64_t little(Bytes bytes, std::size_t offset, unsigned count) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < count; ++i) value |= std::uint64_t(bytes[offset+i]) << (i*8);
    return value;
}
std::uint32_t checksum(Bytes page) {
    static constexpr auto table = [] {
        std::array<std::uint32_t, 256> values{};
        for (std::uint32_t i = 0; i < values.size(); ++i) {
            auto v = i << 24;
            for (int bit = 0; bit < 8; ++bit) v = (v << 1) ^ ((v >> 31) ? 0x04c11db7u : 0u);
            values[i] = v;
        }
        return values;
    }();
    std::uint32_t crc = 0;
    for (std::size_t i = 0; i < page.size(); ++i) {
        auto byte = (i >= 22 && i < 26) ? 0 : page[i];
        crc = (crc << 8) ^ table[(crc >> 24) ^ byte];
    }
    return crc;
}
unsigned opusFrames(Bytes packet) {
    requireAudio(!packet.empty(), "Empty Opus packet");
    auto config = packet[0] >> 3;
    constexpr unsigned silk[]{480,960,1920,2880}, celt[]{120,240,480,960};
    auto samples = config < 12 ? silk[config & 3] : config < 16 ? 480u << (config & 1) : celt[config & 3];
    auto code = packet[0] & 3;
    requireAudio(code != 3 || packet.size() >= 2, "Truncated Opus frame count");
    unsigned count = code == 0 ? 1 : code == 3 ? packet[1] & 63 : 2;
    requireAudio(count && count*samples <= 5760, "Invalid Opus duration");
    return count*samples;
}
}

std::optional<AudioInfo> inspectAudio(Bytes bytes) {
    if (bytes.size() < 27 || !starts(bytes, "OggS") || bytes[4] || (bytes[5] & ~7)) return {};
    std::size_t firstBody = 27u + bytes[26];
    if (firstBody > bytes.size()) return {};
    AudioInfo info;
    info.opus = starts(bytes.subspan(firstBody), "OpusHead");
    if (!info.opus && !starts(bytes.subspan(firstBody), "\1vorbis")) return {};
    auto serial = little(bytes, 14, 4);
    std::uint32_t sequence = 0;
    std::vector<unsigned char> packet;
    std::size_t packets = 0;
    bool eos = false;
    std::uint64_t decoded = 0;
    std::optional<std::uint64_t> origin, finalGranule;
    while (!bytes.empty()) {
        requireAudio(!eos && bytes.size() >= 27 && starts(bytes, "OggS") && bytes[4] == 0, "Invalid Ogg page");
        auto flags = bytes[5];
        requireAudio(!(flags & ~7) && bool(flags & 2) == (sequence == 0) && bool(flags & 1) == !packet.empty()
            && little(bytes,14,4) == serial && little(bytes,18,4) == sequence, "Invalid Ogg sequence or continuation");
        ++sequence;
        eos = flags & 4;
        auto granule = little(bytes, 6, 8);
        std::size_t body = 27u + bytes[26];
        requireAudio(bytes.size() >= body, "Truncated Ogg segment table");
        auto laces = bytes.subspan(27, bytes[26]);
        auto end = body;
        for (auto size : laces) end += size;
        requireAudio(bytes.size() >= end && checksum(bytes.first(end)) == little(bytes,22,4), "Damaged Ogg page");
        std::uint64_t pageFrames = 0;
        for (auto size : laces) {
            auto segment = bytes.subspan(body, size);
            packet.insert(packet.end(), segment.begin(), segment.end());
            body += size;
            if (size == 255) continue;
            if (!packets) {
                if (info.opus) {
                    requireAudio(packet.size() == 19 && starts(packet, "OpusHead") && packet[8] <= 15 && !packet[18], "Invalid Opus header");
                    info.channels = packet[9];
                    info.rate = 48000;
                    info.preSkip = static_cast<unsigned>(little(packet,10,2));
                    info.gain = static_cast<std::int16_t>(little(packet,16,2));
                } else {
                    requireAudio(packet.size() >= 30 && starts(packet, "\1vorbis") && !little(packet,7,4) && (packet[29] & 1), "Invalid Vorbis header");
                    info.channels = packet[11];
                    info.rate = static_cast<std::uint32_t>(little(packet,12,4));
                }
                requireAudio((info.channels == 1 || info.channels == 2) && info.rate, "Invalid sound channels or sample rate");
            } else if (packets == 1) {
                requireAudio(starts(packet, info.opus ? "OpusTags" : "\3vorbis"), "Invalid sound comment header");
            } else if (info.opus) {
                pageFrames += opusFrames(packet);
            } else if (packets == 2) {
                requireAudio(starts(packet, "\5vorbis"), "Invalid Vorbis setup header");
            }
            ++packets;
            packet.clear();
        }
        requireAudio(!eos || packet.empty(), "Partial packet at end of Ogg stream");
        decoded += pageFrames;
        if (info.opus && packets > 2 && granule != UINT64_MAX) {
            requireAudio(granule <= INT64_MAX, "Invalid Opus granule");
            if (!origin) {
                requireAudio(eos || granule >= decoded, "Invalid Opus timeline origin");
                origin = granule >= decoded ? granule - decoded : 0;
            }
            requireAudio(decoded <= INT64_MAX - *origin, "Opus timeline overflow");
            auto decodedGranule = *origin + decoded;
            requireAudio(granule <= decodedGranule && decodedGranule-granule <= (eos ? pageFrames : 0), "Invalid Opus page duration");
            if (eos) finalGranule = granule;
        } else if (!info.opus && eos && granule <= INT64_MAX) finalGranule = granule;
        bytes = bytes.subspan(end);
    }
    requireAudio(eos && packets >= 3 && finalGranule, "Incomplete Ogg timeline");
    auto start = origin.value_or(0) + info.preSkip;
    requireAudio(*finalGranule >= start, "Opus pre-skip exceeds playable duration");
    info.frames = *finalGranule - start;
    return info;
}
std::vector<unsigned char> decodeOpusWave(ModelAudio const& clip, std::size_t budget) {
    auto const& info = clip.info;
    requireAudio(info.opus && info.channels > 0 && info.channels <= 2, "Invalid Opus decode request");
    requireAudio(budget >= 44 && info.frames <= (std::min<std::uint64_t>(budget, UINT32_MAX) - 44)/(2*info.channels),
        "Decoded sound exceeds model memory budget");
    std::unique_ptr<OggOpusFile, decltype(&op_free)> stream(
        op_open_memory(clip.encoded.data(), clip.encoded.size(), nullptr), op_free);
    requireAudio(bool(stream) && op_channel_count(stream.get(), -1) == static_cast<int>(info.channels)
        && op_pcm_total(stream.get(), -1) == static_cast<ogg_int64_t>(info.frames), "Opus decoder rejected sound timeline");
    std::uint32_t pcmSize = static_cast<std::uint32_t>(info.frames * info.channels * 2);
    std::vector<unsigned char> wave(44 + pcmSize);
    auto write = [&](std::size_t at, std::uint32_t value, unsigned bytes) {
        for (unsigned i = 0; i < bytes; ++i) wave[at+i] = static_cast<unsigned char>(value >> (i*8));
    };
    std::memcpy(wave.data(), "RIFF", 4);
    write(4, pcmSize+36, 4);
    std::memcpy(wave.data()+8, "WAVEfmt ", 8);
    write(16,16,4); write(20,1,2); write(22,info.channels,2); write(24,48000,4);
    write(28,48000*info.channels*2,4); write(32,info.channels*2,2); write(34,16,2);
    std::memcpy(wave.data()+36,"data",4); write(40,pcmSize,4);
    std::array<opus_int16, 11520> samples{};
    std::size_t offset = 44;
    for (;;) {
        auto frames = op_read(stream.get(), samples.data(), static_cast<int>(samples.size()), nullptr);
        requireAudio(frames >= 0, "Opus packet decode failed");
        if (!frames) break;
        auto bytes = static_cast<std::size_t>(frames) * info.channels * 2;
        requireAudio(bytes <= wave.size()-offset, "Opus decoded length exceeds declared duration");
        std::memcpy(wave.data()+offset, samples.data(), bytes);
        offset += bytes;
    }
    requireAudio(offset == wave.size(), "Opus decoded length differs from declared duration");
    return wave;
}
}
