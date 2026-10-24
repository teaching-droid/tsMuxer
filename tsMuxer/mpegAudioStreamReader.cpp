
#include "mpegAudioStreamReader.h"

#include <sstream>

void MpegAudioStreamReader::fillDiscoveryData(StreamDiscoveryData& data)
{
    SimplePacketizerReader::fillDiscoveryData(data);  // sampleRate, channels
    data.bitrate = m_bit_rate;
}

void MpegAudioStreamReader::applyDiscoveryData(const StreamDiscoveryData& data)
{
    if (data.sampleRate > 0)
        m_sample_rate = data.sampleRate;
    if (data.bitrate > 0)
        m_bit_rate = data.bitrate;
}

int MpegAudioStreamReader::getTSDescriptor(uint8_t* dstBuff, bool blurayMode, bool hdmvDescriptors)
{
    uint8_t* frame = findFrame(m_buffer, m_bufEnd);
    if (frame == nullptr)
        return 0;
    int skipBytes = 0;
    int skipBeforeBytes = 0;
    decodeFrame(frame, m_bufEnd, skipBytes, skipBeforeBytes);
    return 0;
}

uint8_t* MpegAudioStreamReader::findFrame(uint8_t* buff, uint8_t* end) { return mp3FindFrame(buff, end); }

// A tail shorter than the four header bytes that begins with the eleven sync bits is a cut header.
int MpegAudioStreamReader::wholeFrameSizeAt(const uint8_t* p, const uint8_t* end)
{
    const int64_t avail = end - p;
    if (avail <= 0)
        return 0;
    if (avail < MPEG_AUDIO_HEADER_SIZE)
        return p[0] == 0xff && (avail < 2 || (p[1] & 0xe0) == 0xe0) ? NOT_ENOUGH_BUFFER : 0;
    return frameSizeAt(p, end);
}

int MpegAudioStreamReader::decodeFrame(uint8_t* buff, uint8_t* end, int& skipBytes, int& skipBeforeBytes)
{
    skipBytes = 0;
    skipBeforeBytes = 0;
    return mp3DecodeFrame(buff, end);
}

double MpegAudioStreamReader::getFrameDuration()
{
    // There used to be an "empirical correction" here that halved mono Layer III. It was making up
    // for 1152 samples being counted where MPEG-2 Layer III has 576, which happened to be right
    // for MPEG-2 mono and made MPEG-1 mono run at double speed while MPEG-2 stereo ran at half.
    // With the sample count right, nothing needs correcting.
    return (1.0 * INTERNAL_PTS_FREQ * m_samples) / m_sample_rate;
}

const std::string MpegAudioStreamReader::getStreamInfo()
{
    std::ostringstream str;
    str << "Bitrate: " << m_bit_rate / 1000 << "Kbps  ";
    str << "Sample Rate: " << m_sample_rate / 1000 << "KHz  ";
    str << "Channels: " << m_nb_channels << "  ";
    str << "Layer: " << static_cast<int>(m_layer);
    return str.str();
}
