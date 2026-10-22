#include "dtsStreamReader.h"

#include <bit>
#include <cmath>
#include <sstream>

#include "pgsStreamReader.h"
#include "vodCoreException.h"

// static constexpr int DCA_EXT_CORE = 0x001;       ///< core in core substream
static constexpr int DCA_EXT_XXCH = 0x002;       ///< XXCh channels extension in core substream
static constexpr int DCA_EXT_X96 = 0x004;        ///< 96/24 extension in core substream
static constexpr int DCA_EXT_XCH = 0x008;        ///< XCh channel extension in core substream
static constexpr int DCA_EXT_EXSS_CORE = 0x010;  ///< core in ExSS (extension substream)
static constexpr int DCA_EXT_EXSS_XBR = 0x020;   ///< extended bitrate extension in ExSS
static constexpr int DCA_EXT_EXSS_XXCH = 0x040;  ///< XXCh channels extension in ExSS
static constexpr int DCA_EXT_EXSS_X96 = 0x080;   ///< 96/24 extension in ExSS
static constexpr int DCA_EXT_EXSS_LBR = 0x100;   ///< low bitrate component in ExSS
static constexpr int DCA_EXT_EXSS_XLL = 0x200;   ///< lossless extension in ExSS
static constexpr int DCA_EXT_EXSS_RSV1 = 0x400;  ///< reserved, size only
static constexpr int DCA_EXT_EXSS_RSV2 = 0x800;  ///< reserved, size only

/// Extension components appear inside an asset in this order.
static constexpr int DCA_EXSS_COMPONENTS[] = {DCA_EXT_EXSS_CORE, DCA_EXT_EXSS_XBR, DCA_EXT_EXSS_XXCH,
                                              DCA_EXT_EXSS_X96,  DCA_EXT_EXSS_LBR, DCA_EXT_EXSS_XLL};

static constexpr int dca_ext_audio_descr_mask[] = {
    DCA_EXT_XCH, -1, DCA_EXT_X96, DCA_EXT_XCH | DCA_EXT_X96, -1, -1, DCA_EXT_XXCH, -1,
};

static constexpr int ppi_dts_samplerate[] = {
    0, 8000, 16000, 32000, 0, 0, 11025, 22050, 44100, 0, 0, 12000, 24000, 48000, 96000, 192000,
};

static constexpr int dtshd_samplerate[] = {
    0x1F40,  0x3E80,  0x7D00, 0x0FA00, 0x1F400, 0x5622,  0x0AC44, 0x15888,
    0x2B110, 0x56220, 0x2EE0, 0x5DC0,  0x0BB80, 0x17700, 0x2EE00, 0x5DC00,
};

static constexpr int ppi_dts_bitrate[] = {
    32000,   56000,   64000,   96000,   112000,  128000,  192000,  224000,     256000,         320000,         384000,
    448000,  512000,  576000,  640000,  768000,  896000,  1024000, 1152000,    1280000,        1344000,        1408000,
    1411200, 1472000, 1536000, 1920000, 2048000, 3072000, 3840000, 1 /*open*/, 2 /*variable*/, 3 /*lossless*/,
};

static constexpr int64_t AUPR_HDR = 0x415550522D484452ll;
static constexpr int64_t AUPRINFO = 0x41555052494E464Fll;
static constexpr int64_t BITSHVTB = 0x4249545348565442ll;
static constexpr int64_t BLACKOUT = 0x424C41434B4F5554ll;
static constexpr int64_t BRANCHPT = 0x4252414E43485054ll;
static constexpr int64_t BUILDVER = 0x4255494C44564552ll;
static constexpr int64_t CORESSMD = 0x434F524553534D44ll;
static constexpr int64_t DTSHDHDR = 0x4454534844484452ll;
static constexpr int64_t EXTSS_MD = 0x45585453535f4d44ll;
static constexpr int64_t FILEINFO = 0x46494C45494E464Fll;
static constexpr int64_t NAVI_TBL = 0x4E4156492D54424Cll;
static constexpr int64_t STRMDATA = 0x5354524D44415441ll;
static constexpr int64_t TIMECODE = 0x54494D45434F4445ll;

static constexpr int AOUT_CHAN_CENTER = 0x1;
static constexpr int AOUT_CHAN_LEFT = 0x2;
static constexpr int AOUT_CHAN_RIGHT = 0x4;
static constexpr int AOUT_CHAN_REARCENTER = 0x10;
static constexpr int AOUT_CHAN_REARLEFT = 0x20;
static constexpr int AOUT_CHAN_REARRIGHT = 0x40;
static constexpr int AOUT_CHAN_MIDDLELEFT = 0x100;
static constexpr int AOUT_CHAN_MIDDLERIGHT = 0x200;
static constexpr int AOUT_CHAN_LFE = 0x1000;
// static constexpr int AOUT_CHAN_DOLBYSTEREO = 0x10000;
static constexpr int AOUT_CHAN_DUALMONO = 0x20000;
// static constexpr int AOUT_CHAN_REVERSESTEREO = 0x40000;

using namespace std;

int DTSStreamReader::getTSDescriptor(uint8_t* dstBuff, bool blurayMode, const bool hdmvDescriptors)
{
    uint8_t* frame = findFrame(m_buffer, m_bufEnd);
    if (frame == nullptr)
        return 0;
    int skipBytes = 0;
    int skipBeforeBytes = 0;
    const int len = decodeFrame(frame, m_bufEnd, skipBytes, skipBeforeBytes);
    if (len < 1)
        return 0;

    m_state = DTSDecodeState::stDecodeDTS;

    // no DTS descriptor in Blu-ray
    if (hdmvDescriptors)
        return 0;

    // ETSI TS 101 154 F.4.2 DTS registration descriptor
    *dstBuff++ = static_cast<int>(TSDescriptorTag::REGISTRATION);  // descriptor tag
    *dstBuff++ = 4;                                                // descriptor length
    *dstBuff++ = 'D';
    *dstBuff++ = 'T';
    *dstBuff++ = 'S';

    switch (pi_frame_length)
    {
    case 512:
        *dstBuff++ = '1';
        break;
    case 1024:
        *dstBuff++ = '2';
        break;
    case 2048:
        *dstBuff++ = '3';
        break;
    default:
        *dstBuff++ = ' ';
    }

    // ETSI TS 101 154 F.4.3 DTS audio descriptor
    BitStreamWriter bitWriter{};

    *dstBuff++ = static_cast<uint8_t>(TSDescriptorTag::DTS);  // descriptor tag;
    *dstBuff++ = 5;                                           // descriptor length
    bitWriter.setBuffer(dstBuff, dstBuff + 5);
    bitWriter.putBits(4, pi_sample_rate_index);
    bitWriter.putBits(6, pi_bit_rate_index);
    bitWriter.putBits(7, nblks);
    bitWriter.putBits(14, len);
    bitWriter.putBits(6, getSurroundModeCode());
    bitWriter.putBits(1, (pi_channels_conf & AOUT_CHAN_LFE) ? 1 : 0);
    bitWriter.putBits(2, m_dtsEsChannels ? 1 : 0);  // DTS-EX flag
    bitWriter.flushBits();

    return 13;  // descriptor length
}

void DTSStreamReader::writePESExtension(PESPacket* pesPacket, const AVPacket& avPacket)
{
    // 0f 81 71 - from blu ray DTS-HD  ( can use 0x01 instead 0x0f. bits 1-3 are reserved.)
    // 0x01 0x81 0x71 - ordinal DTS == 0x71?
    if (m_useNewStyleAudioPES)
    {
        pesPacket->flagsLo |= 1;  // enable PES extension for DTS stream
        uint8_t* data = reinterpret_cast<uint8_t*>(pesPacket) + pesPacket->getHeaderLength();
        *data++ = 1;     // PES_extension_flag_2
        *data++ = 0x81;  // marker bit + extension2 len
        if (m_state == DTSDecodeState::stDecodeHD2 || !m_isCoreExists)
            *data = 0x72;  // stream id extension. 71 = DTS frame, 72 HD frame
        else
            *data = 0x71;  // stream id extension
        pesPacket->m_pesHeaderLen += 3;
    }
}

int DTSStreamReader::getSurroundModeCode() const
{
    int rez = 0;
    if ((pi_channels_conf & AOUT_CHAN_LEFT) && (pi_channels_conf & AOUT_CHAN_RIGHT) &&
        (pi_channels_conf & AOUT_CHAN_CENTER) && (pi_channels_conf & AOUT_CHAN_REARLEFT) &&
        (pi_channels_conf & AOUT_CHAN_REARRIGHT))
        rez = 0x09;
    else if ((pi_channels_conf & AOUT_CHAN_LEFT) && (pi_channels_conf & AOUT_CHAN_RIGHT) &&
             (pi_channels_conf & AOUT_CHAN_REARLEFT) && (pi_channels_conf & AOUT_CHAN_REARRIGHT))
        rez = 0x08;
    else if ((pi_channels_conf & AOUT_CHAN_LEFT) && (pi_channels_conf & AOUT_CHAN_RIGHT) &&
             (pi_channels_conf & AOUT_CHAN_CENTER) &&
             ((pi_channels_conf & AOUT_CHAN_REARLEFT) || (pi_channels_conf & AOUT_CHAN_REARRIGHT) ||
              (pi_channels_conf & AOUT_CHAN_REARCENTER)))
        rez = 0x07;
    else if ((pi_channels_conf & AOUT_CHAN_LEFT) && (pi_channels_conf & AOUT_CHAN_RIGHT) &&
             ((pi_channels_conf & AOUT_CHAN_REARLEFT) || (pi_channels_conf & AOUT_CHAN_REARRIGHT) ||
              (pi_channels_conf & AOUT_CHAN_REARCENTER)))
        rez = 0x06;
    else if ((pi_channels_conf & AOUT_CHAN_LEFT) && (pi_channels_conf & AOUT_CHAN_RIGHT) &&
             (pi_channels_conf & AOUT_CHAN_CENTER))
        rez = 0x05;
    else if ((pi_channels_conf & AOUT_CHAN_LEFT) && (pi_channels_conf & AOUT_CHAN_RIGHT))
        rez = 0x02;
    return rez;
}

// An empty file gives no data at all: buff and end were null, end - 4 went round to the top of the address space and
// the first header was read at address 0 (a crash). The magic takes 4 bytes, the header with its frame size 10.
void DTSStreamReader::checkIfOnlyHDDataExists(uint8_t* buff, const uint8_t* end)
{
    for (int i = 0; i < 2 && buff != nullptr && end - buff > 4; ++i)
    {
        const bool isHDData = loadUnaligned<uint32_t>(buff) == my_htonl(DTS_HD_PREFIX);
        if (!isHDData)
            return;
        if (end - buff < 10)
            break;

        BitStreamReader reader{};
        reader.setBuffer(buff + 5, end);  // skip 4 byte magic and 1 unknown byte
        int hdFrameSize;
        reader.skipBits(2);  // nuSubStreamIndex
        if (reader.getBit())
        {
            reader.skipBits(12);  // headerSize
            hdFrameSize = reader.getBits<int32_t>(20) + 1;
        }
        else
        {
            reader.skipBits(8);  // headerSize
            hdFrameSize = reader.getBits<uint16_t>(16) + 1;
        }
        buff += hdFrameSize;
    }
    m_isCoreExists = false;
}

uint8_t* DTSStreamReader::findFrame(uint8_t* buff, uint8_t* end)
{
    // An empty file gives no data at all, buff and end null: the scans below, to end - 4, went round to the top of the
    // address space and read address 0 (a crash). A frame takes more than 4 bytes.
    if (buff == nullptr || end - buff <= 4)
        return nullptr;
    // check for DTS-HD headers
    while (end - buff >= 16)
    {
        const auto ptr = UnalignedPtr<int64_t>(buff);
        const uint64_t hdrType = my_ntohll(ptr[0]);
        const uint64_t hdrSize = my_ntohll(ptr[1]) + 16;

        if (hdrSize > static_cast<uint64_t>(1) << 61)
            break;

        if (hdrType == AUPRINFO || hdrType == BITSHVTB || hdrType == BLACKOUT || hdrType == BRANCHPT ||
            hdrType == BUILDVER || hdrType == CORESSMD || hdrType == EXTSS_MD || hdrType == FILEINFO ||
            hdrType == NAVI_TBL || hdrType == TIMECODE || hdrType == DTSHDHDR)
        {
            if (hdrSize > static_cast<size_t>(end - buff))
                return nullptr;  // need more data
            buff += hdrSize;
        }
        else if (hdrType == AUPR_HDR)
        {
            if (buff + hdrSize > end)
                return nullptr;  // need more data
            // determine skipping frames amount
            m_skippingSamples = (buff[35] << 8) + buff[36];
            buff += hdrSize;
        }
        else if (hdrType == STRMDATA)
        {
            // m_dataSegmentLen = hdrSize;
            buff += 16;
            break;
        }
        else
        {
            break;  // no more HD headers
        }
    }

    if (m_firstCall)
    {
        m_firstCall = false;
        checkIfOnlyHDDataExists(buff, end);
    }

    if (!m_isCoreExists)
    {
        // if not core exists find HD frames directly
        for (uint8_t* p_buf = buff; p_buf < end - 4; p_buf++)
        {
            if (p_buf[0] == 0x64 && p_buf[1] == 0x58 && p_buf[2] == 0x20 && p_buf[3] == 0x25)
            {
                return p_buf;
            }
        }
        return nullptr;
    }

    for (uint8_t* p_buf = buff; p_buf < end - 4; p_buf++)
    {
        if (p_buf < end - 6)
        {
            if (p_buf[0] == 0xff && p_buf[1] == 0x1f && p_buf[2] == 0x00 && p_buf[3] == 0xe8 &&
                (p_buf[4] & 0xf0) == 0xf0 && p_buf[5] == 0x07)
            {
                return p_buf;
            }
            // 14 bits, big endian version of the bitstream
            if (p_buf[0] == 0x1f && p_buf[1] == 0xff && p_buf[2] == 0xe8 && p_buf[3] == 0x00 && p_buf[4] == 0x07 &&
                (p_buf[5] & 0xf0) == 0xf0)
            {
                return p_buf;
            }
        }
        // 16 bits, big endian version of the bitstream
        if (p_buf[0] == 0x7f && p_buf[1] == 0xfe && p_buf[2] == 0x80 && p_buf[3] == 0x01)
        {
            return p_buf;
        }
        // 16 bits, little endian version of the bitstream
        if (p_buf[0] == 0xfe && p_buf[1] == 0x7f && p_buf[2] == 0x01 && p_buf[3] == 0x80)
        {
            return p_buf;
        }
    }
    return nullptr;
}

int DTSStreamReader::decodeHdInfo(uint8_t* buff, const uint8_t* end)
{
    try
    {
        BitStreamReader reader{};
        reader.setBuffer(buff + 5, end);  // skip 4 byte magic and 1 unknown byte
        int headerSize;
        int hdFrameSize;
        const auto nuSubStreamIndex = reader.getBits<uint8_t>(2);
        const bool isBlownUpHeader = reader.getBit();
        if (isBlownUpHeader)
        {
            headerSize = reader.getBits<uint16_t>(12) + 1;
            hdFrameSize = reader.getBits<int32_t>(20) + 1;
        }
        else
        {
            headerSize = reader.getBits<uint8_t>(8) + 1;
            hdFrameSize = reader.getBits<uint16_t>(16) + 1;
        }
        if (m_hdType == DTSHD_SUBTYPE::DTS_SUBTYPE_UNINITIALIZED)
        {
            if (buff + headerSize + 4 > end)
                return NOT_ENOUGH_BUFFER;
            const auto hdAudioData = UnalignedPtr<uint32_t>(buff + headerSize);
            switch (my_ntohl(*hdAudioData))
            {
            case 0x41A29547:  // XLL
                m_hdType = DTSHD_SUBTYPE::DTS_SUBTYPE_MASTER_AUDIO;
                break;
            case 0x655E315E:  // XBR
                m_hdType = DTSHD_SUBTYPE::DTS_SUBTYPE_HIGH_RES;
                break;
            case 0x0A801921:  // LBR
                m_hdType = DTSHD_SUBTYPE::DTS_SUBTYPE_EXPRESS;
                break;
            case 0x47004A03:  // XXCH
            case 0x5A5A5A5A:  // XCH
                m_hdType = DTSHD_SUBTYPE::DTS_SUBTYPE_EX;
                break;
            case 0x1D95F262:  // X96
                m_hdType = DTSHD_SUBTYPE::DTS_SUBTYPE_96;
                break;
            default:  // Core, Substream, Neo, Sensation...
                m_hdType = DTSHD_SUBTYPE::DTS_SUBTYPE_OTHER;
            }
        }
        else
            return hdFrameSize;

        uint8_t nuNumAssets = 1;
        const bool bStaticFieldsPresent = reader.getBit();
        if (bStaticFieldsPresent)
        {
            reader.skipBits(2);  // nuRefClockCode
            const auto nuExSSFrameDurationCode = reader.getBits<uint8_t>(3) + 1;
            if (pi_frame_length == 0)
                pi_frame_length = nuExSSFrameDurationCode << 9;

            if (reader.getBit())
            {
                reader.skipBits(18);  // nuTimeStamp, 18 high bits
                reader.skipBits(18);  // nuTimeStamp, 18 low bits
            }
            const auto nuNumAudioPresent = reader.getBits<uint8_t>(3) + 1;
            nuNumAssets = reader.getBits<uint8_t>(3) + 1;
            for (int i = 0; i < nuNumAudioPresent; i++) reader.skipBits(nuSubStreamIndex + 1);
            for (int i = 0; i < nuNumAudioPresent; i++)
            {
                for (int j = 0; j < nuSubStreamIndex + 1; j++)
                {
                    if ((j + 1) % 2)
                        reader.skipBits(8);
                }
            }
            if (reader.getBit())
            {
                reader.skipBits(2);  // nuMixMetadataAdjLevel
                const auto nuBits4MixOutMask = reader.getBits<uint8_t>(2) * 4 + 4;
                const auto nuNumMixOutConfigs = reader.getBits<uint8_t>(2) + 1;
                for (int i = 0; i < nuNumMixOutConfigs; i++) reader.skipBits(nuBits4MixOutMask);
            }
        }
        for (int i = 0; i < nuNumAssets; i++) reader.skipBits(isBlownUpHeader ? 20 : 16);

        // Parse only the first asset descriptor; scalar fields (channels, sample rate, etc.)
        // would simply be overwritten by subsequent assets, so only the first is meaningful.
        if (nuNumAssets > 0)
        {
            reader.skipBits(12);  // nuAssetDescriptorFSIZE - 1, DescriptorDataForAssetIndex
            if (bStaticFieldsPresent)
            {
                if (reader.getBit())     // AssetTypeDescrPresent
                    reader.skipBits(4);  // AssetTypeDescriptor

                if (reader.getBit())      // LanguageDescrPresent
                    reader.skipBits(24);  // LanguageDescriptor

                if (reader.getBit())  // bInfoTextPresent
                {
                    const auto nuInfoTextByteSize = reader.getBits<uint16_t>(10) + 1;
                    for (int j = 0; j < nuInfoTextByteSize; j++) reader.skipBits(8);
                }
                const auto nuBitResolution = reader.getBits<uint8_t>(5) + 1;
                const auto nuMaxSampleRate = reader.getBits<uint8_t>(4);
                hd_pi_channels = reader.getBits<uint8_t>(8) + 1;
                uint16_t nuSpkrActivityMask = 0;
                if (reader.getBit())  // bOne2OneMapChannels2Speakers
                {
                    if (hd_pi_channels > 2)
                        reader.skipBit();  // bEmbeddedStereoFlag

                    if (hd_pi_channels > 6)
                        reader.skipBit();  // bEmbeddedSixChFlag

                    if (reader.getBit())  // bSpkrMaskEnabled
                    {
                        const auto nuNumBits4SAMask = reader.getBits<uint8_t>(2) * 4 + 4;
                        nuSpkrActivityMask = reader.getBits<uint16_t>(nuNumBits4SAMask);
                    }
                }
                hd_pi_sample_rate = dtshd_samplerate[nuMaxSampleRate];
                hd_bitDepth = nuBitResolution;

                if (!m_isCoreExists)
                    m_frameDuration = static_cast<double>(pi_frame_length) * INTERNAL_PTS_FREQ / hd_pi_sample_rate;

                if (m_hdType != DTSHD_SUBTYPE::DTS_SUBTYPE_MASTER_AUDIO)
                    m_hdBitrate = lround(static_cast<double>(hd_pi_sample_rate) / pi_frame_length * hdFrameSize * 8);

                hd_pi_lfeCnt = 0;

                if ((nuSpkrActivityMask & 0x8) == 0x8)
                    ++hd_pi_lfeCnt;

                if ((nuSpkrActivityMask & 0x1000) == 0x1000)
                    ++hd_pi_lfeCnt;
            }
        }
        return hdFrameSize;
    }
    catch (BitStreamException&)
    {
        return NOT_ENOUGH_BUFFER;
    }
}

// ---------------------------------------------------------------------------
// DTS:X detection.
//
// Nothing in the extension substream header says "DTS:X". The marker is a 32
// bit sync word sitting at the very end of the XLL band data, on the first 32
// bit boundary past it, and the only way to find it is to walk the XLL frame
// far enough to know where the band data stops.
//
// 0x02000850 is the plain DTS:X marker. 0xF14000Dx is the IMAX Enhanced
// variant, where the low nibble is not part of the sync word, so comparing all
// 32 bits against 0xF14000D0 misses real streams, which carry 0xF14000D3.
//
// Measured on a pressed disc carrying a DTS:X 7.1 track and a plain DTS-HD MA
// 5.1 track side by side, three DTS:X trailers, and a second pressed disc
// carrying all four DTS flavours (MA 7.1, MA 5.1, HR 5.1 and Express): 99,883
// DTS:X frames carry a marker on 100% of frames and 74,881 plain MA frames
// carry nothing in that slot. It is the position that carries the meaning and
// not the value alone, since the same word placed anywhere else in the frame
// means nothing, which is why this navigates to the slot rather than scanning
// the frame for a value.
//
// The rule only ever badges on a marker it navigated to, so a stream it cannot
// walk stays unbadged rather than being badged wrongly.
// ---------------------------------------------------------------------------

static constexpr uint32_t DTS_SYNC_XLL = 0x41A29547;
static constexpr uint32_t DTSX_MARKER = 0x02000850;
static constexpr uint32_t DTSX_IMAX_MARKER = 0xF14000D0;
static constexpr uint32_t DTSX_IMAX_MASK = 0xFFFFFFF0;

/// Extension substream frames to examine before giving up on the badge.
static constexpr int DTSX_PROBE_FRAMES = 16;
/// Markers required before the badge is shown, so one stray word cannot do it.
static constexpr int DTSX_MIN_HITS = 2;

struct DtsExssHeader
{
    int headerSize = 0;
    int frameSize = 0;
    unsigned sizeNBits = 0;
    bool staticFieldsPresent = false;
    bool mixMetadataEnabled = false;
    int nMixOutConfigs = 0;
    int nMixOutChs[4] = {};
    int nAssets = 0;
};

struct DtsExssAsset
{
    int offset = 0;  ///< byte offset of the asset inside the extension substream frame
    int size = 0;
    int extMask = 0;         ///< which extension components this asset carries
    int xllOffset = 0;       ///< byte offset of the XLL component inside the frame
    int xllSize = 0;         ///< byte size of the XLL component
    int xllDelayFrames = 0;  ///< non zero means the XLL frame began in an earlier frame
};

// CRC-16-CCITT, polynomial 0x1021 seeded with 0xffff. DTS stores the CRC right
// after the data it covers, so a correct block leaves a residue of 0.
// iso_writer.cpp has a table driven crc16 but it is seeded with 0 for UDF, so
// it cannot be reused here.
static uint16_t dtsCrc16(const uint8_t* data, const int len)
{
    uint16_t crc = 0xffff;
    for (int i = 0; i < len; ++i)
    {
        crc ^= static_cast<uint16_t>(static_cast<uint16_t>(data[i]) << 8);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
    }
    return crc;
}

// BitStreamReader::skipBits() takes at most 32 bits at a time, and a descriptor
// can carry an information text field of up to 1024 bytes.
static void skipBitsLong(BitStreamReader& reader, int64_t bits)
{
    while (bits > 0)
    {
        const auto step = static_cast<unsigned>(FFMIN(bits, 32));
        reader.skipBits(step);
        bits -= step;
    }
}

static void seekToBit(BitStreamReader& reader, const int bitPos)
{
    const int delta = bitPos - reader.getBitsCount();
    if (delta < 0)
        THROW_BITSTREAM_ERR;
    skipBitsLong(reader, delta);
}

static void alignToByte(BitStreamReader& reader)
{
    const int rest = reader.getBitsCount() & 7;
    if (rest)
        reader.skipBits(8 - rest);
}

static int parseLbrParameters(BitStreamReader& reader)
{
    const int size = static_cast<int>(reader.getBits(14)) + 1;
    if (reader.getBit())
        reader.skipBits(2);  // sample rate and band limit info
    return size;
}

static int parseXllParameters(BitStreamReader& reader, const DtsExssHeader& h, DtsExssAsset& a)
{
    const int size = static_cast<int>(reader.getBits(h.sizeNBits)) + 1;
    if (reader.getBit())  // sync word present in this frame
    {
        reader.skipBits(4);  // PBR smoothing buffer size
        const unsigned delayBits = reader.getBits(5) + 1;
        a.xllDelayFrames = static_cast<int>(reader.getBits(delayBits));
        reader.skipBits(h.sizeNBits);  // sync word offset
    }
    return size;
}

// Walks one asset descriptor. Everything before the decoder navigation data has
// to be walked exactly, because it is all variable length and there is no other
// way to reach the component sizes. The descriptor carries its own size, so the
// walk is re-anchored at the end and a mis-parse cannot leak into the next asset.
static bool parseAssetDescriptor(BitStreamReader& reader, const DtsExssHeader& h, DtsExssAsset& a)
{
    const int descrStart = reader.getBitsCount();
    const int descrSize = static_cast<int>(reader.getBits(9)) + 1;
    reader.skipBits(3);  // asset index

    int nChannelsTotal = 0;
    bool embeddedStereo = false;
    bool embedded6Ch = false;

    if (h.staticFieldsPresent)
    {
        if (reader.getBit())
            reader.skipBits(4);  // asset type descriptor
        if (reader.getBit())
            reader.skipBits(24);  // language descriptor
        if (reader.getBit())      // information text
        {
            const int textBytes = static_cast<int>(reader.getBits(10)) + 1;
            skipBitsLong(reader, static_cast<int64_t>(textBytes) * 8);
        }
        reader.skipBits(5);  // PCM bit resolution
        reader.skipBits(4);  // maximum sample rate
        nChannelsTotal = static_cast<int>(reader.getBits(8)) + 1;
        if (reader.getBit())  // one to one channel to speaker mapping
        {
            unsigned spkrMaskNBits = 0;
            if (nChannelsTotal > 2)
                embeddedStereo = reader.getBit();
            if (nChannelsTotal > 6)
                embedded6Ch = reader.getBit();
            if (reader.getBit())  // speaker mask enabled
            {
                spkrMaskNBits = (reader.getBits(2) + 1) << 2;
                reader.skipBits(spkrMaskNBits);  // speaker activity mask
            }
            const int nRemapSets = static_cast<int>(reader.getBits(3));
            if (nRemapSets > 0 && spkrMaskNBits == 0)
                return false;  // remap sets cannot be sized without a mask
            int nSpeakers[8] = {};
            for (int i = 0; i < nRemapSets; ++i) nSpeakers[i] = std::popcount(reader.getBits(spkrMaskNBits));
            for (int i = 0; i < nRemapSets; ++i)
            {
                const unsigned nCh = reader.getBits(5) + 1;
                for (int j = 0; j < nSpeakers[i]; ++j)
                {
                    const unsigned mask = reader.getBits(nCh);
                    skipBitsLong(reader, std::popcount(mask) * 5);
                }
            }
        }
        else
            reader.skipBits(3);  // representation type
    }

    const bool drcPresent = reader.getBit();
    if (drcPresent)
        reader.skipBits(8);  // DRC coefficients
    if (reader.getBit())
        reader.skipBits(5);  // dialog normalization
    if (drcPresent && embeddedStereo)
        reader.skipBits(8);  // DRC coefficients for the embedded stereo downmix

    if (h.mixMetadataEnabled && reader.getBit())
    {
        reader.skipBits(1);  // external mixing
        reader.skipBits(6);  // post mixing gain adjustment
        if (reader.getBits(2) == 3)
            reader.skipBits(8);
        else
            reader.skipBits(3);
        if (reader.getBit())
        {
            for (int i = 0; i < h.nMixOutConfigs; ++i) skipBitsLong(reader, 6LL * h.nMixOutChs[i]);
        }
        else
            skipBitsLong(reader, 6LL * h.nMixOutConfigs);

        const int nDownMixes = nChannelsTotal + (embedded6Ch ? 6 : 0) + (embeddedStereo ? 2 : 0);
        for (int i = 0; i < h.nMixOutConfigs; ++i)
        {
            if (h.nMixOutChs[i] == 0)
                return false;
            for (int j = 0; j < nDownMixes; ++j)
            {
                const unsigned mask = reader.getBits(h.nMixOutChs[i]);
                skipBitsLong(reader, std::popcount(mask) * 6);
            }
        }
    }

    // decoder navigation data: which components are present and how big they are
    int compSize[std::size(DCA_EXSS_COMPONENTS)] = {};
    const unsigned codingMode = reader.getBits(2);
    if (codingMode == 0)
    {
        a.extMask = static_cast<int>(reader.getBits(12));
        if (a.extMask & DCA_EXT_EXSS_CORE)
        {
            compSize[0] = static_cast<int>(reader.getBits(14)) + 1;
            if (reader.getBit())
                reader.skipBits(2);  // core sample rate and band limit info
        }
        if (a.extMask & DCA_EXT_EXSS_XBR)
            compSize[1] = static_cast<int>(reader.getBits(14)) + 1;
        if (a.extMask & DCA_EXT_EXSS_XXCH)
            compSize[2] = static_cast<int>(reader.getBits(14)) + 1;
        if (a.extMask & DCA_EXT_EXSS_X96)
            compSize[3] = static_cast<int>(reader.getBits(12)) + 1;
        if (a.extMask & DCA_EXT_EXSS_LBR)
            compSize[4] = parseLbrParameters(reader);
        if (a.extMask & DCA_EXT_EXSS_XLL)
            compSize[5] = parseXllParameters(reader, h, a);
        if (a.extMask & DCA_EXT_EXSS_RSV1)
            reader.skipBits(16);
        if (a.extMask & DCA_EXT_EXSS_RSV2)
            reader.skipBits(16);
    }
    else if (codingMode == 1)
    {
        a.extMask = DCA_EXT_EXSS_XLL;
        compSize[5] = parseXllParameters(reader, h, a);
    }
    else if (codingMode == 2)
    {
        a.extMask = DCA_EXT_EXSS_LBR;
        compSize[4] = parseLbrParameters(reader);
    }
    else
    {
        a.extMask = 0;
        reader.skipBits(14);
        reader.skipBits(8);
        if (reader.getBit())
            reader.skipBits(3);
    }

    int offset = a.offset;
    int left = a.size;
    for (size_t i = 0; i < std::size(DCA_EXSS_COMPONENTS); ++i)
    {
        if (!(a.extMask & DCA_EXSS_COMPONENTS[i]))
            continue;
        if (compSize[i] > left)
            return false;
        if (DCA_EXSS_COMPONENTS[i] == DCA_EXT_EXSS_XLL)
        {
            a.xllOffset = offset;
            a.xllSize = compSize[i];
        }
        offset += compSize[i];
        left -= compSize[i];
    }

    seekToBit(reader, descrStart + descrSize * 8);
    return true;
}

// Walks an XLL frame to the end of its band data and returns the 32 bit word
// that follows, if there is one. A plain DTS-HD MA frame ends on its band data
// and has no such word, which is what makes the badge safe.
static bool xllTrailerWord(const uint8_t* buf, const int len, uint32_t& word)
{
    BitStreamReader reader{};
    reader.setBuffer(const_cast<uint8_t*>(buf), buf + len);
    if (reader.getBits(32) != DTS_SYNC_XLL)
        return false;
    if (reader.getBits(4) + 1 > 1)  // XLL version
        return false;
    const int headerSize = static_cast<int>(reader.getBits(8)) + 1;
    if (headerSize > len || dtsCrc16(buf + 4, headerSize - 4) != 0)
        return false;
    const unsigned frameSizeNBits = reader.getBits(5) + 1;
    const int frameSize = static_cast<int>(reader.getBits(frameSizeNBits)) + 1;
    const int nChSets = static_cast<int>(reader.getBits(4)) + 1;
    const int nFrameSegs = 1 << reader.getBits(4);
    if (reader.getBits(4) == 0)  // samples per segment
        return false;
    const unsigned segSizeNBits = reader.getBits(5) + 1;
    reader.skipBits(2);  // band CRC present
    const bool scalableLsbs = reader.getBit();
    reader.skipBits(5);  // channel mask bit count
    if (scalableLsbs)
        reader.skipBits(4);

    // A frame longer than the component means it continues in the next extension
    // substream frame. Give up on this one; the next frame usually resolves it.
    if (frameSize > len)
        return false;
    seekToBit(reader, headerSize * 8);

    // Each channel set sub-header carries its own size, so only the leading
    // fields that decide the frequency band count have to be read. nChSets came
    // from a 4 bit field, so it cannot exceed the array.
    int chsFreqBands[16] = {};
    int nFreqBands = 1;
    for (int i = 0; i < nChSets; ++i)
    {
        const int hdrPos = reader.getBitsCount();
        if (hdrPos & 7)
            return false;
        const int chsHdrSize = static_cast<int>(reader.getBits(10)) + 1;
        if (hdrPos / 8 + chsHdrSize > len || dtsCrc16(buf + hdrPos / 8, chsHdrSize) != 0)
            return false;
        const unsigned nCh = reader.getBits(4) + 1;
        reader.skipBits(nCh);  // residual encoding flags
        reader.skipBits(5);    // PCM bit resolution
        reader.skipBits(5);    // storage bit resolution
        const int freq = ppi_dts_samplerate[reader.getBits(4)];
        chsFreqBands[i] = freq > 96000 ? 2 : 1;
        if (chsFreqBands[i] > nFreqBands)
            nFreqBands = chsFreqBands[i];
        seekToBit(reader, hdrPos + chsHdrSize * 8);
    }

    // The navigation table sizes every band data segment, so summing it is what
    // locates the end of the band data.
    if (nFreqBands * nFrameSegs * nChSets > 1024)
        return false;
    const int naviPos = reader.getBitsCount();
    if (naviPos & 7)
        return false;
    int64_t bandDataBytes = 0;
    for (int band = 0; band < nFreqBands; ++band)
        for (int seg = 0; seg < nFrameSegs; ++seg)
            for (int chs = 0; chs < nChSets; ++chs)
            {
                if (chsFreqBands[chs] <= band)
                    continue;
                const int segSize = static_cast<int>(reader.getBits(segSizeNBits));
                if (segSize >= frameSize)
                    return false;
                bandDataBytes += segSize + 1;
            }
    alignToByte(reader);
    reader.skipBits(16);  // navigation table CRC
    const int naviEnd = reader.getBitsCount();
    if (dtsCrc16(buf + naviPos / 8, (naviEnd - naviPos) / 8) != 0)
        return false;

    const int64_t frameBits = static_cast<int64_t>(frameSize) * 8;
    const int64_t bandEnd = naviEnd + bandDataBytes * 8;
    if (bandEnd > frameBits)
        return false;
    const int64_t trailer = (bandEnd + 31) & ~static_cast<int64_t>(31);
    if (trailer + 32 > frameBits)
        return false;  // no trailer word: this is plain DTS-HD MA
    const uint8_t* p = buf + trailer / 8;
    word =
        static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 | p[3];
    return true;
}

void DTSStreamReader::probeDtsX(uint8_t* buff, const uint8_t* end)
{
    m_dtsxFramesProbed++;
    try
    {
        BitStreamReader reader{};
        reader.setBuffer(buff, end);
        if (reader.getBits(32) != DTS_HD_PREFIX)
            return;
        reader.skipBits(8);  // user defined bits
        const unsigned nuSubStreamIndex = reader.getBits(2);
        const bool isBlownUpHeader = reader.getBit();

        DtsExssHeader h;
        h.headerSize = static_cast<int>(reader.getBits(isBlownUpHeader ? 12 : 8)) + 1;
        h.sizeNBits = isBlownUpHeader ? 20 : 16;
        h.frameSize = static_cast<int>(reader.getBits(h.sizeNBits)) + 1;
        if (h.headerSize < 6 || h.headerSize > h.frameSize || buff + h.frameSize > end)
            return;
        // The header CRC covers everything past the sync word and the user
        // defined bits, so a bad walk is rejected before it can reach the badge.
        if (dtsCrc16(buff + 5, h.headerSize - 5) != 0)
            return;

        h.staticFieldsPresent = reader.getBit();
        h.nAssets = 1;
        if (h.staticFieldsPresent)
        {
            reader.skipBits(2);  // reference clock code
            reader.skipBits(3);  // frame duration code
            if (reader.getBit())
            {
                reader.skipBits(18);  // time stamp, high bits
                reader.skipBits(18);  // time stamp, low bits
            }
            const int nAudioPresent = static_cast<int>(reader.getBits(3)) + 1;
            h.nAssets = static_cast<int>(reader.getBits(3)) + 1;
            unsigned active[8] = {};
            for (int i = 0; i < nAudioPresent; ++i) active[i] = reader.getBits(nuSubStreamIndex + 1);
            for (int i = 0; i < nAudioPresent; ++i) skipBitsLong(reader, std::popcount(active[i]) * 8);
            h.mixMetadataEnabled = reader.getBit();
            if (h.mixMetadataEnabled)
            {
                reader.skipBits(2);  // mix metadata adjustment level
                const unsigned spkrMaskNBits = (reader.getBits(2) + 1) << 2;
                h.nMixOutConfigs = static_cast<int>(reader.getBits(2)) + 1;
                for (int i = 0; i < h.nMixOutConfigs; ++i)
                    h.nMixOutChs[i] = std::popcount(reader.getBits(spkrMaskNBits));
            }
        }

        DtsExssAsset assets[8];
        int offset = h.headerSize;
        for (int i = 0; i < h.nAssets; ++i)
        {
            assets[i].offset = offset;
            assets[i].size = static_cast<int>(reader.getBits(h.sizeNBits)) + 1;
            offset += assets[i].size;
            if (offset > h.frameSize)
                return;
        }
        for (int i = 0; i < h.nAssets; ++i)
        {
            if (!parseAssetDescriptor(reader, h, assets[i]))
                return;
        }

        for (int i = 0; i < h.nAssets; ++i)
        {
            const DtsExssAsset& a = assets[i];
            if (!(a.extMask & DCA_EXT_EXSS_XLL) || a.xllSize <= 0 || a.xllOffset + a.xllSize > h.frameSize)
                continue;
            // A non zero decoding delay means this XLL frame started in an earlier
            // extension substream frame, so its end is not reachable from here.
            // About 0.4% of frames are like that and the next frame resolves them,
            // which is why the probe looks at more than one frame.
            if (a.xllDelayFrames != 0)
                continue;
            uint32_t word = 0;
            if (!xllTrailerWord(buff + a.xllOffset, a.xllSize, word))
                continue;
            const bool imaxWord = (word & DTSX_IMAX_MASK) == DTSX_IMAX_MARKER;
            if (word == DTSX_MARKER || imaxWord)
            {
                if (imaxWord)
                    m_dtsxImaxHits++;
                if (++m_dtsxHits >= DTSX_MIN_HITS)
                {
                    m_isDtsX = true;
                    // Same discipline as the badge itself: it takes more than one word, so a
                    // single stray value cannot decide which family this is.
                    m_isDtsXImax = m_dtsxImaxHits >= DTSX_MIN_HITS;
                }
            }
        }
    }
    catch (BitStreamException&)
    {
        // A frame this walk cannot follow simply does not vote.
    }
}

void DTSStreamReader::fillDiscoveryData(StreamDiscoveryData& data)
{
    SimplePacketizerReader::fillDiscoveryData(data);  // sampleRate, channels
    data.bitrate = pi_bit_rate;
    data.bitsPerSample = hd_bitDepth;
}

void DTSStreamReader::applyDiscoveryData(const StreamDiscoveryData& data)
{
    if (data.sampleRate > 0)
    {
        pi_sample_rate = data.sampleRate;
    }
    if (data.channels > 0)
    {
        pi_channels = static_cast<uint8_t>(data.channels);
    }
    if (data.bitrate > 0)
    {
        pi_bit_rate = data.bitrate;
    }
}

bool DTSStreamReader::isPriorityData(AVPacket* packet)
{
    return packet->size >= 2 && ((packet->data[0] == 0x7f && packet->data[1] == 0xfe) ||
                                 (packet->data[0] == 0xfe && packet->data[1] == 0x7f));
}

bool DTSStreamReader::isSecondary() { return m_secondary; }

int DTSStreamReader::decodeFrame(uint8_t* buff, uint8_t* end, int& skipBytes, int& skipBeforeBytes)
{
    uint8_t* afterFrameData = buff;
    i_frame_size = 0;
    // The sync word, and for a core its 14 byte header, are read before anything else; a frame that starts in the
    // last bytes of a read block was read past the block's end (as the AC-3 header was)
    if (end - buff < 4 || (m_isCoreExists && end - buff < static_cast<int>(DTS_HEADER_SIZE)))
        return NOT_ENOUGH_BUFFER;
    if (m_isCoreExists)
    {
        skipBeforeBytes = skipBytes = 0;
        if (m_state == DTSDecodeState::stDecodeHD && loadUnaligned<uint32_t>(buff) == my_htonl(DTS_HD_PREFIX))
        {
            m_state = DTSDecodeState::stDecodeHD2;
            return m_hdFrameLen;
        }
        m_state = DTSDecodeState::stDecodeDTS;

        // 14 bits, little endian version of the bitstream
        if (buff[0] == 0xff && buff[1] == 0x1f && buff[2] == 0x00 && buff[3] == 0xe8 && (buff[4] & 0xf0) == 0xf0 &&
            buff[5] == 0x07)
        {
            uint8_t conv_buf[DTS_HEADER_SIZE];
            buf14To16(conv_buf, buff, DTS_HEADER_SIZE, 1);
            i_frame_size = syncInfo16be(conv_buf);
            i_frame_size = i_frame_size * 8 / 14 * 2;
        }
        // 14 bits, big endian version of the bitstream
        else if (buff[0] == 0x1f && buff[1] == 0xff && buff[2] == 0xe8 && buff[3] == 0x00 && buff[4] == 0x07 &&
                 (buff[5] & 0xf0) == 0xf0)
        {
            uint8_t conv_buf[DTS_HEADER_SIZE];
            buf14To16(conv_buf, buff, DTS_HEADER_SIZE, 0);
            i_frame_size = syncInfo16be(conv_buf);
            i_frame_size = i_frame_size * 8 / 14 * 2;
        }
        // 16 bits, big endian version of the bitstream
        else if (buff[0] == 0x7f && buff[1] == 0xfe && buff[2] == 0x80 && buff[3] == 0x01)
        {
            i_frame_size = syncInfo16be(buff);
        }
        // 16 bits, little endian version of the bitstream
        else if (buff[0] == 0xfe && buff[1] == 0x7f && buff[2] == 0x01 && buff[3] == 0x80)
        {
            uint8_t conv_buf[DTS_HEADER_SIZE];
            BufLeToBe(conv_buf, buff, DTS_HEADER_SIZE);
            i_frame_size = syncInfo16be(buff);
        }
        else
            return 0;

        switch (pi_audio_mode & 0xffff)
        {
        case 0x0:
            // Mono
            pi_channels_conf = AOUT_CHAN_CENTER;
            break;
        case 0x1:
            // Dual-mono = stereo + dual-mono
            pi_channels_conf = AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT | AOUT_CHAN_DUALMONO;
            break;
        case 0x2:
        case 0x3:
        case 0x4:
            // Stereo
            pi_channels = 2;
            pi_channels_conf = AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT;
            break;
        case 0x5:
            // 3F
            pi_channels = 3;
            pi_channels_conf = AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT | AOUT_CHAN_CENTER;
            break;
        case 0x6:
            // 2F/1R
            pi_channels = 3;
            pi_channels_conf = AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT | AOUT_CHAN_REARCENTER;
            break;
        case 0x7:
            // 3F/1R
            pi_channels = 4;
            pi_channels_conf = AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT | AOUT_CHAN_CENTER | AOUT_CHAN_REARCENTER;
            break;
        case 0x8:
            // 2F2R
            pi_channels = 4;
            pi_channels_conf = AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT | AOUT_CHAN_REARLEFT | AOUT_CHAN_REARRIGHT;
            break;
        case 0x9:
            // 3F2R
            pi_channels = 5;
            pi_channels_conf =
                AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT | AOUT_CHAN_CENTER | AOUT_CHAN_REARLEFT | AOUT_CHAN_REARRIGHT;
            break;
        case 0xA:
        case 0xB:
            // 2F2M2R
            pi_channels = 6;
            pi_channels_conf = AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT | AOUT_CHAN_MIDDLELEFT | AOUT_CHAN_MIDDLERIGHT |
                               AOUT_CHAN_REARLEFT | AOUT_CHAN_REARRIGHT;
            break;
        case 0xC:
            // 3F2M2R
            pi_channels = 7;
            pi_channels_conf = AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT | AOUT_CHAN_CENTER | AOUT_CHAN_MIDDLELEFT |
                               AOUT_CHAN_MIDDLERIGHT | AOUT_CHAN_REARLEFT | AOUT_CHAN_REARRIGHT;
            break;
        case 0xD:
        case 0xE:
            // 3F2M2R/LFE
            pi_channels = 8;
            pi_channels_conf = AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT | AOUT_CHAN_CENTER | AOUT_CHAN_MIDDLELEFT |
                               AOUT_CHAN_MIDDLERIGHT | AOUT_CHAN_REARLEFT | AOUT_CHAN_REARRIGHT | AOUT_CHAN_LFE;
            break;
        default:
            if (pi_audio_mode <= 63)
            {
                // User defined
                pi_channels = 0;
                pi_channels_conf = 0;
            }
            else
                return 0;
        }

        if (pi_audio_mode & 0x10000)
        {
            (pi_channels)++;
            pi_channels_conf |= AOUT_CHAN_LFE;
            pi_lfeCnt = 1;
        }

        if (pi_sample_rate_index >= std::size(ppi_dts_samplerate))
        {
            return 0;
        }
        pi_sample_rate = ppi_dts_samplerate[pi_sample_rate_index];
        if (!pi_sample_rate)
            return 0;

        if (pi_bit_rate_index >= std::size(ppi_dts_bitrate))
        {
            return 0;
        }
        pi_bit_rate = ppi_dts_bitrate[pi_bit_rate_index];
        if (!pi_bit_rate)
            return 0;

        pi_frame_length = (nblks + 1) * 32;
        m_frameDuration = static_cast<double>(pi_frame_length) * INTERNAL_PTS_FREQ / pi_sample_rate;

        afterFrameData = buff + i_frame_size;
        if (afterFrameData > end - 4)
            return NOT_ENOUGH_BUFFER;

        if (m_testMode && m_dtsEsChannels == 0)
        {
            auto curPtr32 = UnalignedPtr<uint32_t>(buff + 16);
            const int findSize = FFMIN((int)(end - buff), i_frame_size) / 4 - 4;
            for (int i = 0; i < findSize; ++i)
            {
                if (*curPtr32++ == 0x5a5a5a5a)
                {
                    const auto exHeader = curPtr32.bytes();
                    const int dataRest = static_cast<int>(buff + i_frame_size - exHeader);
                    const int frameSize = (exHeader[0] << 2) + (exHeader[1] >> 6) - 4;  // remove 4 bytes of ext world
                    if (dataRest - frameSize == 0 || dataRest - frameSize == 1)
                    {
                        m_dtsEsChannels = (static_cast<int>(exHeader[1]) >> 2) & 0x07;
                    }
                }
            }
        }
    }

    if (loadUnaligned<uint32_t>(afterFrameData) == my_htonl(DTS_HD_PREFIX))
    {
        m_dts_hd_mode = true;

        const int hdFrameSize = decodeHdInfo(afterFrameData, end);
        if (hdFrameSize == NOT_ENOUGH_BUFFER)
            return NOT_ENOUGH_BUFFER;

        const uint8_t* nextFrame = afterFrameData + hdFrameSize;
        // nextFrame == end means the extension substream ends exactly on the buffer end, so it
        // is complete and can be processed now. Treating that as NOT_ENOUGH_BUFFER stalls the
        // last frame of the stream: no more data ever arrives, and flushPacket then emits the
        // whole remaining buffer verbatim, which puts one extension block into a
        // down-to-dts output that is supposed to carry the core only.
        if (nextFrame > end)
            return NOT_ENOUGH_BUFFER;
        // The whole extension substream frame is known to be in the buffer here,
        // which is what the DTS:X walk needs. It is bounded to the first few
        // frames and stops as soon as the badge is settled.
        if (!m_isDtsX && m_dtsxFramesProbed < DTSX_PROBE_FRAMES)
            probeDtsX(afterFrameData, nextFrame);
        if (m_downconvertToDTS)
        {
            // Extracting the core means dropping the extension substreams and keeping the core
            // frames interleaved with them. A DTS Express (LBR) track, and any other
            // extension-only stream, has no core substream at all, so this drops EVERY frame:
            // skipBytes swallows the whole extension and i_frame_size stays 0, which used to
            // produce a 0 byte file while still reporting "Mux successful complete". Mixed with
            // other tracks it was worse, leaving a PMT entry with no packets behind it.
            // m_isCoreExists is settled by checkIfOnlyHDDataExists() before any decodeFrame call,
            // so it is already known here. m_testMode excludes the detection pass, which must
            // stay non-fatal.
            if (!m_isCoreExists && !m_testMode)
                THROW(ERR_INVALID_CODEC_FORMAT,
                      "down-to-dts: this DTS track has no DTS core to extract, so there is nothing "
                      "to keep. Mux it without down-to-dts to pass the track through unchanged.")
            skipBytes = static_cast<int>(nextFrame - buff - i_frame_size);
            return i_frame_size;
        }
        m_state = DTSDecodeState::stDecodeHD;
        m_hdFrameLen = static_cast<int>(nextFrame - afterFrameData);
        return m_isCoreExists ? i_frame_size : m_hdFrameLen;
    }
    return i_frame_size;
}

int DTSStreamReader::syncInfo16be(const uint8_t* p_buf)
{
    nblks = (p_buf[4] & 0x01) << 6 | p_buf[5] >> 2;
    const int frame_size = (p_buf[5] & 0x03) << 12 | p_buf[6] << 4 | p_buf[7] >> 4;
    pi_audio_mode = (p_buf[7] & 0x0f) << 2 | p_buf[8] >> 6;
    pi_sample_rate_index = (p_buf[8] >> 2) & 0x0f;
    pi_bit_rate_index = (p_buf[8] & 0x03) << 3 | ((p_buf[9] >> 5) & 0x07);
    const int i_lfe = (p_buf[10] >> 1) & 0x03;
    if (i_lfe)
        pi_audio_mode |= 0x10000;

    const int pi_ext_coding = (p_buf[10] >> 4) & 0x01;
    if (pi_ext_coding)
    {
        const int ext_descr = p_buf[10] >> 5;
        core_ext_mask = dca_ext_audio_descr_mask[ext_descr];
    }
    else
    {
        core_ext_mask = 0;
    }

    return frame_size + 1;
}

int DTSStreamReader::testSyncInfo16be(const uint8_t* p_buf)
{
    nblks = (p_buf[4] & 0x01) << 6 | p_buf[5] >> 2;
    const int frame_size = (p_buf[5] & 0x03) << 12 | p_buf[6] << 4 | p_buf[7] >> 4;
    int test_audio_mode = (p_buf[7] & 0x0f) << 2 | p_buf[8] >> 6;
    const unsigned test_sample_rate_index = (p_buf[8] >> 2) & 0x0f;
    const unsigned test_bit_rate_index = (p_buf[8] & 0x03) << 3 | ((p_buf[9] >> 5) & 0x07);
    const int test_lfe = (p_buf[10] >> 1) & 0x03;
    if (test_lfe)
        test_audio_mode |= 0x10000;
    if (test_audio_mode == pi_audio_mode && test_sample_rate_index == pi_sample_rate_index &&
        test_bit_rate_index == pi_bit_rate_index)
        return frame_size + 1;
    return 0;
}

int DTSStreamReader::buf14To16(uint8_t* p_out, const uint8_t* p_in, const int i_in, const int i_le)
{
    unsigned char tmp, cur = 0;
    int bits_in, bits_out = 0;
    int i_out = 0;
    for (int i = 0; i < i_in; i++)
    {
        if (i % 2)
        {
            tmp = p_in[i - i_le];
            bits_in = 8;
        }
        else
        {
            tmp = p_in[i + i_le] & 0x3F;
            bits_in = 8 - 2;
        }

        if (bits_out < 8)
        {
            const int need = (std::min)(8 - bits_out, bits_in);
            cur <<= need;
            cur |= (tmp >> (bits_in - need));
            tmp <<= (8 - bits_in + need);
            tmp >>= (8 - bits_in + need);
            bits_in -= need;
            bits_out += need;
        }
        if (bits_out == 8)
        {
            p_out[i_out] = cur;
            cur = 0;
            bits_out = 0;
            i_out++;
        }

        bits_out += bits_in;
        cur <<= bits_in;
        cur |= tmp;
    }
    return i_out;
}

void DTSStreamReader::BufLeToBe(uint8_t* p_out, const uint8_t* p_in, const int i_in)
{
    for (int i = 0; i < i_in / 2; i++)
    {
        const int j = 2 * i;
        p_out[j] = p_in[j + 1];
        p_out[j + 1] = p_in[j];
    }
}

double DTSStreamReader::getFrameDuration()
{
    if (!m_isCoreExists)
        return m_frameDuration;
    if (m_dts_hd_mode && !m_downconvertToDTS && m_state != DTSDecodeState::stDecodeHD2)
        return 0;

    return m_frameDuration;
}

bool DTSStreamReader::needSkipFrame(const AVPacket& packet)
{
    if (m_skippingSamples == 0)
        return false;

    if (getFrameDuration() > 0)
        m_skippingSamples -= pi_frame_length;
    return true;
}

const std::string DTSStreamReader::getStreamInfo()
{
    std::ostringstream str;

    if (core_ext_mask & DCA_EXT_XCH)
        str << "DTS-ES";
    else if (core_ext_mask & DCA_EXT_X96)
        str << "DTS-96/24";
    if (core_ext_mask & (DCA_EXT_XCH | DCA_EXT_X96))
    {
        if (hd_pi_channels)
            str << " core ";
        else
            str << " ";
    }

    str << "Bitrate: ";
    // 1/*open*/, 2/*variable*/, 3/*lossless*/
    switch (pi_bit_rate)
    {
    case 1:
        str << "Open. ";
        break;
    case 2:
        str << "VBR. ";
        break;
    case 3:
        str << "Loseless. ";
        break;
    default:
        str << (pi_bit_rate + m_hdBitrate) / 1000 << "Kbps  ";
    }

    if (m_hdType == DTSHD_SUBTYPE::DTS_SUBTYPE_MASTER_AUDIO)
        str << "core + MLP data.";
    str << "Sample Rate: " << (hd_pi_sample_rate ? hd_pi_sample_rate : pi_sample_rate) / 1000 << "KHz  ";
    str << "Channels: ";
    if (hd_pi_channels)
    {
        if (hd_pi_lfeCnt)
            str << hd_pi_channels - hd_pi_lfeCnt << '.' << hd_pi_lfeCnt;
        else
            str << static_cast<int>(hd_pi_channels);
    }
    else
    {
        if (pi_lfeCnt)
            str << (pi_channels + m_dtsEsChannels - pi_lfeCnt) << '.' << pi_lfeCnt;
        else
            str << (pi_channels + m_dtsEsChannels);
    }
    if (m_dts_hd_mode)
    {
        switch (m_hdType)
        {
        case DTSHD_SUBTYPE::DTS_SUBTYPE_MASTER_AUDIO:
            str << " (DTS Master Audio";
            break;
        case DTSHD_SUBTYPE::DTS_SUBTYPE_HIGH_RES:
            str << " (DTS High Resolution";
            break;
        case DTSHD_SUBTYPE::DTS_SUBTYPE_EXPRESS:
            str << " (DTS Express";
            break;
        case DTSHD_SUBTYPE::DTS_SUBTYPE_EX:
            str << " (DTS Ex";
            break;
        case DTSHD_SUBTYPE::DTS_SUBTYPE_96:
            str << " (DTS 96";
            break;
        case DTSHD_SUBTYPE::DTS_SUBTYPE_UNINITIALIZED:
        case DTSHD_SUBTYPE::DTS_SUBTYPE_OTHER:
            break;
        }

        // The IMAX variant of the marker is named separately, the way the usual identification
        // tools name it: they call this track "DTS-HD MA + IMAX Enhanced" where a plain DTS:X
        // track is just "DTS-HD MA + DTS:X".
        //
        // IT NAMES THE AUDIO AND NOTHING ELSE. It does NOT say the title is an IMAX Enhanced
        // presentation, and the difference was measured rather than assumed: two demo trailers
        // carrying this same marker display at 1.7778:1, plain 16:9, while an IMAX Enhanced
        // presentation is 1.90:1. So the marker identifies an audio encoding variant, and a
        // certified IMAX picture is a separate thing that is not a flag in the bitstream at all.
        if (m_isDtsX)
            str << (m_isDtsXImax ? " + DTS:X IMAX" : " + DTS:X");

        if (hd_bitDepth > 16)
            str << " 24bit";

        str << ")";
    }
    return str.str();
}
