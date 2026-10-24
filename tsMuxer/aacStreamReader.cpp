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

CheckStreamRez AACStreamReader::checkStream(uint8_t* buffer, const int len, const ContainerType containerType,
                                            const int containerDataType, const int containerStreamIndex)
{
    CheckStreamRez rez =
        SimplePacketizerReader::checkStream(buffer, len, containerType, containerDataType, containerStreamIndex);
    if (!rez.codecInfo.codecID)
        return rez;
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
