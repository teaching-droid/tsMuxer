#include "aac.h"

#include "bitStream.h"
#include "vod_common.h"

static constexpr int aac_sample_rates[16] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350,
};

// Channels per channel configuration. 8 to 15 were read past the end of an 8-entry table; 11 to 14 are 6.1, 7.1,
// 22.2 and 7.1 (ISO/IEC 14496-3 table 1.19), the others are reserved.
static constexpr uint8_t aac_channels[16] = {0, 1, 2, 3, 4, 5, 6, 8, 0, 0, 0, 7, 8, 24, 8, 0};

// Reads a program config element from its first field on. Its byte alignment counts from the start of the reader's
// buffer, which is where the frame or the AudioSpecificConfig begins. False when it names no channel.
static bool readProgramConfig(BitStreamReader& reader, AacProgramConfig& pce)
{
    pce = {};
    auto take = [&](const unsigned n)
    {
        const unsigned value = reader.getBits(n);
        for (int i = static_cast<int>(n) - 1; i >= 0; --i) pce.bits.push_back(static_cast<uint8_t>(value >> i & 1));
        return value;
    };
    take(4);  // element_instance_tag
    take(2);  // object_type
    take(4);  // sampling_frequency_index
    const unsigned front = take(4);
    const unsigned side = take(4);
    const unsigned back = take(4);
    const unsigned lfe = take(2);
    const unsigned assocData = take(3);
    const unsigned validCc = take(4);
    if (take(1))
        take(4);  // mono_mixdown_element_number
    if (take(1))
        take(4);  // stereo_mixdown_element_number
    if (take(1))
        take(3);  // matrix_mixdown_idx, pseudo_surround_enable
    int channels = 0;
    for (unsigned i = 0; i < front + side + back; ++i)
    {
        channels += take(1) ? 2 : 1;  // is_cpe
        take(4);                      // element_tag_select
    }
    for (unsigned i = 0; i < lfe; ++i)
    {
        ++channels;
        take(4);
    }
    for (unsigned i = 0; i < assocData; ++i) take(4);
    for (unsigned i = 0; i < validCc; ++i) take(5);
    reader.skipBits((8 - reader.getBitsCount() % 8) % 8);  // byte_alignment
    const unsigned commentBytes = reader.getBits(8);
    for (unsigned i = 0; i < commentBytes; ++i) pce.comment.push_back(static_cast<uint8_t>(reader.getBits(8)));
    pce.channels = channels;
    return channels > 0;
}

static void putBitsTo(std::vector<uint8_t>& out, size_t& pos, const unsigned n, const unsigned value)
{
    for (int i = static_cast<int>(n) - 1; i >= 0; --i, ++pos)
    {
        if (pos % 8 == 0)
            out.push_back(0);
        if (value >> i & 1)
            out.back() |= static_cast<uint8_t>(0x80 >> pos % 8);
    }
}

// Writes the element at bit pos of out, aligned as counted from the start of out; it ends on a byte boundary.
static void writeProgramConfig(std::vector<uint8_t>& out, size_t& pos, const AacProgramConfig& pce)
{
    for (const auto bit : pce.bits) putBitsTo(out, pos, 1, bit);
    if (pos % 8)
        putBitsTo(out, pos, static_cast<unsigned>(8 - pos % 8), 0);
    putBitsTo(out, pos, 8, static_cast<unsigned>(pce.comment.size()));
    for (const auto byte : pce.comment) putBitsTo(out, pos, 8, byte);
}

void AACCodec::setPce(const AacProgramConfig& pce)
{
    if (pce.bits == m_pce.bits && pce.comment == m_pce.comment && pce.channels == m_pce.channels)
        return;
    m_pce = pce;
    m_inBandPce.clear();
    if (pce.channels == 0)
        return;
    size_t pos = 0;
    putBitsTo(m_inBandPce, pos, 3, 5);  // ID_PCE
    writeProgramConfig(m_inBandPce, pos, pce);
}

std::vector<uint8_t> AACCodec::audioSpecificConfig() const
{
    std::vector<uint8_t> config;
    size_t pos = 0;
    putBitsTo(config, pos, 5, m_profile + 1u);  // audioObjectType: the profile is one lower
    putBitsTo(config, pos, 4, m_sample_rates_index);
    putBitsTo(config, pos, 4, m_pce.channels ? 0 : m_channels_index);
    putBitsTo(config, pos, 3, 0);  // frameLengthFlag, dependsOnCoreCoder, extensionFlag
    if (m_pce.channels)
        writeProgramConfig(config, pos, m_pce);
    return config;
}

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
        const bool protectionAbsent = bits.getBit();
        // -- 16 bit
        m_profile = bits.getBits<uint8_t>(2);            /* profile_objecttype */
        m_sample_rates_index = bits.getBits<uint8_t>(4); /* sample_frequency_index */
        if (!aac_sample_rates[m_sample_rates_index])
            return false;
        bits.skipBit();                              /* private_bit */
        m_channels_index = bits.getBits<uint8_t>(3); /* channel_configuration */
        bits.skipBit();                              /* original/copy */
        bits.skipBit();                              /* home */

        /* adts_variable_header */
        bits.skipBit();                                    /* copyright_identification_bit */
        bits.skipBit();                                    /* copyright_identification_start */
        const auto frameSize = bits.getBits<uint16_t>(13); /* aac_frame_length */
        bits.skipBits(11);                                 /* adts_buffer_fullness */
        m_rdb = bits.getBits<uint8_t>(2);                  /* number_of_raw_data_blocks_in_frame */
        m_crc = !protectionAbsent;

        // Channel configuration 0: the layout is in a program config element at the start of the frame, as for 6.0,
        // 6.1 or 7.1 with front wide speakers; a stream may give it in its first frame only. Such a stream was not
        // recognised at all.
        if (m_channels_index == 0)
        {
            if (!protectionAbsent)
                bits.skipBits(16u * (m_rdb + 1u));  // raw_data_block_position of the later blocks, crc_check
            AacProgramConfig pce;
            if (bits.getBits(3) == 5 && readProgramConfig(bits, pce))  // ID_PCE
                setPce(pce);
            else if (m_pce.channels == 0)
                return false;
        }
        else
            setPce({});
        m_channels = m_channels_index ? aac_channels[m_channels_index] : static_cast<uint8_t>(m_pce.channels);
        m_sample_rate = aac_sample_rates[m_sample_rates_index];
        m_samples = (m_rdb + 1) * 1024;
        // In 64 bits: a long frame at 44.1 kHz and above went past the int range (8191 bytes at 96 kHz is
        // 6.3 billion before the division). The result itself always fits.
        m_bit_rate = static_cast<int>(static_cast<int64_t>(frameSize) * 8 * m_sample_rate / m_samples);
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
    // 0 with a program config element, which the frames carry
    m_channels_index = 0;
    for (uint8_t i = 0; i < 8 && m_inBandPce.empty(); i++)
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

int AACCodec::explicitSbrRate(const uint8_t* config, const int size)
{
    if (config == nullptr || size < 3)
        return 0;
    try
    {
        BitStreamReader reader{};
        // the reader takes a buffer it may write to, and only reads it
        auto* buffer = const_cast<uint8_t*>(config);
        reader.setBuffer(buffer, buffer + size);
        const auto rateAt = [&](const uint8_t index)
        { return index == 0x0f ? reader.getBits<int>(24) : index < 13 ? aac_sample_rates[index] : 0; };
        auto objectType = reader.getBits<uint8_t>(5);
        if (objectType == 31)
            objectType = 32 + reader.getBits<uint8_t>(6);
        if (reader.getBits<uint8_t>(4) == 0x0f)  // the core's samplingFrequencyIndex
            reader.skipBits(24);
        const auto channels = reader.getBits<uint8_t>(4);
        if (objectType == 5 || objectType == 29)
            return rateAt(reader.getBits<uint8_t>(4));  // extensionSamplingFrequencyIndex
        // The backward compatible form: AAC LC, and after its GASpecificConfig an extension with sync word 0x2B7
        // that names SBR and its rate, which a decoder that does not know it reads past
        if (objectType != 2)
            return 0;
        reader.skipBit();  // frameLengthFlag
        if (reader.getBit())
            reader.skipBits(14);  // dependsOnCoreCoder, coreCoderDelay
        reader.skipBit();         // extensionFlag
        AacProgramConfig pce;
        if (channels == 0 && !readProgramConfig(reader, pce))
            return 0;
        if (size * 8 - reader.getBitsCount() < 16 || reader.getBits<int>(11) != 0x2B7 ||
            reader.getBits<uint8_t>(5) != 5 || !reader.getBit())  // extensionAudioObjectType SBR, sbrPresentFlag
            return 0;
        return rateAt(reader.getBits<uint8_t>(4));
    }
    catch (BitStreamException&)
    {
        return 0;
    }
}

std::string AACCodec::sbrParam(const uint8_t* config, const int size)
{
    const int rate = explicitSbrRate(config, size);
    if (rate <= 0)
        return {};
    static const char* HEX = "0123456789abcdef";
    std::string out = std::to_string(rate) + ":";
    for (int i = 0; i < size; ++i)
    {
        out += HEX[config[i] >> 4];
        out += HEX[config[i] & 15];
    }
    return out;
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
    // HE-AAC (5, SBR) and HE-AAC v2 (29, PS) signalled explicitly: the frames are the core codec named after the
    // output sample rate, usually LC, and an ADTS header describes the core. 5 & 3 is 1 and 29 & 3 is 1, so these were
    // given profile 0, AAC Main, which no decoder of HE-AAC expects.
    if (object_type == 5 || object_type == 29)
    {
        if (reader.getBits<uint8_t>(4) == 0x0f)  // extensionSamplingFrequencyIndex
            reader.skipBits(24);
        auto core = reader.getBits<uint8_t>(5);
        if (core == 31)
            core = 32 + reader.getBits<uint8_t>(6);
        if (core >= 1 && core <= 4)
            m_profile = core - 1;
        object_type = core;
    }
    // Channel configuration 0 of a general audio object: a program config element follows in its GASpecificConfig.
    // It was not read, so such a track had no channels and gave empty frames, an empty output said to be successful.
    AacProgramConfig pce;
    static constexpr uint32_t generalAudio = 1u << 1 | 1u << 2 | 1u << 3 | 1u << 4 | 1u << 6 | 1u << 7 | 1u << 17 |
                                             1u << 19 | 1u << 20 | 1u << 21 | 1u << 22 | 1u << 23;
    if (m_channels_index == 0 && object_type < 32 && (generalAudio >> object_type & 1))
    {
        try
        {
            reader.skipBit();  // frameLengthFlag
            if (reader.getBit())
                reader.skipBits(14);  // dependsOnCoreCoder, coreCoderDelay
            reader.skipBit();         // extensionFlag
            if (readProgramConfig(reader, pce))
                m_channels = static_cast<uint8_t>(pce.channels);
        }
        catch (BitStreamException&)
        {
            pce = {};
        }
    }
    setPce(pce);
    // return specific_config_bitindex;
}
