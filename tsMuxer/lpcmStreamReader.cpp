#include "lpcmStreamReader.h"

#include <algorithm>
#include <cmath>
#include <sstream>

#include <fs/systemlog.h>

#include "ioContextDemuxer.h"
#include "tsPacket.h"
#include "vodCoreException.h"
#include "vod_common.h"
#include "wave.h"

namespace
{
uint32_t FOUR_CC(const char a, const char b, const char c, const char d)
{
    return my_ntohl(static_cast<uint32_t>(a) << 24 | b << 16 | c << 8 | d);
}

// Sony Wave64. Every four character chunk id becomes a 16 byte GUID whose first four bytes are
// that same ASCII id, and every 32 bit size becomes 64 bit. Two details are easy to get wrong and
// are the whole reason a reader rejects a file: a chunk's size INCLUDES its own 24 byte header,
// and chunks are padded to an 8 byte boundary.
//
// With a WAVEFORMATPCMEX of 40 bytes the header comes to exactly 128 bytes and needs no padding:
//
//   0    riff GUID           16
//   16   size of the file     8   <- patched on close
//   24   wave GUID           16
//   40   fmt GUID            16
//   56   size of fmt chunk    8   = 16 + 8 + 40
//   64   WAVEFORMATPCMEX     40
//   104  data GUID           16
//   120  size of data chunk   8   <- patched on close
//   128  the audio
// Every chunk GUID begins with the four character id, and the remaining twelve bytes are NOT the
// same for all of them. The riff chunk has its own suffix; wave, fmt and data share a different
// one. Writing the riff suffix on all four produced a file that tsMuxeR itself read back happily,
// because its own detector compares only the first sixteen bytes, while ffprobe and two versions
// of libsndfile all refused it: "No 'data' chunk marker". The audio was never affected, only the
// thirty six bytes of those three GUIDs.
constexpr uint8_t W64_RIFF_SUFFIX[12] = {0x2E, 0x91, 0xCF, 0x11, 0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00};
constexpr uint8_t W64_CHUNK_SUFFIX[12] = {0xF3, 0xAC, 0xD3, 0x11, 0x8C, 0xD1, 0x00, 0xC0, 0x4F, 0x8E, 0xDB, 0x8A};
constexpr int64_t W64_HEADER_SIZE = 128;
constexpr int64_t W64_RIFF_SIZE_OFFSET = 16;
constexpr int64_t W64_DATA_SIZE_OFFSET = 120;
constexpr int64_t W64_CHUNK_HEADER = 24;  // a 16 byte GUID and an 8 byte size

uint8_t* writeW64Guid(uint8_t* dst, const char* fourcc)
{
    // The suffix is chosen here rather than at the call sites so that a chunk added later cannot
    // quietly get the wrong one.
    const bool isRiff = memcmp(fourcc, "riff", 4) == 0;
    memcpy(dst, fourcc, 4);
    memcpy(dst + 4, isRiff ? W64_RIFF_SUFFIX : W64_CHUNK_SUFFIX, 12);
    return dst + 16;
}

uint8_t* writeLE64(uint8_t* dst, const uint64_t value)
{
    for (int i = 0; i < 8; ++i) dst[i] = static_cast<uint8_t>(value >> (i * 8));
    return dst + 8;
}
}  // namespace

static constexpr int m2tsFreqs[] = {0, 48000, 0, 0, 96000, 192000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static constexpr int MAX_HEADER_SIZE = 192;

static const uint32_t RIFF_SMALL = FOUR_CC('r', 'i', 'f', 'f');
static const uint32_t RIFF_LARGE = FOUR_CC('R', 'I', 'F', 'F');

using namespace wave_format;

int LPCMStreamReader::getTSDescriptor(uint8_t* dstBuff, bool blurayMode, bool hdmvDescriptors)
{
    if (m_headerType == LPCMHeaderType::htNone)
        if (!detectLPCMType(m_buffer, m_bufEnd - m_buffer))
            return 0;
    uint8_t* frame = findFrame(m_buffer, m_bufEnd);
    if (frame == nullptr)
        return 0;
    int skipBytes = 0;
    int skipBeforeBytes = 0;
    const int len = decodeFrame(frame, m_bufEnd, skipBytes, skipBeforeBytes);
    if (len < 1)
        return 0;

    // Blu-ray core specifications Table 9-11 - HDMV LPCM audio registration descriptor
    uint8_t* curPos = dstBuff;
    *curPos++ = static_cast<uint8_t>(TSDescriptorTag::REGISTRATION);  // descriptor tag
    *curPos++ = 8;                                                    // descriptor length
    *curPos++ = 'H';
    *curPos++ = 'D';
    *curPos++ = 'M';
    *curPos++ = 'V';
    *curPos++ = 0xff;  // stuffing_bits

    *curPos++ = static_cast<uint8_t>(TSDescriptorTag::LPCM);  // descriptor tag
    const int audio_presentation_type = (bdChannels() > 2) ? 6 : (bdChannels() == 2) ? 3 : 1;
    const int sampling_frequency = (m_freq == 192000) ? 5 : (m_freq == 96000) ? 4 : 1;
    *curPos++ = static_cast<uint8_t>(audio_presentation_type << 4 | sampling_frequency);
    *curPos++ = static_cast<uint8_t>((m_bitsPerSample - 12) << 4 | 0x3f);  // bits_per_sample (2 bits), stuffing_bits

    return static_cast<int>(curPos - dstBuff);
}

int LPCMStreamReader::decodeLPCMHeader(const uint8_t* buff)
{
    const int audio_data_payload_size = AV_RB16(buff);
    const int channelsIndex = buff[2] >> 4;
    m_lfeExists = false;
    m_backCentre = false;
    switch (channelsIndex)
    {
    case 0:
    case 1:
        m_channels = 1;
        break;
    case 2:
    case 3:
        m_channels = 2;
        break;
    case 4:
        m_channels = 3;
        break;
    // 2/1 and 3/1: left, right (and centre) and ONE surround, which is the back centre speaker; neither has an LFE.
    // They were read as 2.1 and 3.1, so a demux gave the surround as the LFE.
    case 5:
        m_channels = 3;
        m_backCentre = true;
        break;
    case 6:
        m_channels = 4;
        m_backCentre = true;
        break;
    case 7:
        m_channels = 4;
        break;
    case 8:
        m_channels = 5;
        break;
    case 9:
        m_channels = 6;
        m_lfeExists = true;
        break;
    case 10:
        m_channels = 7;
        break;
    case 11:
        m_channels = 8;
        m_lfeExists = true;
        break;
    default:;
    }
    const int sampling_index = buff[2] & 0x0f;
    m_freq = m2tsFreqs[sampling_index];
    const uint8_t bits_per_sample = buff[3] >> 6;
    if (bits_per_sample > 0)
        m_bitsPerSample = static_cast<uint16_t>(12 + 4 * bits_per_sample);
    return audio_data_payload_size;
}

void LPCMStreamReader::storeChannelData(const uint8_t* start, const uint8_t* end, const int chNum, uint8_t* tmpData,
                                        const int mch) const
{
    const int ch1SampleSize = (m_bitsPerSample == 20 ? 3 : m_bitsPerSample / 8);
    const int fullSampleSize = mch * ch1SampleSize;
    const int offset = ch1SampleSize * (chNum - 1);
    const uint8_t* curPos = start + offset;
    uint8_t* dst = tmpData;
    for (; curPos < end; curPos += fullSampleSize)
    {
        for (int j = 0; j < ch1SampleSize; j++) *dst++ = curPos[j];
    }
}

void LPCMStreamReader::restoreChannelData(uint8_t* start, const uint8_t* end, const int chNum, const uint8_t* tmpData,
                                          const int mch) const
{
    const int ch1SampleSize = (m_bitsPerSample == 20 ? 3 : m_bitsPerSample / 8);
    const int fullSampleSize = mch * ch1SampleSize;
    const int offset = ch1SampleSize * (chNum - 1);
    uint8_t* curPos = start + offset;
    const uint8_t* src = tmpData;
    for (; curPos < end; curPos += fullSampleSize)
    {
        for (int j = 0; j < ch1SampleSize; j++) curPos[j] = *src++;
    }
}

void LPCMStreamReader::copyChannelData(uint8_t* start, const uint8_t* end, const int chFrom, const int chTo,
                                       const int mch) const
{
    // int mch = m_channels + (m_channels%2==1 ? 1 : 0);
    const int ch1SampleSize = (m_bitsPerSample == 20 ? 3 : m_bitsPerSample / 8);
    const int fullSampleSize = mch * ch1SampleSize;
    int offset = ch1SampleSize * (chFrom - 1);
    const uint8_t* src = start + offset;
    offset = ch1SampleSize * (chTo - 1);
    uint8_t* dst = start + offset;
    for (; src < end; src += fullSampleSize)
    {
        for (int j = 0; j < ch1SampleSize; j++) dst[j] = src[j];
        dst += fullSampleSize;
    }
}

void LPCMStreamReader::removeChannel(uint8_t* start, const uint8_t* end, const int cnNum, const int mch) const
{
    // int mch = m_channels + (m_channels%2==1 ? 1 : 0);
    assert(mch == cnNum);
    const int ch1SampleSize = (m_bitsPerSample == 20 ? 3 : m_bitsPerSample / 8);
    const int fullSampleSize = mch * ch1SampleSize;
    uint8_t* dst = start;
    for (const uint8_t* curPos = start; curPos < end; curPos += fullSampleSize)
    {
        memmove(dst, curPos, fullSampleSize);
        dst += fullSampleSize - ch1SampleSize;
    }
}

// ** A WAV'S CHANNELS ON A DISC. ** A Blu-ray LPCM track has a fixed set of layouts: 1/0, 2/0, 3/0, 2/1, 3/1, 2/2, 3/2,
// 3/2+LFE, 3/4 and 3/4+LFE, the single surround of 2/1 and 3/1 being the back centre. The layout was chosen by the
// number of channels and the LFE bit alone, so a WAV whose speakers are not one of them went onto the disc in the wrong
// places: the LFE of 2.1 and 3.1 played as the surround, the centre of 4.0 as the left surround, the LFE of 4.1 as the
// centre, and the back centre of 6.0 as the LFE. The WAV's own speakers now choose the layout. A layout the disc has
// is written as it is; an LFE without a layout of its own (2.1, 3.1, 4.1) and side or back pairs without a centre get
// the next larger layout with silent channels, every channel at its own speaker; and speakers a disc has no place for
// (a back centre beside a pair, top speakers) are refused. A mask that does not name one speaker per channel says
// nothing, and the channels are taken as before.
void LPCMStreamReader::planBdLayout(const uint32_t mask)
{
    m_bdMap.clear();
    m_backCentre = false;
    std::vector<uint32_t> order;  // the speaker of each channel, in the order of the WAV
    for (uint32_t bit = 1; bit != 0; bit <<= 1)
        if (mask & bit)
            order.push_back(bit);
    if (static_cast<int>(order.size()) != m_channels || m_channels < 2)
        return;
    const uint32_t known = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                           SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT | SPEAKER_BACK_CENTER | SPEAKER_SIDE_LEFT |
                           SPEAKER_SIDE_RIGHT;
    const auto refuse = [&](const char* why)
    {
        THROW(ERR_COMMON, "The WAV of track " << m_streamIndex << " has the speaker layout 0x" << std::hex << mask
                                              << std::dec << ", and " << why
                                              << " Convert it to stereo, 5.1 or 7.1 before muxing it to a disc.")
    };
    if (mask & ~known)
        refuse("a Blu-ray LPCM track has no place for some of its speakers (such as top or front centre-side ones).");
    const bool backPair = (mask & (SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT)) == (SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT);
    const bool sidePair = (mask & (SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT)) == (SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT);
    if (((mask & (SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT)) && !backPair) ||
        ((mask & (SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT)) && !sidePair) ||
        (mask & (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)) != (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT))
        refuse("one speaker of a left and right pair is missing, which no Blu-ray LPCM layout has.");
    const bool lfe = mask & SPEAKER_LOW_FREQUENCY;
    const bool centre = mask & SPEAKER_FRONT_CENTER;
    if (mask & SPEAKER_BACK_CENTER)
    {
        if (backPair || sidePair || lfe)
            refuse("its back centre comes with other surround speakers or an LFE, which no Blu-ray LPCM layout has.");
        m_backCentre = true;  // 2/1 or 3/1, in the order of the WAV
        return;
    }
    // The layout on the disc, in the order of a WAV: what the code below and convertWavToPCM's remap expect
    std::vector<uint32_t> target;
    const uint32_t first = backPair ? SPEAKER_BACK_LEFT : SPEAKER_SIDE_LEFT;
    const uint32_t second = backPair ? SPEAKER_BACK_RIGHT : SPEAKER_SIDE_RIGHT;
    if (!backPair && !sidePair)
    {
        if (!lfe)
            return;  // 2/0 or 3/0
        target = {SPEAKER_FRONT_LEFT,    SPEAKER_FRONT_RIGHT, SPEAKER_FRONT_CENTER,
                  SPEAKER_LOW_FREQUENCY, SPEAKER_BACK_LEFT,   SPEAKER_BACK_RIGHT};
    }
    else if (backPair != sidePair)
    {
        if (centre || !lfe)
            return;  // 2/2, 3/2 or 3/2+LFE
        target = {SPEAKER_FRONT_LEFT, SPEAKER_FRONT_RIGHT, SPEAKER_FRONT_CENTER, SPEAKER_LOW_FREQUENCY, first, second};
    }
    else
    {
        if (centre)
            return;  // 3/4 or 3/4+LFE
        target = {SPEAKER_FRONT_LEFT, SPEAKER_FRONT_RIGHT, SPEAKER_FRONT_CENTER};
        if (lfe)
            target.push_back(SPEAKER_LOW_FREQUENCY);
        for (const uint32_t s : {SPEAKER_BACK_LEFT, SPEAKER_BACK_RIGHT, SPEAKER_SIDE_LEFT, SPEAKER_SIDE_RIGHT})
            target.push_back(s);
    }
    for (const uint32_t speaker : target)
    {
        const auto at = std::find(order.begin(), order.end(), speaker);
        m_bdMap.push_back(at == order.end() ? -1 : static_cast<int>(at - order.begin()));
    }
    if (bdChannels() > 6 && m_freq == 192000)
        refuse(
            "with silent channels added for a disc layout it would have more than the 6 channels a disc carries at "
            "192 kHz.");
    LTRACE(LT_INFO, 2,
           "The WAV of track " << m_streamIndex << " has " << static_cast<int>(m_channels)
                               << " channels in a layout a Blu-ray does not have; it is written with " << bdChannels()
                               << " channels, the missing ones silent, each channel at its own speaker.");
}

int LPCMStreamReader::convertWavToPCM(uint8_t* start, uint8_t* end)
{
    // int mch = m_channels; // + (m_channels%2==1 ? 1 : 0);
    const int ch1SampleSize = (m_bitsPerSample == 20 ? 3 : m_bitsPerSample / 8);
    const int fullSampleSize = m_channels * ch1SampleSize;
    if (end - start < fullSampleSize)
        return 0;
    int64_t cLen = end - start;
    cLen -= cLen % fullSampleSize;
    end = start + cLen;

    // 1. convert byte order to little endian
    if (m_bitsPerSample == 16)
    {
        for (auto curPos = UnalignedPtr<uint16_t>(start); curPos < UnalignedPtr<uint16_t>(end); ++curPos)
            *curPos = my_htons(*curPos);
    }
    else if (m_bitsPerSample > 16)
    {
        for (uint8_t* curPos = start; curPos < end - 2; curPos += 3)
        {
            const uint8_t tmp = curPos[0];
            curPos[0] = curPos[2];
            curPos[2] = tmp;
        }
    }
    // 1b. The disc layout: the WAV's channels in their places, silent channels where the layout has more (planBdLayout)
    int ch = m_channels;
    if (!m_bdMap.empty())
    {
        ch = bdChannels();
        const int64_t samples = (end - start) / fullSampleSize;
        m_bdBuffer.resize(static_cast<size_t>(samples * ch * ch1SampleSize));
        uint8_t* out = m_bdBuffer.data();
        for (const uint8_t* in = start; in < end; in += fullSampleSize)
            for (const int from : m_bdMap)
            {
                if (from >= 0)
                    memcpy(out, in + from * ch1SampleSize, ch1SampleSize);
                else
                    memset(out, 0, ch1SampleSize);
                out += ch1SampleSize;
            }
        start = m_bdBuffer.data();
        end = start + m_bdBuffer.size();
    }
    const int64_t chFullSize = (end - start) / ch;
    // 2. Remap channels to Blu-ray standard
    if (ch == 6)
    {
        const auto tmpData = new uint8_t[chFullSize];
        storeChannelData(start, end, 4, tmpData, ch);    // copy channel 6(LFE) to tmpData
        copyChannelData(start, end, 5, 4, ch);           // Shift RS
        copyChannelData(start, end, 6, 5, ch);           // Shift LS
        restoreChannelData(start, end, 6, tmpData, ch);  // copy LFE to channel #4
        delete[] tmpData;
    }
    else if (ch == 7)
    {
        const auto tmpData = new uint8_t[chFullSize];

        storeChannelData(start, end, 4, tmpData, ch);  // copy channel 8(LFE) to tmpData
        copyChannelData(start, end, 6, 4, ch);
        copyChannelData(start, end, 5, 6, ch);
        restoreChannelData(start, end, 5, tmpData, ch);

        delete[] tmpData;
    }
    else if (ch == 8)
    {
        const auto lfeData = new uint8_t[chFullSize];

        storeChannelData(start, end, 4, lfeData, ch);  // copy channel 8(LFE) to tmpData
        copyChannelData(start, end, 7, 4, ch);
        copyChannelData(start, end, 8, 7, ch);
        restoreChannelData(start, end, 8, lfeData, ch);

        delete[] lfeData;
    }
    // 3. transfer to frame buffer and Add X channel (zero channel for padding if needed)
    uint8_t* dst = m_tmpFrameBuffer + (m_needPCMHdr ? 4 : 0);
    const uint8_t* src = start;
    const int outSampleSize = ch * ch1SampleSize;
    for (; src < end; src += outSampleSize)
    {
        memcpy(dst, src, outSampleSize);
        dst += outSampleSize;
        if (ch % 2 == 1)
        {
            memset(dst, 0, ch1SampleSize);
            dst += ch1SampleSize;
        }
    }
    assert(dst < m_tmpFrameBuffer + sizeof(m_tmpFrameBuffer));
    // 4. add LPCM frame header
    if (m_needPCMHdr)
    {
        const int audio_data_payload_size = (m_bitsPerSample == 20 ? 24 : m_bitsPerSample) * m_freq *
                                            ((ch + 1) & 0xfe) / 8 / 200;  // 5 ms frame len. 1000/5 = 200
        // int audio_data_payload_size = dst - (m_tmpFrameBuffer + 4);
        m_tmpFrameBuffer[0] = static_cast<uint8_t>(audio_data_payload_size >> 8);
        m_tmpFrameBuffer[1] = audio_data_payload_size & 0xff;
        int channelsIndex = 1;
        switch (ch)
        {
        case 2:
            channelsIndex = 3;
            break;
        // 3/0 or 2/1, 2/2 or 3/1: the single surround of 2/1 and 3/1 is the back centre (m_backCentre). A WAV with an
        // LFE and three or four channels has no layout of its own on a disc: planBdLayout gives it 3/2+LFE. It got the
        // 2/1 and 3/1 codes, so its LFE played as the surround.
        case 3:
            channelsIndex = m_backCentre ? 5 : 4;
            break;
        case 4:
            channelsIndex = m_backCentre ? 6 : 7;
            break;
        case 5:
            channelsIndex = 8;
            break;
        case 6:
            channelsIndex = 9;
            break;
        case 7:
            channelsIndex = 10;
            break;
        case 8:
            channelsIndex = 11;
            break;
        default:;
        }
        int sampling_index = 1;
        if (m_freq == 96000)
            sampling_index = 4;
        else if (m_freq == 192000)
            sampling_index = 5;
        m_tmpFrameBuffer[2] = (channelsIndex << 4 | sampling_index) & 0xff;
        const int bits_per_sample = (m_bitsPerSample - 12) / 4;
        // Bit 5 of this byte was set on the first frame of every output file (m_firstFrame is
        // re-armed by onSplitEvent). No pressed disc does that: across two retail sources from
        // different studios, bit 5 is clear on ALL 10,014 frames of a 24-bit 5.1 track and all
        // 18,018 frames of a 16-bit 2.0 one. Setting it was the single byte that stopped a
        // remuxed LPCM track from being bit-identical to its source. m_firstFrame is still
        // needed in demux mode, where it triggers the WAV header, so only this use is dropped.
        m_tmpFrameBuffer[3] = (bits_per_sample << 6) & 0xff;
    }
    return static_cast<int>(dst - m_tmpFrameBuffer);
}

int LPCMStreamReader::convertLPCMToWAV(uint8_t* start, uint8_t* end)
{
    const int mch = m_channels + (m_channels % 2 == 1 ? 1 : 0);
    const int64_t ch1FullSize = (end - start) / mch;

    if (m_lastChannelRemapPos != start)
    {
        m_lastChannelRemapPos = start;

        // 1. convert byte order to little endian
        if (m_bitsPerSample == 16)
        {
            for (auto curPos = UnalignedPtr<uint16_t>(start); curPos < UnalignedPtr<uint16_t>(end); ++curPos)
                *curPos = my_ntohs(*curPos);
        }
        else if (m_bitsPerSample > 16)
        {
            for (uint8_t* curPos = start; curPos < end; curPos += 3)
            {
                const uint8_t tmp = curPos[0];
                curPos[0] = curPos[2];
                curPos[2] = tmp;
            }
        }
        // 2. Remap channels to WAV standard
        if (m_channels == 1)
        {
            removeChannel(start, end, 2, mch);
        }
        else if (m_channels == 3)
        {
            removeChannel(start, end, 4, mch);
        }
        else if (m_channels == 5)
        {
            removeChannel(start, end, 6, mch);
        }
        else if (m_channels == 6)
        {
            const auto tmpData = new uint8_t[ch1FullSize];
            storeChannelData(start, end, 6, tmpData, mch);    // copy channel 6(LFE) to tmpData
            copyChannelData(start, end, 5, 6, mch);           // Shift RS
            copyChannelData(start, end, 4, 5, mch);           // Shift LS
            restoreChannelData(start, end, 4, tmpData, mch);  // copy LFE to channel #4
            delete[] tmpData;
        }
        else if (m_channels == 7)
        {
            const auto tmpData = new uint8_t[ch1FullSize];

            storeChannelData(start, end, 4, tmpData, mch);  // copy channel 8(LFE) to tmpData
            copyChannelData(start, end, 5, 4, mch);
            copyChannelData(start, end, 6, 5, mch);
            restoreChannelData(start, end, 6, tmpData, mch);

            delete[] tmpData;
        }
        else if (m_channels == 8)
        {
            const auto lfeData = new uint8_t[ch1FullSize];

            storeChannelData(start, end, 8, lfeData, mch);
            copyChannelData(start, end, 7, 8, mch);
            copyChannelData(start, end, 4, 7, mch);
            restoreChannelData(start, end, 4, lfeData, mch);

            delete[] lfeData;
        }
        if (m_channels == 7)
            removeChannel(start, end, 8, mch);
    }

    return static_cast<int>(end - start - (m_channels % 2 == 1 ? ch1FullSize : 0));
}

uint8_t* LPCMStreamReader::findSubstr(const char* pattern, uint8_t* buff, const uint8_t* end)
{
    const size_t patternLen = strlen(pattern);
    for (uint8_t* curPos = buff; curPos < end - patternLen; curPos++)
    {
        for (size_t j = 0; j < patternLen; j++)
        {
            if (curPos[j] != static_cast<uint8_t>(pattern[j]))
                break;
            if (j == patternLen - 1)
                return curPos;
        }
    }
    return nullptr;
}

int LPCMStreamReader::decodeWaveHeader(uint8_t* buff, uint8_t* end)
{
    if (end - buff < 20)
        return NOT_ENOUGH_BUFFER;
    uint8_t* curPos = buff;
    // if (m_channels == 0)
    {
        // The format chunk can sit at any address, as in two WAV files joined: it is copied into an aligned struct
        // rather than read through a pointer cast to it, which reads misaligned fields (undefined behaviour).
        alignas(WAVEFORMATPCMEX) uint8_t fmtCopy[sizeof(WAVEFORMATPCMEX)] = {};
        const auto* waveFormatPCMEx = reinterpret_cast<const WAVEFORMATPCMEX*>(fmtCopy);
        const uint8_t* fmtPos;
        uint64_t fmtSize;
        if (m_headerType == LPCMHeaderType::htWAVE64)
        {
            curPos = findSubstr("fmt ", buff, end);
            if (curPos == nullptr || curPos + sizeof(GUID) + 8 >= end)
                return NOT_ENOUGH_BUFFER;
            uint8_t* tmpPos = curPos + sizeof(GUID);
            fmtSize = loadUnaligned<uint64_t>(tmpPos);
            if (curPos + fmtSize >= end)
                return NOT_ENOUGH_BUFFER;
            fmtPos = tmpPos + 8;
        }
        else
        {
            curPos = findSubstr("fmt ", buff, end);
            if (curPos == nullptr || curPos + 8 >= end)
                return NOT_ENOUGH_BUFFER;
            fmtSize = loadUnaligned<uint32_t>(curPos + 4);
            if (curPos + 8 + fmtSize >= end)
                return NOT_ENOUGH_BUFFER;
            curPos += 8;
            fmtPos = curPos;
        }
        memcpy(fmtCopy, fmtPos, std::min<size_t>(sizeof(fmtCopy), static_cast<size_t>(end - fmtPos)));

        m_channels = static_cast<uint8_t>(waveFormatPCMEx->nChannels);
        if (m_channels > 8)
            THROW(ERR_COMMON,
                  "Too many channels: " << static_cast<int>(m_channels) << ". Maximum supported value is 8(7.1)")
        if (m_channels == 0)
            THROW(ERR_COMMON, "Invalid channels count: 0. WAVE header is invalid.")
        m_freq = waveFormatPCMEx->nSamplesPerSec;
        if (m_freq != 48000 && m_freq != 96000 && m_freq != 192000)
            THROW(ERR_COMMON,
                  "Sample rate " << m_freq << " is not supported for LPCM format. Allowed values: 48000, 96000, 192000")
        m_bitsPerSample = waveFormatPCMEx->wBitsPerSample;
        if (m_bitsPerSample != 16 && m_bitsPerSample != 20 && m_bitsPerSample != 24)
            THROW(ERR_COMMON, "Bit depth " << m_bitsPerSample
                                           << " is not supported for LPCM format. Allowed values: 16bit, 20bit, 24bit")
        m_lfeExists = false;
        if (waveFormatPCMEx->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        {
            // The samples fill the container size wBitsPerSample, the valid bits are only their top bits. A disc
            // knows 20 bit samples in a 24 bit container, so a valid size of up to 20 bits in 24 is written as 20
            // bit; any other valid size keeps the container size, which also sets the sample stride.
            const uint16_t validBits = waveFormatPCMEx->Samples.wValidBitsPerSample;
            if (m_bitsPerSample == 24 && validBits && validBits <= 20)
                m_bitsPerSample = 20;
            m_lfeExists = waveFormatPCMEx->dwChannelMask & SPEAKER_LOW_FREQUENCY;
            planBdLayout(waveFormatPCMEx->dwChannelMask);
            if (!(waveFormatPCMEx->SubFormat == KSDATAFORMAT_SUBTYPE_PCM))
                THROW(ERR_COMMON, "Unsupported WAVE format. Only PCM audio is supported.")
        }
        else if (waveFormatPCMEx->wFormatTag == 0x01)
        {  // standard format
            if (m_channels > 2)
            {
                if (m_channels == 3)
                {
                    LTRACE(LT_WARN, 2,
                           "Warning! Multi channels WAVE file for stream "
                               << m_streamIndex
                               << " do not contain channels configuration info. Applying default value: L R LFE");
                    m_lfeExists = true;
                }
                else if (m_channels == 4)
                {
                    LTRACE(LT_WARN, 2,
                           "Warning! Multi channels WAVE file for stream "
                               << m_streamIndex
                               << " do not contain channels configuration info. Applying default value: L R BL BR");
                }
                else if (m_channels == 5)
                {
                    LTRACE(LT_WARN, 2,
                           "Warning! Multi channels WAVE file for stream "
                               << m_streamIndex
                               << " do not contain channels configuration info. Applying default value: L R C BL BR");
                }
                else if (m_channels == 6)
                {
                    LTRACE(
                        LT_WARN, 2,
                        "Warning! Multi channels WAVE file for stream "
                            << m_streamIndex
                            << " do not contain channels configuration info. Applying default value: L R C LFE BL BR");
                    m_lfeExists = true;
                }
                else if (m_channels == 7)
                {
                    LTRACE(LT_WARN, 2,
                           "Warning! Multi channels WAVE file for stream "
                               << m_streamIndex
                               << " do not contain channels configuration info. Applying default value: L R C BL BR SL "
                                  "SR");
                }
                else if (m_channels == 8)
                {
                    LTRACE(LT_WARN, 2,
                           "Warning! Multi channels WAVE file for stream "
                               << m_streamIndex
                               << " do not contain channels configuration info. Applying default value: L R C LFE BL "
                                  "BR SL SR");
                    m_lfeExists = true;
                }
            }
        }
        else
            THROW(ERR_COMMON, "Unsupported WAVE format. wFormatTag: " << waveFormatPCMEx->wFormatTag)
        if (waveFormatPCMEx->wFormatTag == 0x01 && m_channels > 2)
            planBdLayout(getWaveChannelMask(m_channels, m_lfeExists));
        // A Wave64 chunk is padded to 8 bytes: an 18 byte format (42 with its header) has 6 pad bytes,
        // and the chunks after it were looked for from inside them.
        if (m_headerType == LPCMHeaderType::htWAVE64)
            fmtSize = (fmtSize + 7) & ~static_cast<uint64_t>(7);
        curPos += fmtSize;
    }

    // The chunks between the format and the audio are stepped over by their own sizes: a LIST
    // chunk of tags, a bext chunk, an iPhone FLLR filler or a JUNK pad. The audio used to be looked
    // for only in the first 192 bytes after the format, so behind a larger chunk the header itself
    // was taken for audio: a WAV with a 1 KB tag chunk began with 1,076 bytes of noise. A chunk is
    // only stepped over when its name is four printable characters; anything else is searched as
    // before.
    const bool wave64 = m_headerType == LPCMHeaderType::htWAVE64;
    const int chunkHeaderSize = wave64 ? static_cast<int>(sizeof(GUID)) + 8 : 8;
    bool walked = false;
    while (true)
    {
        if (end - curPos < chunkHeaderSize)
            return NOT_ENOUGH_BUFFER;
        if (memcmp(curPos, "data", 4) == 0)
        {
            walked = true;
            break;
        }
        bool printable = true;
        for (int i = 0; i < 4; ++i) printable = printable && curPos[i] >= 0x20 && curPos[i] < 0x7f;
        if (!printable)
            break;
        uint64_t chunkSize;  // the whole chunk, its header included
        if (wave64)
        {
            chunkSize = loadUnaligned<uint64_t>(curPos + sizeof(GUID));
            if (chunkSize < static_cast<uint64_t>(chunkHeaderSize))
                break;
            chunkSize = (chunkSize + 7) & ~static_cast<uint64_t>(7);
        }
        else
        {
            const uint32_t bodySize = loadUnaligned<uint32_t>(curPos + 4);
            chunkSize = 8 + static_cast<uint64_t>(bodySize) + (bodySize & 1);
            // A chunk of odd size is followed by a pad byte of 0, but some writers leave it out:
            // the next chunk's name then begins where the pad byte would be.
            if ((bodySize & 1) && chunkSize <= static_cast<uint64_t>(end - curPos) && curPos[chunkSize - 1] != 0)
                --chunkSize;
        }
        if (chunkSize > static_cast<uint64_t>(DEFAULT_FILE_BLOCK_SIZE))
            THROW(ERR_COMMON, "WAVE header: the chunk '" << std::string(reinterpret_cast<char*>(curPos), 4)
                                                         << "' before the audio is " << chunkSize
                                                         << " bytes. A chunk of more than " << DEFAULT_FILE_BLOCK_SIZE
                                                         << " bytes before the audio is not supported.")
        if (chunkSize > static_cast<uint64_t>(end - curPos))
            return NOT_ENOUGH_BUFFER;
        curPos += chunkSize;
    }
    if (!walked)
    {
        const uint8_t* searchFrom = curPos;  // curPos is null when nothing is found
        curPos = findSubstr("data", curPos, FFMIN(curPos + MAX_HEADER_SIZE, end));
        if (curPos == nullptr)
        {
            return end < searchFrom + MAX_HEADER_SIZE ? NOT_ENOUGH_BUFFER : 0;
            // 'riff' header was wrong detected, it is just data
        }
    }

    if (m_headerType == LPCMHeaderType::htWAVE)
    {
        if (curPos + 8 >= end)
            return NOT_ENOUGH_BUFFER;
        curPos += 4;  // skip 'data' identifier
        const uint32_t dataSize = loadUnaligned<uint32_t>(curPos);
        if (dataSize == 0)
            m_openSizeWaveFormat = true;
        // 0 and 0xFFFFFFFF are written when the size is not known yet, by a stream writer or for
        // a file past 4 GB: the audio then runs to the end of the file.
        setDataChunkSize(dataSize, dataSize != 0 && dataSize != UINT32_MAX);
        curPos += 4;
    }
    else
    {
        if (curPos + sizeof(GUID) + 8 >= end)
            return NOT_ENOUGH_BUFFER;
        curPos += sizeof(GUID);
        // For w64, data length includes data metadata (16 bytes) and size (8 bytes)
        const int64_t dataSize = loadUnaligned<int64_t>(curPos) - 24;
        setDataChunkSize(dataSize, dataSize > 0);
        curPos += 8;
    }
    return static_cast<int>(curPos - buff);
}

void LPCMStreamReader::setDataChunkSize(const int64_t size, const bool known)
{
    m_curChunkLen = size > 0 ? size : 0;
    m_chunkSizeKnown = known && size > 0;
    m_afterData = false;
    if (m_headerType == LPCMHeaderType::htWAVE64)
        m_dataPad = static_cast<int>((8 - (m_curChunkLen + 24) % 8) % 8);
    else
        m_dataPad = static_cast<int>(m_curChunkLen & 1);
}

// The data chunk has ended. A WAV can go on with a chunk of tags (LIST, id3, bext, JUNK and more),
// with another data chunk, or with another whole file joined to it. Each of these used to be read
// as audio: the counter of the data chunk's bytes reaching 0 also meant "size unknown", so a 1 MB
// JUNK chunk after 10 s of audio became 5 s of noise. Returns the bytes to step over now (a pad
// byte or a chunk header), with a chunk's body left in m_skipRest; 0 when audio or a file header
// follows; NOT_ENOUGH_BUFFER when more bytes are needed to tell.
int LPCMStreamReader::stepAfterData(uint8_t* buff, uint8_t* end)
{
    const bool wave64 = m_headerType == LPCMHeaderType::htWAVE64;
    if (m_dataPad > 0)
    {
        // A writer may leave out the pad byte; it is stepped over only where it is 0, as written.
        if (end - buff < m_dataPad)
            return NOT_ENOUGH_BUFFER;
        int n = 0;
        while (n < m_dataPad && buff[n] == 0) ++n;
        m_dataPad = 0;
        return n;
    }
    if (end - buff < 4)
        return NOT_ENOUGH_BUFFER;
    const uint32_t id = loadUnaligned<uint32_t>(buff);
    if (id == RIFF_SMALL || id == RIFF_LARGE)
    {
        m_afterData = false;  // another file: its header is read as at the start
        return 0;
    }
    const int chunkHeaderSize = wave64 ? static_cast<int>(sizeof(GUID)) + 8 : 8;
    if (end - buff < chunkHeaderSize)
        return NOT_ENOUGH_BUFFER;
    if (memcmp(buff, "data", 4) == 0)
    {
        if (wave64)
        {
            const int64_t dataSize = loadUnaligned<int64_t>(buff + sizeof(GUID)) - 24;
            setDataChunkSize(dataSize, dataSize > 0);
        }
        else
        {
            const uint32_t dataSize = loadUnaligned<uint32_t>(buff + 4);
            setDataChunkSize(dataSize, dataSize != 0 && dataSize != UINT32_MAX);
        }
        return chunkHeaderSize;
    }
    // A chunk name is four letters, digits, spaces or underscores. Anything else, or a chunk
    // claiming more than 256 MB, is taken for audio as before: the data size may have been
    // written wrongly, and losing audio is worse than keeping a tag as sound.
    bool name = true;
    for (int i = 0; i < 4; ++i) name = name && (isalnum(buff[i]) || buff[i] == ' ' || buff[i] == '_');
    // The chunk's own size without its pad, which is stepped over like the data chunk's.
    const uint64_t chunkSize = wave64 ? loadUnaligned<uint64_t>(buff + sizeof(GUID))
                                      : 8 + static_cast<uint64_t>(loadUnaligned<uint32_t>(buff + 4));
    if (!name || chunkSize < static_cast<uint64_t>(chunkHeaderSize) || chunkSize > (256ull << 20))
    {
        m_afterData = false;
        m_chunkSizeKnown = false;
        return 0;
    }
    LTRACE(LT_INFO, 2,
           "WAVE: the chunk '" << std::string(reinterpret_cast<char*>(buff), 4) << "' after the audio (" << chunkSize
                               << " bytes) is not audio and is skipped.");
    m_skipRest = static_cast<int64_t>(chunkSize) - chunkHeaderSize;
    m_dataPad = static_cast<int>(wave64 ? (8 - chunkSize % 8) % 8 : chunkSize & 1);
    return chunkHeaderSize;
}

int LPCMStreamReader::decodeFrame(uint8_t* buff, uint8_t* end, int& skipBytes, int& skipBeforeBytes)
{
    skipBeforeBytes = skipBytes = 0;
    if (m_headerType == LPCMHeaderType::htM2TS)
    {
        const int audio_data_payload_size = decodeLPCMHeader(buff);
        if (end - buff < audio_data_payload_size + 4)
            return NOT_ENOUGH_BUFFER;
        if (m_demuxMode)
        {
            const int32_t newSize = convertLPCMToWAV(buff + 4, buff + 4 + audio_data_payload_size);
            skipBytes = audio_data_payload_size - newSize;
            skipBeforeBytes = 4;
            return newSize;
        }
        return 4 + audio_data_payload_size;  // 4 byte header + payload
    }
    if (m_headerType == LPCMHeaderType::htWAVE || m_headerType == LPCMHeaderType::htWAVE64)
    {
        int hdrSize = 0;
        if (end - buff < 4)
            return NOT_ENOUGH_BUFFER;

        const auto curPtr32 = UnalignedPtr<uint32_t>(buff);
        if (m_curChunkLen == 0 && (*curPtr32 == RIFF_SMALL || *curPtr32 == RIFF_LARGE))
        {
            if (end - buff < 8)
                return NOT_ENOUGH_BUFFER;
            if (!m_openSizeWaveFormat || curPtr32[1] == 0)
            {
                hdrSize = decodeWaveHeader(buff, end);
                if (hdrSize == NOT_ENOUGH_BUFFER)
                    return NOT_ENOUGH_BUFFER;
                skipBeforeBytes = hdrSize;
                if (m_needSync)
                {
                    // decode header again. A data size of 0 (open) sets the flag below on this first
                    // read, and the second read then took the header for audio.
                    m_curChunkLen = 0;
                    m_openSizeWaveFormat = false;
                }
            }
        }
        if (m_frameRest == 0)
        {
            if (m_bitsPerSample == 0)
                return 0;  // can't decode frame

            // Assume 5ms frames = 1s / 200
            const int reqFrameLen = (m_bitsPerSample == 20 ? 3 : m_bitsPerSample / 8) * m_channels * m_freq / 200;
            int frameLen = reqFrameLen;
            // a chunk can contain several frames, but a frame canot be bigger than a chunck
            if (m_curChunkLen)
                frameLen = static_cast<int>(FFMIN(reqFrameLen, m_curChunkLen));
            if (end - buff < frameLen + hdrSize)
                return NOT_ENOUGH_BUFFER;
            const int sampleSize = (m_bitsPerSample == 20 ? 3 : m_bitsPerSample / 8) * m_channels;
            m_frameRest = reqFrameLen - (frameLen / sampleSize * sampleSize);
            if (m_frameRest == reqFrameLen)
                m_frameRest = 0;
            else
                m_needPCMHdr = true;
            return frameLen;
        }
        if (end - buff < m_frameRest + hdrSize)
            return NOT_ENOUGH_BUFFER;
        const int frameLen = m_frameRest;
        m_frameRest = 0;
        m_needPCMHdr = false;
        return frameLen;
    }
    return 0;
}

uint8_t* LPCMStreamReader::findFrame(uint8_t* buff, uint8_t* end)
{
    if (m_headerType == LPCMHeaderType::htNone)
        if (!detectLPCMType(buff, end - buff))
            return nullptr;
    return buff;
}

double LPCMStreamReader::getFrameDuration()
{
    if (m_frameRest == 0)
        return 5 * INTERNAL_PTS_FREQ / 1000.0;  // 5 ms frames
    return 0;
}

void LPCMStreamReader::fillDiscoveryData(StreamDiscoveryData& data)
{
    SimplePacketizerReader::fillDiscoveryData(data);  // sampleRate, channels
    data.bitsPerSample = m_bitsPerSample;
}

void LPCMStreamReader::applyDiscoveryData(const StreamDiscoveryData& data)
{
    if (data.sampleRate > 0)
        m_freq = data.sampleRate;
    if (data.channels > 0)
        m_channels = static_cast<uint8_t>(data.channels);
    if (data.bitsPerSample > 0)
        m_bitsPerSample = static_cast<uint16_t>(data.bitsPerSample);
}

const std::string LPCMStreamReader::getStreamInfo()
{
    std::ostringstream str;
    const int mch = m_channels + (m_channels % 2 == 1 ? 1 : 0);
    const uint32_t bitrate = (m_bitsPerSample == 20 ? 24 : m_bitsPerSample) * m_freq * mch;
    str << "Bitrate: " << bitrate / 1000 << "Kbps  ";
    str << "Sample Rate: " << m_freq / 1000 << "KHz  ";
    str << "Channels: ";
    if (m_lfeExists)
        str << static_cast<int>(m_channels) - 1 << ".1";
    else
        str << static_cast<int>(m_channels);
    str << "  Bits per sample: " << m_bitsPerSample << "bit";
    return str.str();
}

void LPCMStreamReader::writePESExtension(PESPacket* pesPacket, const AVPacket& avPacket) {}

int LPCMStreamReader::writeAdditionData(uint8_t* dstBuffer, uint8_t* dstEnd, AVPacket& avPacket,
                                        PriorityDataInfo* priorityData)
{
    uint8_t* curPos = dstBuffer;
    if (m_demuxMode)
    {
        if (m_firstFrame)
        {
            // The RIFF header is 68 bytes and the Wave64 one is 128. The check here used to ask
            // for sizeof(WAVEFORMATPCMEX) + 8, which is 48, so it passed for a buffer that could
            // not hold what was about to be written into it.
            const int64_t headerSize = m_demuxToW64 ? W64_HEADER_SIZE : 68;
            if (dstEnd - dstBuffer < headerSize)
                THROW(ERR_COMMON, "LPCM stream error: Not enough buffer for writing headers")
            if (m_demuxToW64)
            {
                curPos = writeW64Guid(curPos, "riff");
                curPos = writeLE64(curPos, 0);  // the file size, patched when the file closes
                curPos = writeW64Guid(curPos, "wave");
                curPos = writeW64Guid(curPos, "fmt ");
                curPos = writeLE64(curPos, W64_CHUNK_HEADER + sizeof(WAVEFORMATPCMEX));
            }
            else
            {
                // write wave header
                for (const char c : "RIFF\xff\xff\xff\xffWAVEfmt ") *curPos++ = c;
                curPos--;
                const auto fmtSize = UnalignedPtr<uint32_t>(curPos);
                *fmtSize = static_cast<uint32_t>(sizeof(WAVEFORMATPCMEX));
                curPos += 4;
            }
            const auto waveFormatPCMEx = reinterpret_cast<WAVEFORMATPCMEX*>(curPos);
            waveFormatPCMEx->wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            waveFormatPCMEx->nChannels = m_channels;
            waveFormatPCMEx->nSamplesPerSec = m_freq;
            waveFormatPCMEx->nAvgBytesPerSec = m_channels * m_freq * ((m_bitsPerSample + 4) >> 3);
            const uint16_t bitsPerSample = m_bitsPerSample == 20 ? 24 : m_bitsPerSample;
            waveFormatPCMEx->nBlockAlign = m_channels * bitsPerSample / 8;
            waveFormatPCMEx->wBitsPerSample = bitsPerSample;
            waveFormatPCMEx->cbSize = 22;  // After this to GUID
            waveFormatPCMEx->Samples.wValidBitsPerSample = m_bitsPerSample;
            waveFormatPCMEx->dwChannelMask = getWaveChannelMask(m_channels, m_lfeExists, m_backCentre);  // Specify PCM
            waveFormatPCMEx->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
            curPos += sizeof(WAVEFORMATPCMEX);
            if (m_demuxToW64)
            {
                curPos = writeW64Guid(curPos, "data");
                curPos = writeLE64(curPos, 0);  // the data size, patched when the file closes
                assert(curPos - dstBuffer == W64_HEADER_SIZE);
            }
            else
            {
                for (const char c : "data\xff\xff\xff\xff") *curPos++ = c;
                curPos--;
            }
        }
    }
    /*
    if (m_headerType == htM2TS) {
            m_firstFrame = false;
            return curPos - dstBuffer; // frames already has right LPCM headers
    }
    */
    m_firstFrame = false;
    return static_cast<int>(curPos - dstBuffer);
}

void LPCMStreamReader::setHeadersType(const LPCMHeaderType value) { m_headerType = value; }

bool LPCMStreamReader::detectLPCMType(uint8_t* buffer, const int64_t len)
{
    if (len == 0)
        return false;

    if (m_containerDataType == TRACKTYPE_WAV)
    {
        m_headerType = LPCMHeaderType::htWAVE;
        return true;
    }

    const uint8_t* end = buffer + len;
    // 1. test for WAVEHeader
    uint8_t* curPos = buffer;
    if ((curPos[0] == 'R' && curPos[1] == 'I' && curPos[2] == 'F' && curPos[3] == 'F') ||
        (curPos[0] == 'r' && curPos[1] == 'i' && curPos[2] == 'f' && curPos[3] == 'f'))
    {
        m_headerType = LPCMHeaderType::htWAVE;
        const auto testWave64 = reinterpret_cast<GUID*>(curPos);
        if (*testWave64 == WAVE64GUID)
            m_headerType = LPCMHeaderType::htWAVE64;
        return true;
    }

    if (m_testMode && m_containerType != ContainerType::ctLPCM && m_containerDataType != TRACKTYPE_PCM)
        return false;  // LPCM definition has too few bytes. We can't detect LPCM without hints from source container

    // 2. test for M2TS LPCM headers
    curPos = buffer;
    while (curPos < end)
    {
        const uint16_t frameLen = AV_RB16(curPos);
        switch (frameLen)
        {
        case 960:
        case 1440:
        case 1920:
        case 2880:
        case 3840:
        case 4320:
        case 5760:
        case 7680:
        case 8640:
        case 11520:
        case 17280:
            curPos += frameLen + 4;
            break;
        default:
            return false;
        }
    }
    m_headerType = LPCMHeaderType::htM2TS;
    return true;
}

bool LPCMStreamReader::beforeFileCloseEvent(File& file)
{
    file.sync();
    int64_t fileSize = 0;
    file.size(&fileSize);
    if (m_demuxToW64)
    {
        // No ceiling to guard against here, which is the whole point of writing this form: both
        // sizes are 64 bit. The data chunk's size includes its own 24 byte header, so it is the
        // audio plus 24, not the audio.
        uint8_t buf[8];
        if (file.seek(W64_RIFF_SIZE_OFFSET, File::SeekMethod::smBegin) == -1)
            return false;
        writeLE64(buf, static_cast<uint64_t>(fileSize));
        if (file.write(buf, 8) != 8)
            return false;
        if (file.seek(W64_DATA_SIZE_OFFSET, File::SeekMethod::smBegin) == -1)
            return false;
        writeLE64(buf, static_cast<uint64_t>(fileSize - W64_HEADER_SIZE + W64_CHUNK_HEADER));
        if (file.write(buf, 8) != 8)
            return false;
        return true;
    }
    if (fileSize <= UINT_MAX)
    {
        uint32_t dataSize = static_cast<uint32_t>(fileSize) - 8;
        if (file.seek(4, File::SeekMethod::smBegin) == -1)
            return false;
        if (file.write(&dataSize, 4) != 4)
            return false;
        if (file.seek(64, File::SeekMethod::smBegin) == -1)
            return false;
        dataSize = static_cast<uint32_t>(fileSize) - 68;
        if (file.write(&dataSize, 4) != 4)
            return false;
    }
    return true;
}

int LPCMStreamReader::readPacket(AVPacket& avPacket)
{
    applyTimeGaps();           // as in the base class
    avPacket.flags = m_flags;  // + AVPacket::IS_COMPLETE_FRAME | AVPacket::FORCE_NEW_FRAME;
    avPacket.stream_index = m_streamIndex;
    avPacket.codecID = getCodecInfo().codecID;
    avPacket.codec = this;
    avPacket.data = nullptr;
    avPacket.size = 0;
    avPacket.duration = 0;
    avPacket.dts = avPacket.pts = static_cast<int64_t>(m_curPts * m_stretch) + m_timeOffset;
    assert(m_curPos <= m_bufEnd);
    if (m_curPos == m_bufEnd)
        return NEED_MORE_DATA;
    int skipBytes = 0;
    int skipBeforeBytes = 0;
    if (m_needSync)
    {
        uint8_t* frame = findFrame(m_curPos, m_bufEnd);
        if (frame == nullptr)
        {
            m_processedBytes += m_bufEnd - m_curPos;
            // The same accounting as the base class. THIS FUNCTION IS A COPY OF IT, so anything
            // added there has to be added here too or this codec silently reports nothing.
            //
            // IN PRACTICE THESE COUNTERS CANNOT FIRE FOR LPCM, and the reason is worth stating
            // correctly because an earlier version of this comment stated it wrongly twice.
            // findFrame CAN return nullptr, at line 614, when m_headerType is htNone and
            // detectLPCMType fails; decodeFrame CAN answer "this is not a frame", returning 0 at
            // line 586 when m_bitsPerSample is 0. Both happen: asking for A_LPCM on a TrueHD or
            // DTS track gives a 0 byte file by exactly that route.
            //
            // WHAT ACTUALLY KEEPS THESE AT ZERO IS m_everSynced. On every path that fails, no
            // frame is ever decoded, so m_everSynced is never set and both guards below are false.
            // And once a real PCM stream HAS synced it cannot desync, because there is no sync
            // word to lose: corrupting the samples changes what it sounds like, not whether it
            // parses. Measured, a stream with 3,000 random bytes altered demuxes to exactly the
            // same byte count as the original and reports nothing, correctly.
            //
            // They are written anyway so that this copy does not silently diverge from the
            // original, and so that a future change which CAN reject PCM data is counted.
            // ** DO NOT READ A SILENT LPCM RESULT AS EVIDENCE THAT THE REPORT WORKS. **
            if (m_everSynced)
                m_pendingLost += m_bufEnd - m_curPos;
            return NEED_MORE_DATA;
        }
        const int decodeRez = decodeFrame(frame, m_bufEnd, skipBytes, skipBeforeBytes);
        if (decodeRez == NOT_ENOUGH_BUFFER)
        {
            memmove(m_tmpBuffer.data(), m_curPos, m_bufEnd - m_curPos);
            m_tmpBufferLen = m_bufEnd - m_curPos;
            m_curPos = m_bufEnd;
            return NEED_MORE_DATA;
        }
        if (decodeRez + skipBytes + skipBeforeBytes <= 0)
        {
            m_curPos++;
            m_processedBytes++;
            if (m_everSynced)
                m_lostBytes++;
            return 0;
        }
        m_processedBytes += frame - m_curPos;
        // Before the first frame these are the leading fragment of a stream that began mid frame,
        // which is not a loss. After it they are data being abandoned, except for any part of them
        // that is a recognised tag, which is skipped on purpose wherever it sits. A frame having
        // been found, anything held back earlier was a hole rather than the tail, so it is promoted.
        if (m_everSynced)
            m_lostBytes += frame - m_curPos;
        else
            m_everSynced = true;
        m_lostBytes += m_pendingLost;
        m_pendingLost = 0;
        LTRACE(LT_INFO, 2,
               "Decoding " << getCodecInfo().displayName << " stream (track " << m_streamIndex
                           << "): " << getStreamInfo());
        m_curPos = frame;
        m_needSync = false;
    }
    avPacket.dts = avPacket.pts = llround(m_curPts * m_stretch) + m_timeOffset;
    while (m_skipRest > 0 || m_afterData)
    {
        if (m_skipRest > 0)
        {
            const int64_t n = FFMIN(m_skipRest, m_bufEnd - m_curPos);
            m_curPos += n;
            m_processedBytes += n;
            m_skipRest -= n;
            if (m_curPos == m_bufEnd)
                return NEED_MORE_DATA;
            continue;
        }
        const int n = stepAfterData(m_curPos, m_bufEnd);
        if (n == NOT_ENOUGH_BUFFER)
        {
            memmove(m_tmpBuffer.data(), m_curPos, m_bufEnd - m_curPos);
            m_tmpBufferLen = m_bufEnd - m_curPos;
            m_curPos = m_bufEnd;
            return NEED_MORE_DATA;
        }
        m_curPos += n;
        m_processedBytes += n;
        if (n == 0 && m_skipRest == 0 && !m_afterData)
            break;
    }
    if (m_curPos == m_bufEnd)
        return NEED_MORE_DATA;
    if (m_bufEnd - m_curPos < getHeaderLen())
    {
        memmove(m_tmpBuffer.data(), m_curPos, m_bufEnd - m_curPos);
        m_tmpBufferLen = m_bufEnd - m_curPos;
        m_curPos = m_bufEnd;
        return NEED_MORE_DATA;
    }
    skipBytes = 0;
    skipBeforeBytes = 0;
    const int frameLen = decodeFrame(m_curPos, m_bufEnd, skipBytes, skipBeforeBytes);
    if (frameLen == NOT_ENOUGH_BUFFER)
    {
        memmove(m_tmpBuffer.data(), m_curPos, m_bufEnd - m_curPos);
        m_tmpBufferLen = m_bufEnd - m_curPos;
        m_curPos = m_bufEnd;
        return NEED_MORE_DATA;
    }
    if (frameLen + skipBytes + skipBeforeBytes <= 0)
    {
        LTRACE(LT_INFO, 2, getCodecInfo().displayName << " bad frame detected. Resync stream.");
        m_needSync = true;
        return 0;
    }
    if (m_bufEnd - m_curPos < frameLen + skipBytes + skipBeforeBytes)
    {
        memmove(m_tmpBuffer.data(), m_curPos, m_bufEnd - m_curPos);
        m_tmpBufferLen = m_bufEnd - m_curPos;
        m_curPos = m_bufEnd;
        return NEED_MORE_DATA;
    }
    avPacket.data = m_curPos;
    avPacket.data += skipBeforeBytes;
    if (m_curChunkLen)
    {
        m_curChunkLen -= frameLen;
        if (m_curChunkLen == 0 && m_chunkSizeKnown)
            m_afterData = true;
    }
    if (frameLen > MAX_AV_PACKET_SIZE)
        THROW(ERR_AV_FRAME_TOO_LARGE, "AV frame too large (" << frameLen << " bytes). Increase AV buffer.")
    avPacket.size = frameLen;
    avPacket.duration = static_cast<int64_t>(getFrameDuration());  // m_ptsIncPerFrame;
    m_curPts += static_cast<double>(avPacket.duration);
    doMplsCorrection();
    if ((m_headerType == LPCMHeaderType::htWAVE || m_headerType == LPCMHeaderType::htWAVE64) && !m_demuxMode)
    {
        avPacket.size = convertWavToPCM(avPacket.data, avPacket.data + avPacket.size);
        avPacket.data = m_tmpFrameBuffer;
    }

    m_frameNum++;
    if (m_frameNum % 1000 == 0)
        LTRACE(LT_DEBUG, 0, "Processed " << m_frameNum << " " << getCodecInfo().displayName << " frames");
    m_curPos += frameLen + skipBytes + skipBeforeBytes;
    m_processedBytes += frameLen + skipBytes + skipBeforeBytes;
    if (m_frameRest == 0)
        avPacket.flags = m_flags + (AVPacket::IS_COMPLETE_FRAME | AVPacket::FORCE_NEW_FRAME);
    return 0;
}

int LPCMStreamReader::flushPacket(AVPacket& avPacket)
{
    avPacket.duration = 0;
    avPacket.data = nullptr;
    avPacket.size = 0;
    avPacket.stream_index = m_streamIndex;
    avPacket.flags = m_flags + AVPacket::IS_COMPLETE_FRAME;
    avPacket.codecID = getCodecInfo().codecID;
    avPacket.codec = this;
    int skipBytes = 0;
    int skipBeforeBytes = 0;
    if (m_afterData || m_skipRest > 0)
    {
        // What is left after the end of the data chunk is a part of a chunk, not audio.
        m_processedBytes += m_tmpBufferLen;
        m_tmpBufferLen = 0;
    }
    if (m_tmpBufferLen >= getHeaderLen())
    {
        const int size =
            decodeFrame(m_tmpBuffer.data(), m_tmpBuffer.data() + m_tmpBufferLen, skipBytes, skipBeforeBytes);
        if (size + skipBytes + skipBeforeBytes <= 0 && size != NOT_ENOUGH_BUFFER)
            return 0;
    }
    avPacket.dts = avPacket.pts = static_cast<int64_t>(m_curPts * m_stretch) + m_timeOffset;
    if (!m_tmpBuffer.empty())
        avPacket.data = m_tmpBuffer.data();
    else
        avPacket.data = nullptr;
    avPacket.data += skipBeforeBytes;
    if (m_tmpBufferLen > 0)
    {
        avPacket.size = static_cast<int>(m_tmpBufferLen);
    }
    if ((m_headerType == LPCMHeaderType::htWAVE || m_headerType == LPCMHeaderType::htWAVE64) && !m_demuxMode)
    {
        if (m_frameRest > 0)
            m_needPCMHdr = false;
        if (avPacket.size > 0)
            avPacket.size = convertWavToPCM(avPacket.data, avPacket.data + avPacket.size);
        avPacket.data = m_tmpFrameBuffer;
    }

    if (m_frameRest > 0 && !m_demuxMode)
    {
        const int32_t samplesRest = m_frameRest / m_channels;
        m_frameRest = samplesRest * ((bdChannels() + 1) & 0xfe);
        m_frameRest -= avPacket.size - (m_needPCMHdr ? 4 : 0);
        if (avPacket.data != nullptr && m_frameRest > 0)
        {
            memset(avPacket.data + avPacket.size, 0, m_frameRest);
            avPacket.size += m_frameRest;
            m_processedBytes -= m_frameRest;
        }
    }
    LTRACE(LT_DEBUG, 0, "Processed " << m_frameNum << " " << getCodecInfo().displayName << " frames");
    m_processedBytes += avPacket.size + skipBytes + skipBeforeBytes;
    return avPacket.size;
}

const CodecInfo& LPCMStreamReader::getCodecInfo() { return lpcmCodecInfo; }

void LPCMStreamReader::setBuffer(uint8_t* data, const uint32_t dataLen, const bool lastBlock)
{
    m_lastChannelRemapPos = nullptr;
    SimplePacketizerReader::setBuffer(data, dataLen, lastBlock);
}
