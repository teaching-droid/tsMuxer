#include "mlpStreamReader.h"
#include "nalUnits.h"
#include "tsPacket.h"
#include "vodCoreException.h"
#include "vod_common.h"

void MLPStreamReader::fillDiscoveryData(StreamDiscoveryData& data)
{
    SimplePacketizerReader::fillDiscoveryData(data);  // sampleRate, channels
    data.bitrate = m_bitrate;
}

void MLPStreamReader::applyDiscoveryData(const StreamDiscoveryData& data)
{
    if (data.sampleRate > 0)
        m_samplerate = data.sampleRate;
    if (data.channels > 0)
        m_channels = static_cast<uint8_t>(data.channels);
    if (data.bitrate > 0)
        m_bitrate = data.bitrate;
}

int MLPStreamReader::getHeaderLen() { return MLP_HEADER_LEN; }

uint8_t* MLPStreamReader::findFrame(uint8_t* buff, uint8_t* end)
{
    uint8_t* frame = MLPCodec::findFrame(buff, end);

    // A Blu-ray track that carries the AC-3 compatibility core starts with an AC-3 frame and
    // interleaves the lossless frames behind it. Read as A_MLP the core frames are skipped, and
    // most of the lossless data goes with them: measured at 1.54 seconds recovered from a 39.9
    // second track, in a file that still reports itself as correct 8 channel 24 bit TrueHD. The
    // mux path loses the same way. Every label on the result is right and the audio is not there,
    // so this has to be refused rather than warned about.
    //
    // The test is the disc arrangement itself: an AC-3 sync word at the very start, with the
    // first lossless frame somewhere behind it. A standalone TrueHD stream begins with its own
    // frame, so it returns the start of the buffer and never matches.
    if (!m_coreProbed)
    {
        m_coreProbed = true;
        if (frame != nullptr && frame != buff && end - buff >= 2 && buff[0] == 0x0B && buff[1] == 0x77)
            THROW(ERR_INVALID_CODEC_FORMAT,
                  "This track carries an AC-3 core with the TrueHD interleaved behind it, which is how a Blu-ray "
                  "stores it, so A_MLP reads only a part of it. Change this line's codec to A_AC3: tsMuxeR keeps "
                  "the core and the lossless track together under that name, and it is what the track listing "
                  "reports for this track. Add down-to-ac3 to that line if you want the AC-3 core on its own.")
    }

    return frame;
}

const std::string MLPStreamReader::getStreamInfo()
{
    std::ostringstream str;

    if (m_subType == MlpSubType::stTRUEHD)
        str << "TRUE-HD";
    else if (m_subType == MlpSubType::stMLP)
        str << "MLP";
    else
        str << "UNKNOWN";

    if (m_substreams == 4)
        str << " + ATMOS";
    str << ". ";
    str << "Peak bitrate: " << m_bitrate / 1000 << "Kbps ";
    str << "Sample Rate: " << m_samplerate / 1000 << "KHz ";
    str << "Channels: " << static_cast<int>(m_channels);
    return str.str();
}

int MLPStreamReader::decodeFrame(uint8_t* buff, uint8_t* end, int& skipBytes, int& skipBeforeBytes)
{
    skipBytes = 0;
    skipBeforeBytes = 0;
    // Below MLP_FULL_HEADER_LEN the codec cannot tell a broken access unit from one that is
    // merely cut off by the end of the read block, and answers false either way. Returning 0
    // here would make the caller resync to the next major sync and drop everything up to it,
    // so ask for more data instead and let the access unit be carried into the next block.
    if (end - buff < MLP_FULL_HEADER_LEN)
        return NOT_ENOUGH_BUFFER;
    if (MLPCodec::decodeFrame(buff, end))
        return getFrameSize(buff);
    return 0;
}

int MLPStreamReader::getTSDescriptor(uint8_t* dstBuff, bool blurayMode, bool hdmvDescriptors)
{
    // Ensure we have decoded at least one frame so codec parameters are valid
    uint8_t* frame = MLPCodec::findFrame(m_buffer, m_bufEnd);
    if (frame == nullptr)
        return 0;
    int skipBytes = 0;
    int skipBeforeBytes = 0;
    if (decodeFrame(frame, m_bufEnd, skipBytes, skipBeforeBytes) < 1)
        return 0;

    if (hdmvDescriptors)
    {
        // Blu-ray core specifications - HDMV TrueHD/MLP audio registration descriptor
        *dstBuff++ = static_cast<uint8_t>(TSDescriptorTag::REGISTRATION);  // descriptor tag
        *dstBuff++ = 8;                                                    // descriptor length
        *dstBuff++ = 'H';
        *dstBuff++ = 'D';
        *dstBuff++ = 'M';
        *dstBuff++ = 'V';
        *dstBuff++ = 0xff;                                             // stuffing_bits
        *dstBuff++ = static_cast<uint8_t>(StreamType::AUDIO_TRUE_HD);  // stream_coding_type
        const int audio_presentation_type = (m_channels > 2) ? 6 : (m_channels == 2) ? 3 : 1;
        const int sampling_frequency = (m_samplerate == 192000) ? 5 : (m_samplerate == 96000) ? 4 : 1;
        *dstBuff++ = static_cast<uint8_t>(audio_presentation_type << 4 | sampling_frequency);
        *dstBuff++ = 0xff;  // stuffing_bits

        return 10;  // total descriptor length (2 header + 8 data)
    }

    // Non-HDMV: SMPTE-RA registered format identifier for MLP audio
    // https://smpte-ra.org/registered-mpeg-ts-ids
    *dstBuff++ = static_cast<uint8_t>(TSDescriptorTag::REGISTRATION);  // descriptor tag
    *dstBuff++ = 4;                                                    // descriptor length
    *dstBuff++ = 'm';
    *dstBuff++ = 'l';
    *dstBuff++ = 'p';
    *dstBuff++ = 'a';

    return 6;  // total descriptor length
}

int MLPStreamReader::readPacket(AVPacket& avPacket)
{
    while (true)
    {
        const int rez = SimplePacketizerReader::readPacket(avPacket);
        if (rez != 0)
            return rez;

        // thg packet
        // m_samplerate stays 0 until a major sync decodes; a stream that never produces one
        // must not divide by it.
        if (m_samplerate)
            avPacket.dts = avPacket.pts = m_totalTHDSamples * INTERNAL_PTS_FREQ / m_samplerate;

        m_totalTHDSamples += m_samples;
        m_demuxedTHDSamples += m_samples;
        if (m_demuxedTHDSamples >= m_samples)
        {
            m_demuxedTHDSamples -= m_samples;
        }
        return 0;
    }
}

int MLPStreamReader::flushPacket(AVPacket& avPacket)
{
    const int rez = SimplePacketizerReader::flushPacket(avPacket);
    if (rez > 0)
    {
        if (!(avPacket.flags & AVPacket::PRIORITY_DATA) && m_samplerate)
            avPacket.pts = avPacket.dts =
                m_totalTHDSamples * INTERNAL_PTS_FREQ / m_samplerate;  // replace time to a next HD packet
    }
    return rez;
}
