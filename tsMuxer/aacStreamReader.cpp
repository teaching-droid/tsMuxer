// Largely rewritten by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#include "aacStreamReader.h"

#include <cmath>

#include "nalUnits.h"
#include "vodCoreException.h"

void AACStreamReader::fillDiscoveryData(StreamDiscoveryData& data)
{
    SimplePacketizerReader::fillDiscoveryData(data);  // sampleRate, channels
    data.bitrate = m_bit_rate;
}

void AACStreamReader::applyDiscoveryData(const StreamDiscoveryData& data)
{
    if (data.sampleRate > 0)
        m_sample_rate = data.sampleRate;
    if (data.channels > 0)
        m_channels = static_cast<uint8_t>(data.channels);
    if (data.bitrate > 0)
        m_bit_rate = data.bitrate;
}

int AACStreamReader::getHeaderLen() { return AAC_HEADER_LEN; }

// The listing gave every other audio format a bitrate and AAC none. AAC is usually variable, and the ten
// frames the detection checks are a fifth of a second, so the figure is the average over every frame in
// the data the listing reads, and it says so.
const std::string AACStreamReader::getStreamInfo()
{
    std::ostringstream str;
    if (m_seenSeconds > 0)
        str << "Bitrate: " << llround(static_cast<double>(m_seenBits) / m_seenSeconds / 1000.0) << "Kbps (average)  ";
    str << "Sample Rate: " << m_sample_rate / 1000 << "KHz  ";
    str << "Channels: " << static_cast<int>(m_channels);
    return str.str();
}

// ** AAC FROM A MATROSKA FILE IS CHECKED ON ITS OWN DATA. ** A Matroska file carries AAC without ADTS headers, and the
// demuxer writes one in front of each block from the CodecPrivate, so the frame check of checkStream passes on any
// data: an A_AAC track that held H.264 was listed as AAC. The first syntax element of the raw_data_block behind each
// header is read instead, which every AAC frame has: for channel configurations 1 to 7 it is the first channel element
// of the configuration (ISO/IEC 14496-3 Table 1.19: a single channel element for 1 and 3 to 7, a channel pair element
// for 2), element_instance_tag 0, or a data stream or fill element, which an encoder may put before it. Data that is
// not AAC meets that in about half of its blocks at most, and AAC in all of them; three in four is asked. Configuration
// 0 begins with the program config element the demuxer copies into each frame and is not checked. Too few frames for a
// share are not checked either, so a short track is listed as before.
static bool matroskaBlocksLookLikeAac(uint8_t* buffer, uint8_t* end)
{
    enum
    {
        ID_SCE = 0,
        ID_CPE = 1,
        ID_DSE = 4,
        ID_FIL = 6
    };
    static constexpr int MIN_FRAMES = 8;
    static constexpr int MAX_FRAMES = 64;
    int checked = 0;
    int fitting = 0;
    for (uint8_t* frame = AACCodec::findAacFrame(buffer, end, nullptr);
         frame && end - frame > AAC_HEADER_LEN + 2 && checked < MAX_FRAMES;)
    {
        const int size = AACCodec::getFrameSize(frame);
        const int headerLen = (frame[1] & 0x01) ? AAC_HEADER_LEN : AAC_HEADER_LEN + 2;
        if (size <= headerLen || size > end - frame)
            break;
        if (const int channelConfig = ((frame[2] & 0x01) << 2) | (frame[3] >> 6); channelConfig != 0)
        {
            const int element = frame[headerLen] >> 5;
            const int tag = (frame[headerLen] >> 1) & 0x0f;
            const int first = channelConfig == 2 ? ID_CPE : ID_SCE;
            if ((element == first && tag == 0) || element == ID_DSE || element == ID_FIL)
                ++fitting;
            ++checked;
        }
        frame += size;
    }
    return checked < MIN_FRAMES || fitting * 4 >= checked * 3;
}

CheckStreamRez AACStreamReader::checkStream(uint8_t* buffer, const int len, const ContainerType containerType,
                                            const int containerDataType, const int containerStreamIndex)
{
    CheckStreamRez rez =
        SimplePacketizerReader::checkStream(buffer, len, containerType, containerDataType, containerStreamIndex);
    if (!rez.codecInfo.codecID)
        return rez;
    if (containerType == ContainerType::ctMKV && !matroskaBlocksLookLikeAac(buffer, buffer + len))
        return {};
    // Only the frame headers are read: each one gives its own length and its number of samples. The
    // header itself (7 bytes, 9 with its CRC) is not part of the audio and is left out of the figure, which
    // is how a codec's bitrate is given; an AAC read out of an MP4 or a Matroska carries that header only
    // because the reader adds it.
    //
    // ** THE HEADERS ARE READ INTO A CODEC OF THEIR OWN, NOT INTO THIS READER. ** Read into the reader, they
    // left it with the LAST frame's sample rate and channels, so the line no longer showed what the detection
    // had found: a stream that changes from mono to stereo inside the data was listed as stereo, and a 48 kHz
    // stereo AAC followed by a 96 kHz 5.1 one was listed as 96 kHz 5.1 at 1313 Kbps, every frame timed by the
    // last frame's rate, where the average is 821. Each frame is now timed by its own sample rate.
    AACCodec walker;
    m_seenBits = 0;
    m_seenSeconds = 0;
    uint8_t* const end = buffer + len;
    for (uint8_t* frame = findFrame(buffer, end); frame && end - frame >= AAC_HEADER_LEN;)
    {
        if (!walker.decodeFrame(frame, end))
            break;
        const int size = getFrameSize(frame);
        const int headerSize = (frame[1] & 0x01) ? AAC_HEADER_LEN : AAC_HEADER_LEN + 2;
        if (size <= headerSize || size > end - frame)
            break;  // no room for audio behind its own header, so not a frame; or cut off by the end of the data
        m_seenBits += static_cast<int64_t>(size - headerSize) * 8;
        m_seenSeconds += static_cast<double>(walker.m_samples) / walker.m_sample_rate;
        frame += size;
    }
    rez.streamDescr = getStreamInfo();
    return rez;
}

// Returns frame length, or zero (not a frame), or NOT_ENOUGH_BUFFER.
//
// ** THE LAST CLAUSE WAS MISSING AND IT COST EVERY BUILD A FRAME. ** AACCodec::decodeFrame reads
// through a BitStreamReader and catches BitStreamException, so a header cut short by the end of the
// buffer simply answers false, and answering 0 here tells the caller "this is not a frame" when the
// truth is that there is not enough data yet to say.
//
// The caller has two paths. After a sync it checks m_bufEnd - m_curPos < getHeaderLen() and asks
// for more data, so it is safe. On a RESYNC it does not, so a candidate less than a header from the
// end of the block took the decodeRez <= 0 branch: m_curPos++, the byte charged as lost, and the
// frame gone. Measured on a file whose audio starts DEFAULT_FILE_BLOCK_SIZE - 6 bytes in, with the
// junk in front carrying no ADTS candidate of its own:
//
//     junk prefix 2,097,145    194,146 bytes out, the whole stream
//     junk prefix 2,097,146    193,637 bytes out, short by 509 - exactly the first frame's
//                              declared aac_frame_length - at exit 0 with no warning
//
// 2,097,152 - 2,097,145 = 7 = AAC_HEADER_LEN, so the window is exactly one header wide. This is not
// a defect of the frame search: the unmodified reader loses the same 509 bytes.
//
// AC3Codec::decodeFrame already keeps this contract ("if (end - buf < 2) return
// NOT_ENOUGH_BUFFER"), and the caller already handles the answer on both paths, memmoving from
// m_curPos rather than from the frame so the junk in front is carried over too, not dropped.
int AACStreamReader::decodeFrame(uint8_t* buff, uint8_t* end, int& skipBytes, int& skipBeforeBytes)
{
    skipBytes = 0;
    skipBeforeBytes = 0;
    if (end - buff < AAC_HEADER_LEN)
        return NOT_ENOUGH_BUFFER;
    if (AACCodec::decodeFrame(buff, end))
        return getFrameSize(buff);
    return 0;
}

// The ADTS header gives aac_frame_length, header included, in its bytes 3 to 5. A tail shorter than that which begins
// with the 12 sync bits is a cut header.
int AACStreamReader::wholeFrameSizeAt(const uint8_t* p, const uint8_t* end)
{
    const int64_t avail = end - p;
    if (avail <= 0 || p[0] != 0xff || (avail >= 2 && (p[1] & 0xf0) != 0xf0))
        return 0;
    if (avail < 6)
        return NOT_ENOUGH_BUFFER;
    const int size = getFrameSize(p);
    return size >= AAC_HEADER_LEN ? size : 0;
}

int AACStreamReader::getTSDescriptor(uint8_t* dstBuff, bool blurayMode, bool hdmvDescriptors)
{
    // Ensure we have decoded at least one frame so codec parameters are valid
    uint8_t* frame = findFrame(m_buffer, m_bufEnd);
    if (frame == nullptr)
        return 0;
    int skipBytes = 0;
    int skipBeforeBytes = 0;
    if (decodeFrame(frame, m_bufEnd, skipBytes, skipBeforeBytes) < 1)
        return 0;

    // H.222 Table 2-94 - MPEG-2 AAC_audio_descriptor
    *dstBuff++ = static_cast<uint8_t>(TSDescriptorTag::AAC2);  // MPEG-2 AAC descriptor tag
    *dstBuff++ = 3;                                            // descriptor length
    *dstBuff++ = m_profile;                                    // MPEG-2_AAC_profile
    *dstBuff++ = m_channels_index;                             // MPEG-2_AAC_channel_configuration
    *dstBuff++ = 0;                                            // MPEG-2_AAC_additional_information

    return 5;  // total descriptor length
}
