#include "mp3Codec.h"

static constexpr uint16_t ff_mpa_bitrate_tab[2][3][15] = {
    {
        {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},
        {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320},
    },
    {
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
    },
};

static constexpr uint16_t ff_mpa_freq_tab[3] = {44100, 48000, 32000};

// static constexpr int MPA_STEREO = 0;
// static constexpr int MPA_JSTEREO = 1;
// static constexpr int MPA_DUAL = 2;
static constexpr int MPA_MONO = 3;

// Frame length in bytes from a header alone, or 0 when it cannot be worked out. Deliberately a
// copy of the arithmetic in mp3DecodeFrame rather than a call to it: this runs while SEARCHING,
// on bytes that are very often not a frame at all, and it must not disturb the decoder's state.
static int mp3FrameSizeFromHeader(const uint32_t header)
{
    int lsf, mpeg25;
    if (header & (1 << 20))
    {
        lsf = (header & (1 << 19)) ? 0 : 1;
        mpeg25 = 0;
    }
    else
    {
        lsf = 1;
        mpeg25 = 1;
    }
    const int layer = 4 - static_cast<int>((header >> 17) & 3);
    if (layer == 4)
        return 0;
    const unsigned sampleRateIndex = (header >> 10) & 3;
    if (sampleRateIndex == 3)
        return 0;
    const int sampleRate = ff_mpa_freq_tab[sampleRateIndex] >> (lsf + mpeg25);
    if (sampleRate <= 0)
        return 0;
    const unsigned bitrateIndex = (header >> 12) & 0xf;
    // A free format stream, index 0, has no length in its header. mp3DecodeFrame already refuses
    // those, so requiring a length here takes nothing away that works today.
    if (bitrateIndex == 0 || bitrateIndex == 15)
        return 0;
    const int padding = static_cast<int>((header >> 9) & 1);
    int size = ff_mpa_bitrate_tab[lsf][layer - 1][bitrateIndex];
    switch (layer)
    {
    case 1:
        size = (size * 12000) / sampleRate;
        size = (size + padding) * 4;
        break;
    case 2:
        size = (size * 144000) / sampleRate;
        size += padding;
        break;
    default:
    case 3:
        size = (size * 144000) / (sampleRate << lsf);
        size += padding;
        break;
    }
    return size;
}

// The fields a real stream keeps constant from one frame to the next: the sync word, the version,
// the layer and the sample rate. Bitrate and padding are excluded because a variable bitrate file
// changes both legitimately, frame by frame.
static constexpr uint32_t MP3_CONSTANT_FIELDS = 0xfffe0c00;

uint8_t* MP3Codec::mp3FindFrame(uint8_t* buff, const uint8_t* end)
{
    if (buff == nullptr)
        return nullptr;
    // header
    for (uint8_t* cur = buff; cur < end - 4; cur++)
    {
        const uint32_t header = my_ntohl(*reinterpret_cast<uint32_t*>(cur));
        if ((header & 0xffe00000) != 0xffe00000)
            continue;
        // layer check
        if ((header & (3 << 17)) == 0)
            continue;
        // bit rate
        if ((header & (0xf << 12)) == 0xf << 12)
            continue;
        // frequency
        if ((header & (3 << 10)) == 3 << 10)
            continue;

        // A LONE HEADER IS NOT ENOUGH. The four tests above examine eleven sync bits and three
        // small fields, which arbitrary data satisfies roughly once every few thousand bytes, so
        // any sizeable block of non-audio in front of the stream is certain to contain matches. A
        // metadata tag is exactly that: measured on a real file, a UTF-16 byte order mark inside
        // the tag passed every test above, and the bytes from there to the true first frame were
        // written into the output as if they were sound. Demuxing an ordinary tagged file produced
        // an elementary stream beginning with the track title.
        //
        // So require the frame to be CONFIRMED by its successor: a second header exactly one frame
        // later, agreeing on the fields a stream does not change. Two independent matches at a
        // computed distance are far beyond coincidence, and this is what the detection path has
        // always done with its ten frame run; only this search accepted a single hit.
        const int frameSize = mp3FrameSizeFromHeader(header);
        if (frameSize <= 4)
            continue;
        if (cur + frameSize + 4 <= end)
        {
            const uint32_t next = my_ntohl(*reinterpret_cast<uint32_t*>(cur + frameSize));
            if ((next & MP3_CONSTANT_FIELDS) != (header & MP3_CONSTANT_FIELDS))
                continue;
        }
        // Too near the end of the buffer to look: accept it. Refusing here would discard the last
        // frame of every stream, which is a certain loss traded against a possible one.
        return cur;
    }
    return nullptr;
}

int MP3Codec::mp3DecodeFrame(uint8_t* buff, const uint8_t* end)
{
    // int sample_rate, frame_size, mpeg25, padding;
    // int sample_rate_index, bitrate_index;
    int mpeg25, lsf;
    if (end - buff < 4)
        return 0;
    const uint32_t header = my_ntohl(*reinterpret_cast<uint32_t*>(buff));

    // ** THE ELEVEN SYNC BITS, WHICH THIS NEVER CHECKED. **
    //
    // mp3FindFrame tests them before it will even consider a candidate. This function did not, so
    // anything at all handed to it came back as a frame with a length derived from whatever the
    // bytes happened to say. An ID3v2 tag header is the ordinary way to meet that: the four bytes
    // "ID3" and a version byte parse as Layer II at 11 kHz and return a length of 314.
    //
    // The reader is at a real frame boundary when it meets a tag mid stream, so it took those 314
    // bytes as audio, wrote them out, and landed 58 bytes INSIDE the first real frame after the
    // tag, which was then thrown away. Measured on a clean stream with a tag pushed in on a frame
    // boundary: a tag SHORTER than 314 bytes destroys one whole 835 byte frame of audio, a longer
    // one does not, because the bogus frame stays inside the tag and the resync lands correctly.
    //
    // Nothing that is a real frame can fail this test, because a real frame carries these bits by
    // definition, and the search path already required them of every candidate it returns.
    if ((header & 0xffe00000) != 0xffe00000)
        return 0;

    if (header & (1 << 20))
    {
        lsf = (header & (1 << 19)) ? 0 : 1;
        mpeg25 = 0;
    }
    else
    {
        lsf = 1;
        mpeg25 = 1;
    }

    m_layer = static_cast<int8_t>(4 - ((header >> 17) & 3));
    if (m_layer == 4)
        return 0;
    /* extract frequency */
    m_sample_rate_index = static_cast<uint8_t>((header >> 10) & 3);
    if (m_sample_rate_index == 3)
        return 0;  // invalid sample rate
    m_sample_rate = ff_mpa_freq_tab[m_sample_rate_index] >> (lsf + mpeg25);
    m_sample_rate_index += 3 * (lsf + mpeg25);
    // error_protection = ((header >> 16) & 1) ^ 1;

    m_bitrate_index = static_cast<uint8_t>((header >> 12) & 0xf);
    if (m_bitrate_index == 15)
        return 0;  // invalid bitrate index
    const auto padding = static_cast<uint8_t>((header >> 9) & 1);
    // extension = (header >> 8) & 1;
    m_mode = static_cast<uint8_t>((header >> 6) & 3);
    m_mode_ext = static_cast<uint8_t>((header >> 4) & 3);
    // copyright = (header >> 3) & 1;
    // original = (header >> 2) & 1;
    // emphasis = header & 3;

    m_nb_channels = (m_mode == MPA_MONO) ? 1 : 2;

    if (m_bitrate_index != 0)
    {
        m_frame_size = ff_mpa_bitrate_tab[lsf][m_layer - 1][m_bitrate_index];
        m_bit_rate = m_frame_size * 1000;
        switch (m_layer)
        {
        case 1:
            m_samples = 384;
            m_frame_size = (m_frame_size * 12000) / m_sample_rate;
            m_frame_size = (m_frame_size + padding) * 4;
            break;
        case 2:
            m_samples = 1152;
            m_frame_size = (m_frame_size * 144000) / m_sample_rate;
            m_frame_size += padding;
            break;
        default:
        case 3:
            m_samples = 1152;
            m_frame_size = (m_frame_size * 144000) / (m_sample_rate << lsf);
            m_frame_size += padding;
            break;
        }
    }
    else
    {
        /* if no frame size computed, signal it */
        return 0;
    }
    return m_frame_size;
}
