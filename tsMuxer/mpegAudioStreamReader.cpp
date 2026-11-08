// Largely rewritten by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.

#include "mpegAudioStreamReader.h"

#include <fs/systemlog.h>

#include <cstring>
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

// A Layer III frame that holds a Xing or Info header (after the side information) or a VBRI header (32 bytes after
// the frame header), as an encoder writes at the start of an MP3 file: the length and the seek table of the stream,
// not sound.
static bool isInfoFrame(const uint8_t* frame, const int size)
{
    if (size < 4 || frame[0] != 0xFF || (frame[1] & 0xE0) != 0xE0 || (frame[1] >> 1 & 3) != 1)
        return false;
    const bool mpeg1 = (frame[1] >> 3 & 3) == 3;
    const bool crc = (frame[1] & 1) == 0;
    const bool mono = (frame[3] >> 6) == 3;
    const int tag = 4 + (crc ? 2 : 0) + (mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17));
    if (tag + 4 <= size && (memcmp(frame + tag, "Xing", 4) == 0 || memcmp(frame + tag, "Info", 4) == 0))
        return true;
    return 36 + 4 <= size && memcmp(frame + 36, "VBRI", 4) == 0;
}

// ** THE XING OR INFO FRAME OF AN MP3 IS NOT SOUND. ** It is the first frame of the file, and a player of the file
// skips it. Muxed to a transport stream it was played as a frame of silence, and the sound came 24 ms (at 48 kHz) after
// its time; in a join, again at the start of each later file. It is left out, and the next frame takes its time. A
// demux keeps it, as the file it writes is an MP3 file again. Matroska is given the first one (keepInfoFrames), whose
// encoder delay and padding it keeps, and leaves it out itself; the one of a later file of a join was muxed as sound.
int MpegAudioStreamReader::readPacket(AVPacket& avPacket)
{
    while (true)
    {
        const int rez = SimplePacketizerReader::readPacket(avPacket);
        if (rez != 0 || m_demuxMode || (m_keepInfoFrames && m_frameNum == 1) || avPacket.data == nullptr ||
            !isInfoFrame(avPacket.data, avPacket.size))
            return rez;
        LTRACE(LT_INFO, 2,
               getCodecInfo().displayName << " stream (track " << m_streamIndex
                                          << "): the Xing/Info header frame holds no sound and is left out.");
        m_curPts -= getFrameDuration();
        m_frameNum--;
    }
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
