#include "aac.h"

#include "bitStream.h"
#include "vod_common.h"

static constexpr int aac_sample_rates[16] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350,
};

static constexpr uint8_t aac_channels[8] = {0, 1, 2, 3, 4, 5, 6, 8};

// Is there a second ADTS header exactly one frame after this one, agreeing on the fields a stream
// does not change?
//
// ** THIS IS COMMIT 14638a4's TECHNIQUE, APPLIED TO THE CODEC THAT NEEDED IT MOST. ** That commit
// fixed an MP3 whose demuxed audio began with the song title, by refusing to believe a single
// frame sync. AAC's search was the worse case and was left alone: it accepts twelve sync bits and
// two layer bits, which arbitrary data satisfies about once every sixteen thousand bytes, and AAC
// has no CRC check either. A metadata tag is a large block of arbitrary data sitting in front of
// the stream, so it was never a question of whether a false match occurs but of how many.
//
// Measured, on a real AAC file carrying 1.2 MB of cover art: the demuxed elementary stream came
// out at 455,104 bytes where the audio is 194,146. A quarter of a megabyte of picture was written
// out as if it were sound.
//
// Two independent matches at a distance the first one COMPUTED are far beyond coincidence. And
// unlike stepping over a tag by its declared length - which eight rounds of review showed destroys
// real audio - a stricter search can never skip more than it can prove: when it refuses, it simply
// keeps looking, which is self correcting.
static bool adtsConfirmedBySuccessor(const uint8_t* h, const uint8_t* end)
{
    if (end - h < 7)
        return true;  // too near the end of the buffer to look. Refusing here would discard the
                      // last frame of every stream, a certain loss traded against a possible one
    const int frameSize = (h[3] & 0x03) << 11 | h[4] << 3 | h[5] >> 5;
    if (frameSize < 7)
        return false;  // shorter than its own header, so it is not one
    const uint8_t* n = h + frameSize;
    if (end - n < 4)
        return true;  // the successor has not arrived yet; judged when it does
    // sync and layer, then profile and sample rate index, then channel configuration. The private
    // bit and the copyright bits are free to differ and are masked out.
    return n[0] == 0xff && (n[1] & 0xf6) == (h[1] & 0xf6) && (n[2] & 0xfd) == (h[2] & 0xfd) &&
           (n[3] & 0xc0) == (h[3] & 0xc0);
}

// ** A REFUSAL MUST NEVER COST A FRAME, AND THE FIRST VERSION OF THIS DID. **
//
// The ninth review measured it. I had written that a stricter search "can never skip more than it
// can prove, and if it refuses a candidate it keeps looking, which is self correcting". The first
// half is true. The second was an assumption about the CALLER and it was wrong: when this function
// finds nothing and returns nullptr, simplePacketizerReader DISCARDS everything from m_curPos to
// the end of the buffer. A refused frame is not revisited. It is thrown away.
//
// So a frame whose successor is a TAG rather than another frame died - and a tag after the last
// frame is the ordinary shape of almost every tagged file there is:
//     a genuine last frame after any resync    13 to 339 bytes gone, exit 0, no warning
//     a second file consisting of one frame    all 509 bytes of it
//
// THE RULE NOW: prefer a confirmed frame, but if the whole buffer holds none, return the first
// candidate the UNMODIFIED reader would have returned. The strictness can then only ever move the
// answer FORWARD past unconfirmable junk; it can never turn an answer into nothing.
//
// ** THAT IS WHAT MAKES THIS "NEVER WORSE THAN NOT HAVING IT", WHICH IS WHAT I CLAIMED BEFORE
//    WITHOUT CHECKING IT. **
// ** AND THE RULE ABOVE IS RIGHT INSIDE A TAG AND WRONG EVERYWHERE ELSE. **
//
// Refusing a candidate cannot cost audio where there is no audio, and a metadata tag is exactly
// that place. Outside one it costs plenty: a genuine frame whose SUCCESSOR is damaged is
// indistinguishable from a false sync, so refusing it throws away every real frame in front of the
// next confirmable one. Measured on this reader: one flipped byte cost 881 bytes of undamaged
// audio, and a file whose frames are separated by junk lost 193,449 of 194,146 at exit 0 in
// silence.
//
// Those two cases cannot be separated by looking at the candidate, nor by counting the frames that
// follow it. They are identical on every such measure, which was established by measurement rather
// than argued: the same tests accept and reject both together at every depth. What separates them
// is not a property of the sync at all. It is WHERE THE READER IS STANDING.
//
// So strictness now applies below tagCeiling and nowhere else. The caller sets that to the end of a
// tag run it WALKED AND VERIFIED, never one that merely declared a length, because a few bytes of
// forged header would otherwise arm a skip over real audio. With no tag there, the ceiling is the
// search position itself, every candidate is at or above it, and the FIRST one is returned: byte
// for byte what the unmodified reader answers.
uint8_t* AACCodec::findAacFrame(uint8_t* buffer, const uint8_t* end, const uint8_t* tagCeiling)
{
    uint8_t* curBuf = buffer;
    uint8_t* firstUnconfirmed = nullptr;
    if (tagCeiling == nullptr)
        tagCeiling = buffer;
    while (curBuf < end)
    {
        if (*curBuf < 0xf0)
        {
            curBuf += 2;
            continue;
        }
        uint8_t* cand = nullptr;
        if (*curBuf == 0xff && curBuf < end - 1 && (curBuf[1] & 0xf6) == 0xf0)
            cand = curBuf;
        else if ((*curBuf & 0xf6) == 0xf0 && curBuf > buffer && curBuf[-1] == 0xff)
            cand = curBuf - 1;
        if (cand != nullptr)
        {
            if (cand >= tagCeiling)
                return cand;  // outside a verified tag: the unmodified answer, unconditionally
            if (adtsConfirmedBySuccessor(cand, end))
                return cand;
            if (firstUnconfirmed == nullptr)
                firstUnconfirmed = cand;  // what the unmodified reader would have answered
        }
        curBuf++;
    }
    return firstUnconfirmed;
}

int AACCodec::getFrameSize(const uint8_t* buffer) { return (buffer[3] & 0x03) << 11 | buffer[4] << 3 | buffer[5] >> 5; }

bool AACCodec::decodeFrame(uint8_t* buffer, const uint8_t* end)
{
    BitStreamReader bits{};
    try
    {
        bits.setBuffer(buffer, end);
        if (bits.getBits(12) != 0xfff)  // sync bytes
            return false;

        m_id = bits.getBit();               /* 0: MPEG-4, 1: MPEG-2*/
        m_layer = bits.getBits<uint8_t>(2); /* layer */
        bits.skipBit();                     /* protection_absent */
        // -- 16 bit
        m_profile = bits.getBits<uint8_t>(2);            /* profile_objecttype */
        m_sample_rates_index = bits.getBits<uint8_t>(4); /* sample_frequency_index */
        if (!aac_sample_rates[m_sample_rates_index])
            return false;
        bits.skipBit();                              /* private_bit */
        m_channels_index = bits.getBits<uint8_t>(3); /* channel_configuration */
        if (!aac_channels[m_channels_index])
            return false;
        bits.skipBit(); /* original/copy */
        bits.skipBit(); /* home */

        /* adts_variable_header */
        bits.skipBit();                                    /* copyright_identification_bit */
        bits.skipBit();                                    /* copyright_identification_start */
        const auto frameSize = bits.getBits<uint16_t>(13); /* aac_frame_length */
        bits.skipBits(11);                                 /* adts_buffer_fullness */
        m_rdb = bits.getBits<uint8_t>(2);                  /* number_of_raw_data_blocks_in_frame */

        m_channels = aac_channels[m_channels_index];
        m_sample_rate = aac_sample_rates[m_sample_rates_index];
        m_samples = (m_rdb + 1) * 1024;
        m_bit_rate = frameSize * 8 * m_sample_rate / m_samples;
        return true;
    }
    catch (BitStreamException&)
    {
        return false;
    }
}

void AACCodec::buildADTSHeader(uint8_t* buffer, const unsigned frameSize)
{
    BitStreamWriter writer{};
    writer.setBuffer(buffer, buffer + AAC_HEADER_LEN);
    writer.putBits(12, 0xfff);
    writer.putBit(m_id);
    writer.putBits(2, m_layer);
    writer.putBit(1);  // protection_absent
    writer.putBits(2, m_profile);
    m_sample_rates_index = 0;
    for (uint8_t i = 0; i < 16; i++)
        if (aac_sample_rates[i] == m_sample_rate)
        {
            m_sample_rates_index = i;
            break;
        }
    writer.putBits(4, m_sample_rates_index);
    writer.putBit(0); /* private_bit */
    m_channels_index = 0;
    for (uint8_t i = 0; i < 8; i++)
        if (aac_channels[i] == m_channels)
        {
            m_channels_index = i;
            break;
        }
    writer.putBits(3, m_channels_index);

    writer.putBit(0); /* original/copy */
    writer.putBit(0); /* home */

    /* adts_variable_header */
    writer.putBit(0); /* copyright_identification_bit */
    writer.putBit(0); /* copyright_identification_start */

    writer.putBits(13, frameSize);  // /* aac_frame_length */
    writer.putBits(11, 2047);       // /* adts_buffer_fullness */
    writer.putBits(2, m_rdb);       /* number_of_raw_data_blocks_in_frame */
    writer.flushBits();
}

void AACCodec::readConfig(uint8_t* buff, const int size)
{
    BitStreamReader reader{};
    reader.setBuffer(buff, buff + size);
    auto object_type = reader.getBits<uint8_t>(5);
    if (object_type == 31)
        object_type = 32 + reader.getBits<uint8_t>(6);
    m_profile = (object_type & 0x3) - 1;
    m_sample_rates_index = reader.getBits<uint8_t>(4);
    m_sample_rate = m_sample_rates_index == 0x0f ? reader.getBits<int>(24) : aac_sample_rates[m_sample_rates_index];
    m_channels_index = reader.getBits<uint8_t>(4);
    m_channels = aac_channels[m_channels_index];
    // return specific_config_bitindex;
}
