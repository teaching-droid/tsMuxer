// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#include "wavpackFile.h"

#include <fs/discImage.h>
#include <fs/systemlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <vector>

#include "memoryMatroska.h"
#include "vodCoreException.h"
#include "vod_common.h"

namespace
{
using namespace memoryMatroska;

// tsMuxeR reads the whole file into memory, as an Ogg file
#if INTPTR_MAX > INT32_MAX
constexpr int64_t MAX_WAVPACK_BYTES = INT64_C(1) << 30;
#else
constexpr int64_t MAX_WAVPACK_BYTES = INT64_C(256) << 20;
#endif

// the block header (WavPack5FileFormat 2.0): 32 bytes, little endian
constexpr int HEADER = 32;
constexpr uint32_t MONO_FLAG = 4;
constexpr uint32_t HYBRID_FLAG = 8;
constexpr uint32_t FLOAT_DATA = 0x80;
constexpr uint32_t INITIAL_BLOCK = 0x800;
constexpr uint32_t FINAL_BLOCK = 0x1000;
constexpr uint32_t DSD_FLAG = 0x80000000U;
constexpr int ID_SAMPLE_RATE = 0x27;
constexpr int SAMPLE_RATES[] = {6000,  8000,  9600,  11025, 12000, 16000, 22050, 24000,
                                32000, 44100, 48000, 64000, 88200, 96000, 192000};

std::mutex preparedLock;
std::set<std::string>& prepared()
{
    static std::set<std::string> names;
    return names;
}

uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }

void putLe32(std::vector<uint8_t>& out, const uint32_t v)
{
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

// The sample rate a block gives in its metadata sub-blocks (ID_SAMPLE_RATE), for a rate its flags have no code for
int sampleRateOf(const uint8_t* data, const size_t size)
{
    for (size_t at = 0; at + 2 <= size;)
    {
        const int id = data[at];
        size_t words;
        size_t head = 2;
        if (id & 0x80)  // ID_LARGE: a 3 byte size
        {
            if (at + 4 > size)
                break;
            words = data[at + 1] | data[at + 2] << 8 | static_cast<size_t>(data[at + 3]) << 16;
            head = 4;
        }
        else
            words = data[at + 1];
        size_t bytes = words * 2;
        if ((id & 0x3F) == ID_SAMPLE_RATE && bytes >= 3 && at + head + 3 <= size)
            return data[at + head] | data[at + head + 1] << 8 | data[at + head + 2] << 16;
        at += head + bytes;  // the size of an odd one (ID_ODD_SIZE) is padded to whole words
    }
    return 0;
}

// The data of a block without its checksum (ID_BLOCK_CHECKSUM, WavPack 5): it covers the whole block with its header,
// which Matroska keeps only in part and a demux makes anew, so it would no longer match. It is left out, and the flag
// that says it is there (bit 28) cleared, as other muxers store WavPack in Matroska.
constexpr uint32_t HAS_CHECKSUM = 0x10000000;
constexpr int ID_BLOCK_CHECKSUM = 0x2F;

std::vector<uint8_t> withoutChecksum(const uint8_t* data, const size_t size)
{
    std::vector<uint8_t> out;
    out.reserve(size);
    for (size_t at = 0; at + 2 <= size;)
    {
        const int id = data[at];
        size_t head = 2;
        size_t words = data[at + 1];
        if (id & 0x80)  // ID_LARGE: a 3 byte size
        {
            if (at + 4 > size)
                break;
            words = data[at + 1] | data[at + 2] << 8 | static_cast<size_t>(data[at + 3]) << 16;
            head = 4;
        }
        const size_t end = (std::min)(size, at + head + words * 2);
        if ((id & 0x3F) != ID_BLOCK_CHECKSUM)
            out.insert(out.end(), data + at, data + end);
        at = end;
    }
    return out;
}

struct Frame
{
    int64_t index;  // of its first sample
    uint32_t samples;
    std::vector<uint8_t> bytes;       // as a Matroska block holds it
    std::vector<uint8_t> correction;  // the frame of the correction file, as BlockAdditional 1 holds it
    int blocks = 0;
    std::vector<uint32_t> flags;  // of each block, without HAS_CHECKSUM: its correction blocks must have the same
};

// The blocks of the correction file (.wvc) of a hybrid file, by block_index: for each, the crc and the data of every
// block with audio of the frame, in the order of the file
struct CorrectionBlock
{
    uint32_t samples;
    uint32_t flags;  // without HAS_CHECKSUM
    uint32_t crc;
    std::vector<uint8_t> data;
};

std::vector<std::pair<int64_t, std::vector<CorrectionBlock>>> readCorrection(const std::vector<uint8_t>& data)
{
    std::vector<std::pair<int64_t, std::vector<CorrectionBlock>>> frames;
    for (size_t at = 0; at + HEADER <= data.size();)
    {
        const uint8_t* h = data.data() + at;
        if (std::memcmp(h, "wvpk", 4) != 0)
        {
            ++at;
            continue;
        }
        const uint32_t blockSize = le32(h + 4) + 8;
        if (blockSize < HEADER || blockSize > data.size() - at)
        {
            ++at;
            continue;
        }
        const int64_t index = static_cast<int64_t>(h[10]) << 32 | le32(h + 16);
        const uint32_t samples = le32(h + 20);
        const uint32_t flags = le32(h + 24);
        at += blockSize;
        if (samples == 0)
            continue;
        if ((flags & INITIAL_BLOCK) || frames.empty() || frames.back().first != index)
            frames.push_back({index, {}});
        frames.back().second.push_back({samples, flags & ~HAS_CHECKSUM, le32(h + 28),
                                        (flags & HAS_CHECKSUM) ? withoutChecksum(h + HEADER, blockSize - HEADER)
                                                               : std::vector<uint8_t>(h + HEADER, h + blockSize)});
    }
    return frames;
}
}  // namespace

bool isWavpackFileExt(const std::string& upperExt) { return upperExt == "WV"; }

bool prepareWavpackFile(const std::string& fileName)
{
    const std::string name = unquoteStr(trimStr(fileName));
    if (!isWavpackFileExt(strToUpperCase(extractFileExt(name))))
        return false;
    {
        std::lock_guard<std::mutex> guard(preparedLock);
        if (prepared().count(name))
            return true;
    }
    const std::filesystem::path path = sourcePath(name);
    std::error_code ec;
    const auto fileSize = static_cast<int64_t>(std::filesystem::file_size(path, ec));
    std::ifstream file(path, std::ios::binary);
    if (ec || !file)
        return false;  // said by whatever opens it, as for any file that cannot be opened
    if (fileSize > MAX_WAVPACK_BYTES)
        THROW(ERR_COMMON, "The WavPack file " << name << " is " << fileSize / (1024 * 1024)
                                              << " MiB. tsMuxeR reads a WavPack file into memory, up to "
                                              << MAX_WAVPACK_BYTES / (1024 * 1024) << " MiB.")
    std::vector<uint8_t> data(static_cast<size_t>(fileSize));
    file.read(reinterpret_cast<char*>(data.data()), fileSize);
    data.resize(static_cast<size_t>(file.gcount()));

    // the blocks, a frame each run from an initial to a final block
    std::vector<Frame> frames;
    Frame frame;
    bool inFrame = false;
    uint16_t version = 0;
    int rate = 0;
    int channels = 0;
    int frameChannels = 0;
    int bits = 0;
    uint32_t firstFlags = 0;
    int64_t strayBytes = 0;
    int64_t lostFrames = 0;
    for (size_t at = 0; at < data.size();)
    {
        if (data.size() - at < HEADER || std::memcmp(data.data() + at, "wvpk", 4) != 0)
        {
            // tags at the end (APEv2, ID3v1) and anything else that is no block: passed over to the next block
            if (data.size() - at >= 8 && std::memcmp(data.data() + at, "APETAGEX", 8) == 0)
                break;
            if (data.size() - at == 128 && std::memcmp(data.data() + at, "TAG", 3) == 0)
                break;
            if (frames.empty() && !inFrame && at == 0)
                THROW(ERR_COMMON, "The file " << name << " is no WavPack file: it does not begin with a block.")
            ++strayBytes;
            ++at;
            continue;
        }
        const uint8_t* h = data.data() + at;
        const uint32_t blockSize = le32(h + 4) + 8;
        if (blockSize < HEADER || blockSize > data.size() - at)
        {
            ++strayBytes;  // a damaged size, or the file cut off inside the block
            ++at;
            continue;
        }
        const auto blockVersion = static_cast<uint16_t>(h[8] | h[9] << 8);
        const int64_t index = static_cast<int64_t>(h[10]) << 32 | le32(h + 16);
        const uint32_t samples = le32(h + 20);
        const uint32_t flags = le32(h + 24);
        const uint32_t crc = le32(h + 28);
        std::vector<uint8_t> bodyKept;
        if (flags & HAS_CHECKSUM)
            bodyKept = withoutChecksum(h + HEADER, blockSize - HEADER);
        const uint8_t* body = (flags & HAS_CHECKSUM) ? bodyKept.data() : h + HEADER;
        const size_t bodySize = (flags & HAS_CHECKSUM) ? bodyKept.size() : blockSize - HEADER;
        at += blockSize;
        if (flags & DSD_FLAG)
            THROW(ERR_COMMON, "The WavPack file " << name << " holds DSD audio, which tsMuxeR does not read.")
        if (samples == 0)
            continue;  // a block of metadata only, as the RIFF trailer of a file packed from a WAV
        if (flags & INITIAL_BLOCK)
        {
            if (inFrame)
                ++lostFrames;  // a frame whose final block is lost
            frame = Frame{index, samples, {}};
            putLe32(frame.bytes, samples);
            frameChannels = 0;
            inFrame = true;
            if (version == 0)
            {
                version = blockVersion;
                firstFlags = flags;
                const unsigned code = flags >> 23 & 0xF;
                rate = code < 15 ? SAMPLE_RATES[code] : sampleRateOf(body, bodySize);
                bits = (flags & FLOAT_DATA) ? 32 : static_cast<int>((flags & 3) + 1) * 8;
            }
        }
        else if (!inFrame || index != frame.index)
        {
            continue;  // a block of a frame whose initial block is lost
        }
        frameChannels += (flags & MONO_FLAG) ? 1 : 2;
        ++frame.blocks;
        frame.flags.push_back(flags & ~HAS_CHECKSUM);
        putLe32(frame.bytes, flags & ~HAS_CHECKSUM);
        putLe32(frame.bytes, crc);
        if ((flags & (INITIAL_BLOCK | FINAL_BLOCK)) != (INITIAL_BLOCK | FINAL_BLOCK))
            putLe32(frame.bytes, static_cast<uint32_t>(bodySize));
        frame.bytes.insert(frame.bytes.end(), body, body + bodySize);
        if (flags & FINAL_BLOCK)
        {
            if (channels == 0)
                channels = frameChannels;
            frames.push_back(std::move(frame));
            frame = Frame{};
            inFrame = false;
        }
    }
    if (frames.empty() || rate <= 0)
        THROW(ERR_COMMON, "The WavPack file " << name << " holds no audio that tsMuxeR can read.")
    if (strayBytes > 0 || lostFrames > 0)
        LTRACE(LT_WARN, 2,
               "Warning: the WavPack file " << name << " is damaged: " << strayBytes
                                            << " bytes are no part of a block, and " << lostFrames
                                            << " frames have lost blocks. What is whole is read.");
    // A hybrid file: the correction file beside it (.wvc) makes the sound lossless again. Its frames go into the
    // BlockAdditional (BlockAddID 1) of the frames of the lossy part, as the Matroska mapping of A_WAVPACK4 has it: the
    // crc of each block, its size where the frame has more than one block, and its data.
    bool corrected = false;
    if (firstFlags & HYBRID_FLAG)
    {
        std::string correction = name + (name.compare(name.size() - 2, 2, "WV") == 0 ? "C" : "c");
        std::ifstream cfile(sourcePath(correction), std::ios::binary);
        if (cfile)
        {
            std::vector<uint8_t> cdata((std::istreambuf_iterator<char>(cfile)), std::istreambuf_iterator<char>());
            const auto cframes = readCorrection(cdata);
            size_t c = 0;
            int64_t matched = 0;
            for (Frame& f : frames)
            {
                while (c < cframes.size() && cframes[c].first < f.index) ++c;
                if (c >= cframes.size() || cframes[c].first != f.index ||
                    static_cast<int>(cframes[c].second.size()) != f.blocks)
                    continue;
                // The header of a correction block is that of its lossy block but for the size and the crc: a block
                // of another file with the same block_index is not taken for this one's
                bool same = true;
                for (size_t k = 0; k < cframes[c].second.size() && same; ++k)
                    same = cframes[c].second[k].samples == f.samples && cframes[c].second[k].flags == f.flags[k];
                if (!same)
                    continue;
                for (const CorrectionBlock& cb : cframes[c].second)
                {
                    putLe32(f.correction, cb.crc);
                    if (f.blocks > 1)
                        putLe32(f.correction, static_cast<uint32_t>(cb.data.size()));
                    f.correction.insert(f.correction.end(), cb.data.begin(), cb.data.end());
                }
                ++matched;
            }
            corrected = matched > 0;
            if (matched < static_cast<int64_t>(frames.size()))
                LTRACE(LT_WARN, 2,
                       "Warning: the correction file " << correction << " of " << name << " matches " << matched
                                                       << " of its " << frames.size()
                                                       << " frames; the others are only the lossy part.");
        }
        else
            LTRACE(LT_INFO, 2,
                   name << " is the lossy part of a hybrid WavPack file, and no correction file " << correction
                        << " is beside it: the lossy part is muxed.");
    }

    // the Matroska file: one track, a cluster each 30 seconds so that a block's time fits the 16 bits it is given
    const auto ms = [rate](const int64_t sample)
    { return static_cast<int64_t>(std::llround(static_cast<double>(sample) * 1000.0 / rate)); };
    std::vector<uint8_t> mkv;
    ebmlHeader(mkv);
    putId(mkv, 0x18538067);
    const size_t segmentSizeAt = mkv.size();
    putSize(mkv, 0);
    const size_t segmentStart = mkv.size();
    {
        std::vector<uint8_t> info;
        uintElement(info, 0x2AD7B1, 1000000);  // TimestampScale: milliseconds
        stringElement(info, 0x4D80, "");       // no application wrote it: the listing names none
        stringElement(info, 0x5741, "");
        int64_t total = 0;
        for (const Frame& f : frames) total += f.samples;
        floatElement(info, 0x4489, static_cast<double>(total) * 1000.0 / rate);
        element(mkv, 0x1549A966, info);
    }
    {
        std::vector<uint8_t> entry;
        uintElement(entry, 0xD7, 1);
        uintElement(entry, 0x73C5, 1);
        uintElement(entry, 0x83, 2);  // audio
        uintElement(entry, 0x9C, 0);
        stringElement(entry, 0x86, "A_WAVPACK4");
        element(entry, 0x63A2, {static_cast<uint8_t>(version), static_cast<uint8_t>(version >> 8)});
        stringElement(entry, 0x22B59C, "und");
        if (corrected)
            uintElement(entry, 0x55EE, 1);  // MaxBlockAdditionID: the correction part in BlockAddID 1
        std::vector<uint8_t> audio;
        floatElement(audio, 0xB5, rate);
        uintElement(audio, 0x9F, static_cast<uint64_t>(channels));
        uintElement(audio, 0x6264, static_cast<uint64_t>(bits));
        element(entry, 0xE1, audio);
        std::vector<uint8_t> tracks;
        element(tracks, 0xAE, entry);
        element(mkv, 0x1654AE6B, tracks);
    }
    // each frame at the samples of the frames before it: a block_index that goes back, as in two files joined or a
    // damaged one, cannot put a frame before the one in front of it
    std::vector<int64_t> at(frames.size());
    for (size_t k = 1; k < frames.size(); ++k) at[k] = at[k - 1] + frames[k - 1].samples;
    for (size_t i = 0; i < frames.size();)
    {
        const int64_t clusterMs = ms(at[i]);
        std::vector<uint8_t> cluster;
        uintElement(cluster, 0xE7, static_cast<uint64_t>(clusterMs));
        for (; i < frames.size() && ms(at[i]) - clusterMs <= 30000; ++i)
        {
            const auto rel = static_cast<int16_t>(ms(at[i]) - clusterMs);
            const bool withCorrection = !frames[i].correction.empty();
            // a SimpleBlock of audio is a key frame; a Block has no such flag
            std::vector<uint8_t> block{0x81, static_cast<uint8_t>(static_cast<uint16_t>(rel) >> 8),
                                       static_cast<uint8_t>(rel & 0xFF),
                                       static_cast<uint8_t>(withCorrection ? 0 : 0x80)};
            block.insert(block.end(), frames[i].bytes.begin(), frames[i].bytes.end());
            if (withCorrection)
            {
                std::vector<uint8_t> more;
                uintElement(more, 0xEE, 1);  // BlockAddID
                element(more, 0xA5, frames[i].correction);
                std::vector<uint8_t> additions;
                element(additions, 0xA6, more);
                std::vector<uint8_t> group;
                element(group, 0xA1, block);
                element(group, 0x75A1, additions);
                element(cluster, 0xA0, group);
            }
            else
                element(cluster, 0xA3, block);
        }
        element(mkv, 0x1F43B675, cluster);
    }
    const uint64_t segmentSize = mkv.size() - segmentStart;
    for (int k = 0; k < 7; ++k) mkv[segmentSizeAt + 1 + k] = static_cast<uint8_t>(segmentSize >> (48 - 8 * k));
    discImage::addMemoryFile(name, std::move(mkv));
    std::lock_guard<std::mutex> guard(preparedLock);
    prepared().insert(name);
    return true;
}
