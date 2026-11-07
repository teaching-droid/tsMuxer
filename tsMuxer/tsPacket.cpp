// Changed by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.

#ifndef _WIN32
#endif
#include "tsPacket.h"

#include <fs/file.h>
#include <fs/systemlog.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "bitStream.h"
#include "crc32.h"
#include "h264StreamReader.h"
#include "metaDemuxer.h"
#include "mpegStreamReader.h"
#include "simplePacketizerReader.h"
#include "tsMuxer.h"
#include "vodCoreException.h"

using namespace std;

bool isVideoStreamType(const StreamType stream_coding_type)
{
    switch (stream_coding_type)
    {
    case StreamType::VIDEO_MPEG2:
    case StreamType::VIDEO_H264:
    case StreamType::VIDEO_VC1:
    case StreamType::VIDEO_MVC:
    case StreamType::VIDEO_H265:
    case StreamType::VIDEO_H266:
        return true;
    default:
        return false;
    }
}

bool isAudioStreamType(const StreamType stream_coding_type)
{
    switch (stream_coding_type)
    {
    case StreamType::AUDIO_LPCM:
    case StreamType::AUDIO_AC3:
    case StreamType::AUDIO_DTS:
    case StreamType::AUDIO_TRUE_HD:
    case StreamType::AUDIO_EAC3:
    case StreamType::AUDIO_EAC3_ATSC:
    case StreamType::AUDIO_DTS_HD:
    case StreamType::AUDIO_DTS_HD_MA:
    case StreamType::AUDIO_EAC3_SECONDARY:
    case StreamType::AUDIO_DTS_HD_SECONDARY:
    case StreamType::AUDIO_AAC:
    case StreamType::AUDIO_AAC_RAW:
    case StreamType::AUDIO_MPEG1:
    case StreamType::AUDIO_MPEG2:
        return true;
    default:
        return false;
    }
}

// ------------ PS PACK -------------------
bool PS_stream_pack::deserialize(uint8_t* buffer, const int buf_size)
{
    try
    {
        m_pts = 0;
        BitStreamReader bitReader{};
        bitReader.setBuffer(buffer, buffer + buf_size);
        if (bitReader.getBits(2) != 1)
            return false;  // 0b01 required
        m_pts = bitReader.getBits(3) << 30;
        if (bitReader.getBit() != 1)
            return false;
        m_pts += bitReader.getBits(15) << 15;
        if (bitReader.getBit() != 1)
            return false;
        m_pts += bitReader.getBits(15);
        if (bitReader.getBit() != 1)
            return false;
        m_pts_ext = bitReader.getBits(9);
        if (bitReader.getBit() != 1)
            return false;
        m_program_mux_rate = bitReader.getBits(22) * 50 * 8;  // convert to bits/sec
        if (bitReader.getBits(2) != 3)
            return false;
        bitReader.skipBits(5);  // reserved
        m_pack_stuffing_length = bitReader.getBits(3);
        return true;
    }
    catch (BitStreamException&)
    {
        return false;
    }
}

// ------------- PAT -----------------------

namespace
{
int get16(uint8_t** pp)
{
    uint8_t* p = *pp;
    const int c = (p[0] << 8) | p[1];
    p += 2;
    *pp = p;
    return c;
}
}  // namespace

TS_program_association_section::TS_program_association_section() : transport_stream_id(0)
{
    m_nitPID = -1;
    // av_crc_init(tmpAvCrc, 0, 32, 0x04c11db7, sizeof(AVCRC)*257);
}

bool TS_program_association_section::deserialize(uint8_t* buffer, const int buf_size)
{
    m_nitPID = -1;
    buffer++;
    BitStreamReader bitReader{};
    try
    {
        bitReader.setBuffer(buffer, buffer + buf_size);

        const auto table_id = bitReader.getBits<uint8_t>(8);
        if (table_id != 0x0)
            return false;

        const auto indicator = bitReader.getBits<uint8_t>(2);  // section syntax indicator and reserved '0' bit
        if (indicator != 2)
            return false;
        bitReader.skipBits(2);  // reserved

        const auto section_length = bitReader.getBits<uint16_t>(12);
        const int crcBit = bitReader.getBitsCount() + (section_length - 4) * 8;

        transport_stream_id = bitReader.getBits<uint16_t>(16);
        bitReader.skipBits(2);  // reserved
        bitReader.skipBits(5);  // int version_number
        bitReader.skipBit();    // int current_next_indicator

        bitReader.skipBits(8);  // int section_number
        bitReader.skipBits(8);  // int last_section_number

        // Taken over only when the section could be read: a damaged PAT leaves the map of the one before
        std::map<int, int> programs;
        std::map<int, std::set<int>> programsOnPid;

        while (bitReader.getBitsCount() < crcBit)
        {
            const auto program_number = bitReader.getBits<uint16_t>(16);
            bitReader.skipBits(3);  // reserved
            const auto program_pid = bitReader.getBits<uint16_t>(13);
            if (program_number != 0)  // not a network pid
            {
                programs[program_pid] = program_number;
                programsOnPid[program_pid].insert(program_number);
            }
            else
                m_nitPID = program_pid;
        }
        pmtPids = std::move(programs);
        programsOfPid = std::move(programsOnPid);
        return bitReader.getBitsCount() == crcBit;
    }
    catch (BitStreamException&)
    {
        return false;
    }
}

uint32_t TS_program_association_section::serialize(uint8_t* buffer, const int buf_size)
{
    buffer[0] = 0;
    buffer++;
    BitStreamWriter bitWriter{};

    // init_bitWriter.putBits(buffer, buf_size*8);
    bitWriter.setBuffer(buffer, buffer + buf_size);

    bitWriter.putBits(8, 0);
    bitWriter.putBits(2, 2);  // indicator
    bitWriter.putBits(2, 3);  // reserved

    const auto section_length = static_cast<unsigned>(9 + pmtPids.size() * 4);
    bitWriter.putBits(12, section_length);
    bitWriter.putBits(16, transport_stream_id);
    bitWriter.putBits(2, 3);  // reserved
    bitWriter.putBits(5, 0);  // version
    bitWriter.putBits(1, 1);  // current next indicator

    bitWriter.putBits(16, 0);  // section and last section number
    for (const auto [fst, snd] : pmtPids)
    {
        bitWriter.putBits(16, snd);  // program number
        bitWriter.putBits(3, 7);     // current next indicator
        bitWriter.putBits(13, fst);  // pid
    }
    bitWriter.flushBits();
    const uint32_t crc = calculateCRC32(buffer, bitWriter.getBitsCount() / 8);
    const auto crcPtr = UnalignedPtr<uint32_t>(buffer + bitWriter.getBitsCount() / 8);
    *crcPtr = my_htonl(crc);

    return bitWriter.getBitsCount() / 8 + 5;
}

// ------------- PMT ------------------------

TS_program_map_section::TS_program_map_section()
    : video_pid(0), audio_pid(0), sub_pid(0), pcr_pid(0), casPID(0), casID(0), program_number(0)
{
    video_type = -1;
    audio_type = -1;
}

bool TS_program_map_section::isFullBuff(uint8_t* buffer, const int buf_size)
{
    const uint8_t pointerField = *buffer;
    const uint8_t* bufEnd = buffer + buf_size;
    BitStreamReader bitReader{};
    try
    {
        bitReader.setBuffer(buffer + 1 + pointerField, buffer + buf_size);

        const auto table_id = bitReader.getBits<uint8_t>(8);
        if (table_id != 0x02)
            return false;

        const auto indicator = bitReader.getBits<uint8_t>(2);  // section syntax indicator and reserved '0' bit
        if (indicator != 2)
            return false;
        bitReader.skipBits(2);  // reserved

        const auto section_length = bitReader.getBits<uint16_t>(12);
        return bitReader.getBuffer() + bitReader.getBitsCount() / 8 + section_length <= bufEnd;
    }
    catch (BitStreamException&)
    {
        return false;
    }
}

void TS_program_map_section::extractPMTDescriptors(uint8_t* curPos, const int es_info_len)
{
    const uint8_t* end = curPos + es_info_len;
    while (curPos < end)
    {
        const auto tag = static_cast<TSDescriptorTag>(*curPos);
        const uint8_t len = curPos[1];
        curPos += 2;
        if (tag == TSDescriptorTag::CAS && len >= 4)
        {
            casID = (curPos[0] << 8) + curPos[1];
            casPID = ((curPos[2] & 0x0f) << 8) + curPos[3];
        }
        curPos += len;
    }
}

bool TS_program_map_section::deserialize(uint8_t* buffer, const int buf_size)
{
    if (buf_size < 1)
        return false;
    const uint8_t pointerField = *buffer;
    const uint8_t* bufferEnd = buffer + buf_size;
    BitStreamReader bitReader{};
    try
    {
        bitReader.setBuffer(buffer + 1 + pointerField, buffer + buf_size);

        const auto table_id = bitReader.getBits<uint8_t>(8);
        if (table_id != 0x02)
            return false;

        const auto indicator = bitReader.getBits<uint8_t>(2);  // section syntax indicator and reserved '0' bit
        if (indicator != 2)
            return false;
        bitReader.skipBits(2);  // reserved

        const auto section_length = bitReader.getBits<uint16_t>(12);
        const uint8_t* crcPos = bitReader.getBuffer() + bitReader.getBitsCount() / 8 + section_length - 4;
        if (crcPos > bufferEnd)
        {
            LTRACE(LT_WARN, 0, "Bad PMT table. skipped");
            return false;
        }

        program_number = bitReader.getBits<uint16_t>(16);
        // reserved, version_number, current_next_indicator, section_number, last_section_number
        bitReader.skipBits(27);
        pcr_pid = bitReader.getBits<uint16_t>(13);

        // we set video=pcr pid by default.
        // the video PID is not available in the scrambled channel veriMatrix 239.255.2.58:5500.
        // Is that similar to the es_info_len crap?
        video_pid = pcr_pid;

        bitReader.skipBits(4);  // reserved
        const auto program_info_len = bitReader.getBits<uint16_t>(12);
        uint8_t* curPos = bitReader.getBuffer() + bitReader.getBitsCount() / 8;
        if (curPos + program_info_len + 4 > bufferEnd)
        {
            LTRACE(LT_WARN, 0, "Bad PMT table. skipped");
            return false;
        }
        extractPMTDescriptors(curPos, program_info_len);
        curPos += program_info_len;
        while (curPos < crcPos)
        {
            auto stream_type = static_cast<StreamType>(*curPos++);
            int elementary_pid = get16(&curPos) & 0x1fff;
            switch (stream_type)
            {
            case StreamType::VIDEO_MPEG1:
            case StreamType::VIDEO_MPEG2:
            case StreamType::VIDEO_MPEG4:
            case StreamType::VIDEO_H264:
            case StreamType::VIDEO_H265:
            case StreamType::VIDEO_MVC:
            case StreamType::VIDEO_VC1:
                video_pid = elementary_pid;
                video_type = static_cast<int>(stream_type);
                break;
            case StreamType::AUDIO_MPEG1:
            case StreamType::AUDIO_MPEG2:
            case StreamType::AUDIO_AAC:
            case StreamType::AUDIO_AC3:
            case StreamType::AUDIO_EAC3:
            case StreamType::AUDIO_EAC3_ATSC:
            case StreamType::AUDIO_DTS:
                audio_pid = elementary_pid;
                audio_type = static_cast<int>(stream_type);
                break;
            case StreamType::SUB_DVB:
                sub_pid = elementary_pid;
                break;
            default:
                break;
            }
            PMTStreamInfo pmtStreamInfo(stream_type, elementary_pid, nullptr, 0, nullptr, "", false);
            pmtStreamInfo.m_programNumber = program_number;
            const int es_info_len = get16(&curPos) & 0xfff;
            if (curPos + es_info_len > crcPos)
            {
                LTRACE(LT_WARN, 0, "Bad PMT table. skipped");
                return false;
            }
            const uint32_t regId = extractDescriptors(curPos, es_info_len, pmtStreamInfo);

            // Identify PRIVATE_DATA streams by their registration descriptor.
            // 'Opus' (0x4F707573) registration identifies Opus audio.
            if (stream_type == StreamType::PRIVATE_DATA && regId == 0x4F707573 /* 'Opus' */)
            {
                audio_pid = elementary_pid;
                audio_type = static_cast<int>(stream_type);
            }
            if (stream_type == StreamType::PRIVATE_DATA && regId == 0x41563031 /* 'AV01' */)
                pmtStreamInfo.m_av1Registration = true;

            pidList[elementary_pid] = pmtStreamInfo;
            curPos += es_info_len;
        }
        if (curPos != crcPos)
            return false;

        // uint32_t rez = my_htonl(calculateCRC32(buffer, curPos - buffer));
        return true;
    }
    catch (BitStreamException&)
    {
        return false;
    }
}

uint32_t TS_program_map_section::extractDescriptors(uint8_t* curPos, const int es_info_len, PMTStreamInfo& pmtInfo)
{
    uint32_t registrationId = 0;
    const uint8_t* end = curPos + es_info_len;
    while (curPos < end)
    {
        const auto tag = static_cast<TSDescriptorTag>(*curPos);
        const uint8_t len = curPos[1];
        curPos += 2;
        const uint8_t* descrBuf = curPos;

        if (tag == TSDescriptorTag::LANG && len >= 3)
        {
            for (int i = 0; i < 3; i++) pmtInfo.m_lang[i] = static_cast<char>(descrBuf[i]);
        }
        else if (tag == TSDescriptorTag::DVB_SUBTITLING || tag == TSDescriptorTag::DVB_TELETEXT)
        {
            pmtInfo.m_dvbDescriptor = static_cast<int>(tag);
            // The language of a DVB subtitle track is in this descriptor, which as a rule is the only one it has.
            if (tag == TSDescriptorTag::DVB_SUBTITLING && len >= 3 && pmtInfo.m_lang[0] == 0)
                for (int i = 0; i < 3; i++) pmtInfo.m_lang[i] = static_cast<char>(descrBuf[i]);
        }
        else if (tag == TSDescriptorTag::REGISTRATION && len >= 4)
        {
            // format_identifier is 4 bytes (stored as-is, not byte-swapped)
            registrationId = (static_cast<uint32_t>(descrBuf[0]) << 24) | (static_cast<uint32_t>(descrBuf[1]) << 16) |
                             (static_cast<uint32_t>(descrBuf[2]) << 8) | static_cast<uint32_t>(descrBuf[3]);
            if (registrationId == 0x41432D33 /* 'AC-3' */)
                pmtInfo.m_soundDescriptor = static_cast<int>(StreamType::AUDIO_AC3);
            else if (registrationId == 0x45414333 /* 'EAC3' */)
                pmtInfo.m_soundDescriptor = static_cast<int>(StreamType::AUDIO_EAC3_ATSC);
            else if (registrationId == 0x41432D34 /* 'AC-4' */ && pmtInfo.m_ac4Descriptor == 0)
                pmtInfo.m_ac4Descriptor = 0x05;
        }
        else if (tag == TSDescriptorTag::DVB_EXTENSION && len >= 1 && descrBuf[0] == 0x15)  // AC-4_descriptor
            pmtInfo.m_ac4Descriptor = 0x7F;
        else if (static_cast<uint8_t>(tag) == 0x52 && len >= 1)  // stream_identifier_descriptor
            pmtInfo.m_componentTag = descrBuf[0];
        else if (static_cast<uint8_t>(tag) == 0xFD && len >= 2)  // data_component_descriptor (ISDB)
            pmtInfo.m_dataComponentId = descrBuf[0] << 8 | descrBuf[1];
        else if (tag == TSDescriptorTag::DVB_AC3)
            pmtInfo.m_soundDescriptor = static_cast<int>(StreamType::AUDIO_AC3);
        else if (tag == TSDescriptorTag::DVB_EAC3)
            pmtInfo.m_soundDescriptor = static_cast<int>(StreamType::AUDIO_EAC3_ATSC);
        else if (tag == TSDescriptorTag::DVB_DTS)
            pmtInfo.m_soundDescriptor = static_cast<int>(StreamType::AUDIO_DTS);
        else if (tag == TSDescriptorTag::DVB_AAC)
            pmtInfo.m_soundDescriptor = static_cast<int>(StreamType::AUDIO_AAC);
        curPos += len;
    }
    return registrationId;
}

uint32_t TS_program_map_section::serialize(uint8_t* buffer, const int max_buf_size, const bool blurayMode,
                                           const bool hdmvDescriptors)
{
    buffer[0] = 0;
    buffer++;
    BitStreamWriter bitWriter{};
    bitWriter.setBuffer(buffer, buffer + max_buf_size);
    bitWriter.putBits(8, 2);  // table id

    const auto LengthPos1 = UnalignedPtr<uint16_t>(bitWriter.getBuffer() + bitWriter.getBitsCount() / 8);
    bitWriter.putBits(2, 2);   // indicator
    bitWriter.putBits(2, 3);   // reserved
    bitWriter.putBits(12, 0);  // length
    const unsigned beforeCount1 = bitWriter.getBitsCount() / 8;

    bitWriter.putBits(16, program_number);
    bitWriter.putBits(2, 3);         // reserved
    bitWriter.putBits(5, 0);         // version_number
    bitWriter.putBits(1, 1);         // current next indicator
    bitWriter.putBits(16, 0);        // section_number and last_section_number
    bitWriter.putBits(3, 7);         // reserved
    bitWriter.putBits(13, pcr_pid);  // reserved

    const auto LengthPos2 = UnalignedPtr<uint16_t>(bitWriter.getBuffer() + bitWriter.getBitsCount() / 8);
    bitWriter.putBits(4, 15);  // reserved
    bitWriter.putBits(12, 0);  // program info len
    const unsigned beforeCount2 = bitWriter.getBitsCount() / 8;

    if (hdmvDescriptors)
    {
        // put 'HDMV' registration descriptor
        bitWriter.putBits(8, static_cast<uint8_t>(TSDescriptorTag::HDMV));
        bitWriter.putBits(8, 0x04);
        bitWriter.putBits(32, 0x48444d56);

        // put DTCP descriptor
        bitWriter.putBits(8, static_cast<uint8_t>(TSDescriptorTag::COPY_CONTROL));
        bitWriter.putBits(8, 0x04);
        bitWriter.putBits(32, 0x0ffffcfc);
    }

    if (casPID)
    {
        // put CAS descriptor
        bitWriter.putBits(8, static_cast<uint8_t>(TSDescriptorTag::CAS));
        bitWriter.putBits(8, 0x04);
        bitWriter.putBits(16, casID);
        bitWriter.putBits(16, casPID);
    }
    *LengthPos2 = my_htons(static_cast<uint16_t>(0xf000 + bitWriter.getBitsCount() / 8 - beforeCount2));

    if (video_pid)
    {
        bitWriter.putBits(8, video_type);
        bitWriter.putBits(3, 7);  // reserved
        bitWriter.putBits(13, video_pid);
        bitWriter.putBits(4, 15);  // reserved
        bitWriter.putBits(12, 0);  // es_info_len
    }

    if (audio_pid)
    {
        bitWriter.putBits(8, audio_type);
        bitWriter.putBits(3, 7);  // reserved
        bitWriter.putBits(13, audio_pid);
        bitWriter.putBits(4, 15);  // reserved
        bitWriter.putBits(12, 0);  // es_info_len
    }

    if (sub_pid)
    {
        bitWriter.putBits(8, static_cast<uint8_t>(StreamType::SUB_DVB));
        bitWriter.putBits(3, 7);  // reserved
        bitWriter.putBits(13, sub_pid);
        bitWriter.putBits(4, 15);  // reserved
        bitWriter.putBits(12, 0);  // es_info_len
    }

    for (const auto& [pid, si] : pidList)
    {
        // Once a run: the PMT is written again every few tenths of a second, and the warning came with each copy
        static bool saidPgs = false;
        if (si.m_streamType == StreamType::SUB_PGS && !hdmvDescriptors && !saidPgs)
        {
            LTRACE(LT_WARN, 2, "Warning: PGS might not work without HDMV descriptors.");
            saidPgs = true;
        }

        bitWriter.putBits(8, static_cast<int>(si.m_streamType));
        bitWriter.putBits(3, 7);  // reserved
        bitWriter.putBits(13, si.m_pid);

        const auto esInfoLen = UnalignedPtr<uint16_t>(bitWriter.getBuffer() + bitWriter.getBitsCount() / 8);
        bitWriter.putBits(4, 15);  // reserved
        bitWriter.putBits(12, 0);  // es_info_len
        const unsigned beforeCount = bitWriter.getBitsCount() / 8;

        // For codecs whose stream info (e.g. channel count) may not have been
        // available at intAddStream() time, re-query the codec reader now so the
        // PMT always contains up-to-date descriptors.  This is needed for Opus
        // when the OpusHead is parsed after the initial PMT write.
        int curEsInfoLen = si.m_esInfoLen;
        const uint8_t* curEsInfoData = si.m_esInfoData;
        uint8_t refreshedDescr[TS_ES_INFO_MAX];
        if (si.m_codecReader != nullptr)
        {
            const int freshLen = si.m_codecReader->getTSDescriptor(refreshedDescr, blurayMode, hdmvDescriptors);
            if (freshLen > 0)
            {
                curEsInfoLen = freshLen;
                curEsInfoData = refreshedDescr;
            }
        }
        for (int j = 0; j < curEsInfoLen; j++) bitWriter.putBits(8, curEsInfoData[j]);
        // ** A STREAM TYPE OF BLU-RAY NEEDS A NAME WITHOUT THE HDMV DESCRIPTORS. ** Stream types from 0x80 up are user
        // private (H.222.0 Table 2-34): what they mean is said by a registration descriptor, the program's "HDMV" or
        // the stream's own ("AC-3", "DTS1", "mlpa", ...). With --no-hdmv-descriptors the program's is left out, and a
        // stream without one of its own is named by nothing. That was said for PGS only (above); DTS Express, which has
        // no DTS core and so gets no DTS descriptor, went out as stream type 0x85 or 0xA2 without a word.
        if (!hdmvDescriptors && si.m_streamType != StreamType::SUB_PGS && static_cast<int>(si.m_streamType) >= 0x80)
        {
            bool named = false;
            for (int j = 0; j + 1 < curEsInfoLen; j += 2 + curEsInfoData[j + 1])
                named = named || curEsInfoData[j] == static_cast<uint8_t>(TSDescriptorTag::REGISTRATION);
            static std::set<int> said;  // once for each track: the PMT is written again and again
            if (!named && said.insert(si.m_pid).second)
                LTRACE(LT_WARN, 2,
                       "Warning: the track on PID " << si.m_pid << " (stream type 0x" << std::hex
                                                    << static_cast<int>(si.m_streamType) << std::dec
                                                    << ") might not work without HDMV descriptors: its stream type "
                                                       "is one only Blu-ray defines, and no descriptor names its "
                                                       "codec.");
        }

        if (*si.m_lang && !blurayMode)
        {
            bitWriter.putBits(8, static_cast<unsigned>(TSDescriptorTag::LANG));  // lang descriptor ID
            bitWriter.putBits(8, 4);                                             // lang descriptor len
            for (int k = 0; k < 3; k++) bitWriter.putBits(8, si.m_lang[k]);      // lang code[i]
            bitWriter.putBits(8, 0);
        }
        *esInfoLen = my_htons(static_cast<uint16_t>(0xf000 + bitWriter.getBitsCount() / 8 - beforeCount));
    }
    *LengthPos1 = my_htons(static_cast<uint16_t>(0xb000 + bitWriter.getBitsCount() / 8 - beforeCount1 + 4));
    bitWriter.flushBits();

    const uint32_t crc = calculateCRC32(buffer, bitWriter.getBitsCount() / 8);

    const auto crcPtr = UnalignedPtr<uint32_t>(buffer + bitWriter.getBitsCount() / 8);
    *crcPtr = my_htonl(crc);

    return bitWriter.getBitsCount() / 8 + 5;
}

// --------------------- CLPIParser -----------------------------

void CLPIStreamInfo::ISRC(BitStreamReader& reader)
{
    readString(country_code, reader, 2);
    readString(copyright_holder, reader, 3);
    readString(recording_year, reader, 2);
    readString(recording_number, reader, 5);
}

// No ISRC is known for a stream, and a disc without one has zero bytes there, not the character "0"
void CLPIStreamInfo::composeISRC(BitStreamWriter& writer)
{
    // country_code, copyright_holder, recording_year, recording_number
    for (int i = 0; i < 12; i++) writer.putBits(8, 0);
}

void CLPIStreamInfo::parseStreamCodingInfo(BitStreamReader& reader)
{
    reader.skipBits(8);  // length
    stream_coding_type = static_cast<StreamType>(reader.getBits(8));

    if (isVideoStreamType(stream_coding_type))
    {
        video_format = reader.getBits<uint8_t>(4);
        frame_rate_index = reader.getBits<uint8_t>(4);
        aspect_ratio_index = reader.getBits<uint8_t>(4);
        reader.skipBits(20);  // reserved_for_future_use, cc_flag
        ISRC(reader);
        reader.skipBits(32);  // reserved_for_future_use
    }
    else if (isAudioStreamType(stream_coding_type))
    {
        audio_presentation_type = reader.getBits<uint8_t>(4);
        sampling_frequency_index = reader.getBits<uint8_t>(4);
        readString(language_code, reader, 3);
        ISRC(reader);
        reader.skipBits(32);
    }
    else if (stream_coding_type == StreamType::SUB_PGS || stream_coding_type == StreamType::SUB_IGS)
    {
        readString(language_code, reader, 3);
        reader.skipBits(8);  // reserved_for_future_use
        ISRC(reader);
        reader.skipBits(32);  // reserved_for_future_use
    }
    else if (stream_coding_type == StreamType::SUB_TGS)
    {
        // Text subtitle stream
        character_code = reader.getBits<uint8_t>(8);
        readString(language_code, reader, 3);
        ISRC(reader);
        reader.skipBits(32);  // reserved_for_future_use
    }
}

void CLPIStreamInfo::composeStreamCodingInfo(BitStreamWriter& writer) const
{
    uint8_t* lengthPos = writer.getBuffer() + writer.getBitsCount() / 8;
    writer.putBits(8, 0);  // length
    const unsigned beforeCount = writer.getBitsCount() / 8;

    writer.putBits(8, static_cast<int>(stream_coding_type));

    if (isVideoStreamType(stream_coding_type))
    {
        writer.putBits(4, video_format);
        writer.putBits(4, frame_rate_index);
        writer.putBits(4, aspect_ratio_index);
        writer.putBits(2, 0);  // reserved_for_future_use
        writer.putBit(0);      // cc_flag
        writer.putBit(0);      // reserved
        if (HDR & 18)
            writer.putBits(8, 0x12);  // HDR10 or HDR10plus
        else if (HDR == 4)
            writer.putBits(8, 0x22);  // DV
        else
            writer.putBits(8, colour_space);  // SDR: 1 for BT.709 HEVC, 2 for BT.2020 HEVC, else 0
        if (HDR == 16)
            writer.putBits(8, 0x80);  // HDR10plus
        else
            writer.putBits(8, 0);
        composeISRC(writer);
        writer.putBits(32, 0);  // reserved_for_future_use
    }
    else if (isAudioStreamType(stream_coding_type))
    {
        writer.putBits(4, audio_presentation_type);
        writer.putBits(4, sampling_frequency_index);
        writeString(language_code, writer, 3);
        composeISRC(writer);
        writer.putBits(32, 0);  // reserved_for_future_use
    }
    else if (stream_coding_type == StreamType::SUB_PGS || stream_coding_type == StreamType::SUB_IGS)
    {
        writeString(language_code, writer, 3);
        writer.putBits(8, 0);  // reserved_for_future_use
        composeISRC(writer);
        writer.putBits(32, 0);  // reserved_for_future_use
    }
    else if (stream_coding_type == StreamType::SUB_TGS)
    {
        // Text subtitle stream
        writer.putBits(8, character_code);
        writeString(language_code, writer, 3);
        composeISRC(writer);
        writer.putBits(32, 0);  // reserved_for_future_use
    }
    *lengthPos = static_cast<uint8_t>(writer.getBitsCount() / 8 - beforeCount);
}

void CLPIParser::parseProgramInfo(uint8_t* buffer, const uint8_t* end, std::vector<CLPIProgramInfo>& programInfoMap,
                                  std::map<int, CLPIStreamInfo>& streamInfoMap)
{
    BitStreamReader reader{};
    reader.setBuffer(buffer, end);
    reader.skipBits(32);  // length
    reader.skipBits(8);   // reserved_for_word_align
    const auto number_of_program_sequences = reader.getBits<uint8_t>(8);
    for (int i = 0; i < number_of_program_sequences; i++)
    {
        programInfoMap.emplace_back();
        programInfoMap[i].SPN_program_sequence_start = reader.getBits(32);
        programInfoMap[i].program_map_PID = reader.getBits<uint16_t>(16);
        programInfoMap[i].number_of_streams_in_ps = reader.getBits<uint8_t>(8);
        reader.skipBits(8);
        for (int stream_index = 0; stream_index < programInfoMap[i].number_of_streams_in_ps; stream_index++)
        {
            const auto pid = reader.getBits<uint16_t>(16);
            CLPIStreamInfo streamInfo;
            streamInfo.parseStreamCodingInfo(reader);
            streamInfoMap[pid] = streamInfo;
        }
    }
}

void CLPIParser::composeProgramInfo(BitStreamWriter& writer, const bool isSsExt)
{
    const auto lengthPos = UnalignedPtr<uint32_t>(writer.getBuffer() + writer.getBitsCount() / 8);
    writer.putBits(32, 0);  // length

    const int beforeCount = writer.getBitsCount() / 8;

    writer.putBits(8, 0);  // reserved
    writer.putBits(8, 1);  // number_of_program_sequences = 1
    // for (int i=0; i < number_of_program_sequences; i++)
    {
        writer.putBits(32, 0);                // m_programInfo[i].SPN_program_sequence_start
        writer.putBits(16, DEFAULT_PMT_PID);  // m_programInfo[i].program_map_PID =

        int streams = 0;
        for (const auto& [index, si] : m_streamInfo)
        {
            const bool streamOK = (isSsExt && si.stream_coding_type == StreamType::VIDEO_MVC) ||
                                  (!isSsExt && si.stream_coding_type != StreamType::VIDEO_MVC);
            if (streamOK)
                streams++;
        }

        writer.putBits(8, streams);  // m_programInfo[i].number_of_streams_in_ps
        writer.putBits(8, 0);        // reserved_for_future_use
        // for (int i=0; i < m_streamInfo.size(); i++)
        for (const auto& [index, si] : m_streamInfo)
        {
            const bool streamOK = (isSsExt && si.stream_coding_type == StreamType::VIDEO_MVC) ||
                                  (!isSsExt && si.stream_coding_type != StreamType::VIDEO_MVC);
            if (!streamOK)
                continue;
            writer.putBits(16, index);  // pid
            si.composeStreamCodingInfo(writer);
        }
    }

    if (isSsExt && (writer.getBitsCount() - m_extensionDataStart) % 32 != 0)
        writer.putBits(16, 0);

    *lengthPos = my_htonl(writer.getBitsCount() / 8 - beforeCount);
}

void CLPIParser::TS_type_info_block(BitStreamReader& reader)
{
    reader.skipBits(24);                                       // length // Validity_flags, 1000 0000b is typical value
    CLPIStreamInfo::readString(format_identifier, reader, 4);  // HDMV
    // Network_information 8*9
    for (int i = 0; i < 9; i++) reader.skipBits(8);
    // Stream_format_name 8*16
    for (int i = 0; i < 4; i++) reader.skipBits(32);
}

void CLPIParser::composeTS_type_info_block(BitStreamWriter& writer)
{
    const auto lengthPos = UnalignedPtr<uint16_t>(writer.getBuffer() + writer.getBitsCount() / 8);
    writer.putBits(16, 0);  // length
    const int beforeCount = writer.getBitsCount() / 8;

    writer.putBits(8, 0x80);  // Validity_flags
    CLPIStreamInfo::writeString("HDMV", writer, 4);
    // Network_information 8*9
    for (int i = 0; i < 9; i++) writer.putBits(8, 0);
    // Stream_format_name 8*16
    for (int i = 0; i < 4; i++) writer.putBits(32, 0);
    *lengthPos = my_ntohs(static_cast<uint16_t>(writer.getBitsCount() / 8 - beforeCount));
}

void CLPIParser::parseClipInfo(BitStreamReader& reader)
{
    reader.skipBits(32);                            // length
    reader.skipBits(16);                            // reserved_for_future_use
    clip_stream_type = reader.getBits<uint8_t>(8);  // 1 - AV stream
    application_type = reader.getBits<uint8_t>(8);  // 1 - Main TS for a main-path of Movie
    reader.skipBits(31);                            // reserved_for_future_use
    is_ATC_delta = reader.getBit();
    TS_recording_rate = reader.getBits(32);         // kbps in bytes/sec
    number_of_source_packets = reader.getBits(32);  // number of TS packets?
    for (int i = 0; i < 32; i++) reader.skipBits(32);
    TS_type_info_block(reader);
}

void CLPIParser::composeClipInfo(BitStreamWriter& writer) const
{
    const auto lengthPos = UnalignedPtr<uint32_t>(writer.getBuffer() + writer.getBitsCount() / 8);
    writer.putBits(32, 0);  // length
    const int beforeCount = writer.getBitsCount() / 8;

    writer.putBits(16, 0);                // reserved_for_future_use
    writer.putBits(8, clip_stream_type);  // 1 - AV stream
    writer.putBits(8, application_type);  // 1 - Main TS for a main-path of Movie
    writer.putBits(31, 0);                // reserved_for_future_use
    writer.putBit(is_ATC_delta);
    writer.putBits(32, TS_recording_rate);               // kbps in bytes/sec
    writer.putBits(32, number_of_source_packets);        // number of TS packets?
    for (int i = 0; i < 32; i++) writer.putBits(32, 0);  // reserved
    composeTS_type_info_block(writer);
    if (is_ATC_delta)
        THROW(ERR_COMMON, "CLPI is_ATC_delta is not implemented yet.")
    if (application_type == 6)
        THROW(ERR_COMMON, "CLPI application_type==6 is not implemented yet.")
    *lengthPos = my_htonl(writer.getBitsCount() / 8 - beforeCount);
}

void CLPIParser::parseSequenceInfo(uint8_t* buffer, const uint8_t* end)
{
    BitStreamReader reader{};
    reader.setBuffer(buffer, end);
    reader.skipBits(32);                                              // length
    reader.skipBits(8);                                               // reserved_for_word_align
    const auto number_of_ATC_sequences = reader.getBits<uint8_t>(8);  // 1 is tipical value
    for (uint8_t atc_id = 0; atc_id < number_of_ATC_sequences; atc_id++)
    {
        reader.skipBits(32);  // SPN_ATC_start, 0 is tipical value
        const auto number_of_STC_sequences = reader.getBits<uint8_t>(8);
        const auto offset_STC_id = reader.getBits<uint8_t>(8);
        for (uint8_t stc_id = offset_STC_id; stc_id < number_of_STC_sequences + offset_STC_id; stc_id++)
        {
            reader.skipBits(16);  // PCR_PID
            reader.skipBits(32);  // SPN_STC_start
            presentation_start_time = reader.getBits(32);
            presentation_end_time = reader.getBits(32);
        }
    }
}

void CLPIParser::composeSequenceInfo(BitStreamWriter& writer) const
{
    const auto lengthPos = UnalignedPtr<uint32_t>(writer.getBuffer() + writer.getBitsCount() / 8);
    writer.putBits(32, 0);  // length
    const int beforeCount = writer.getBitsCount() / 8;

    writer.putBits(8, 0);  // reserved_for_word_align
    writer.putBits(8, 1);  // number_of_ATC_sequences
    // for (int atc_id = 0; atc_id < number_of_ATC_sequences; atc_id++)
    {
        writer.putBits(32, 0);  // SPN_ATC_start
        writer.putBits(8, 1);   // number_of_STC_sequences
        writer.putBits(8, 0);   // offset_STC_id
        // for (int stc_id=offset_STC_id; stc_id < number_of_STC_sequences + offset_STC_id; stc_id++)
        {
            // The clip's own clock PID. It was always 4097, also when the clock rides on the video PID, which is the
            // command line's default.
            writer.putBits(16, pcr_pid);
            writer.putBits(32, 0);  // SPN_STC_start
            writer.putBits(32, presentation_start_time);
            writer.putBits(32, presentation_end_time);
        }
    }
    *lengthPos = my_htonl(writer.getBitsCount() / 8 - beforeCount);
}

void CLPIParser::parseCPI(uint8_t* buffer, const uint8_t* end)
{
    BitStreamReader reader{};
    reader.setBuffer(buffer, end);
    if (reader.getBits(32) != 0)  // length
        reader.skipBits(16);      // reserved_for_word_align, CPI_type (1 is typical value)
}

void CLPIParser::EP_map(BitStreamReader& reader) {}

bool readClipEntryPoints(const std::string& clpiFileName, int& videoPid, std::vector<ClipEntryPoint>& points)
{
    points.clear();
    File file;
    int64_t size = 0;
    if (!file.open(clpiFileName.c_str(), File::ofRead) || !file.size(&size) || size < 40 || size > 64 * 1024 * 1024)
        return false;
    std::vector<uint8_t> d(static_cast<size_t>(size));
    if (file.read(d.data(), static_cast<uint32_t>(size)) != static_cast<int>(size))
        return false;
    const auto be = [&d](const size_t pos, const size_t bytes, uint32_t& value)
    {
        if (pos + bytes > d.size())
            return false;
        value = 0;
        for (size_t i = 0; i < bytes; ++i) value = value << 8 | d[pos + i];
        return true;
    };
    uint32_t seqStart = 0, cpiStart = 0, cpiLen = 0, v = 0;
    if (memcmp(d.data(), "HDMV", 4) != 0 || !be(8, 4, seqStart) || !be(16, 4, cpiStart))
        return false;
    // A clip with more than one sequence of times can give the same time to two places, and an entry point could not
    // be told apart by its time: such a clip is not cut
    uint32_t atcCount = 0, stcCount = 0;
    if (!be(seqStart + 5, 1, atcCount) || atcCount != 1 || !be(seqStart + 10, 1, stcCount) || stcCount != 1)
        return false;
    if (!be(cpiStart, 4, cpiLen) || cpiLen == 0)
        return false;
    // EP_map: the streams it indexes, 12 bytes each after the count, then for each stream its coarse entries (8 bytes)
    // and its fine entries (4 bytes). The coarse entry keeps bits 19 to 32 of a time and the fine one bits 9 to 19.
    const size_t epMap = cpiStart + 6;
    uint32_t streams = 0;
    if (!be(epMap + 1, 1, streams))
        return false;
    for (uint32_t k = 0; k < streams; ++k)
    {
        const size_t entry = epMap + 2 + 12 * k;
        uint32_t pid = 0, typeAndCoarse = 0, fineCount = 0, start = 0;
        if (!be(entry, 2, pid) || !be(entry + 2, 4, typeAndCoarse) || !be(entry + 5, 3, fineCount) ||
            !be(entry + 8, 4, start))
            return false;
        const uint32_t epStreamType = typeAndCoarse >> 18 & 0x0f;
        const uint32_t coarseCount = typeAndCoarse >> 2 & 0xffff;
        fineCount &= 0x3ffff;
        if (epStreamType != 1)  // a video stream
            continue;
        const size_t one = epMap + start;
        uint32_t fineStart = 0;
        if (!be(one, 4, fineStart))
            return false;
        for (uint32_t c = 0; c < coarseCount; ++c)
        {
            uint32_t coarse = 0, coarseSpn = 0, nextFine = fineCount;
            if (!be(one + 4 + 8 * c, 4, coarse) || !be(one + 8 + 8 * c, 4, coarseSpn))
                return false;
            if (c + 1 < coarseCount && be(one + 4 + 8 * (c + 1), 4, v))
                nextFine = v >> 14;
            for (uint32_t f = coarse >> 14; f < nextFine && f < fineCount; ++f)
            {
                uint32_t fine = 0;
                if (!be(one + fineStart + 4 * f, 4, fine))
                    return false;
                const int64_t pts =
                    (static_cast<int64_t>(coarse & 0x3ffe) << 19) + (static_cast<int64_t>(fine >> 17 & 0x7ff) << 9);
                points.push_back({pts, (coarseSpn & ~0x1ffffu) + (fine & 0x1ffff)});
            }
        }
        videoPid = static_cast<int>(pid);
        return !points.empty();
    }
    return false;
}

void CLPIParser::composeCPI(BitStreamWriter& writer, const bool isCPIExt)
{
    const auto lengthPos = UnalignedPtr<uint32_t>(writer.getBuffer() + writer.getBitsCount() / 8);
    writer.putBits(32, 0);  // length

    if ((isDependStream && !isCPIExt) || (!isDependStream && isCPIExt))
        return;  // CPI_SS for MVC depend stream only and vice versa: standard CPI for standard video stream

    const unsigned beforeCount = writer.getBitsCount() / 8;

    writer.putBits(12, 0);  // reserved_for_word_align
    writer.putBits(4, 1);   // CPI_type
    composeEP_map(writer, isCPIExt);

    if (isCPIExt && (writer.getBitsCount() - m_extensionDataStart) % 32 != 0)
        writer.putBits(16, 0);

    *lengthPos = my_htonl(writer.getBitsCount() / 8 - beforeCount);
}

void CLPIParser::composeEP_map(BitStreamWriter& writer, const bool isSSExt)
{
    const uint32_t beforeCount = writer.getBitsCount() / 8;
    std::vector<CLPIStreamInfo> processStream;
    int EP_stream_type = 1;
    for (auto& [index, si] : m_streamInfo)
    {
        const StreamType coding_type = si.stream_coding_type;
        if (isSSExt)
        {
            if (coding_type == StreamType::VIDEO_MVC)
                processStream.push_back(si);
        }
        else
        {
            if (coding_type != StreamType::VIDEO_MVC && isVideoStreamType(coding_type))
                processStream.push_back(si);
        }
    }
    if (processStream.empty())
        for (const auto& [index, si] : m_streamInfo)
        {
            const StreamType coding_type = si.stream_coding_type;
            if (isAudioStreamType(coding_type))
            {
                processStream.push_back(si);
                if (si.isSecondary)
                    EP_stream_type = 4;
                else
                    EP_stream_type = 3;
                break;
            }
        }
    if (processStream.empty())
        THROW(ERR_COMMON, "Can't create EP map. One audio or video stream is needed.")
    // ------------------
    writer.putBits(8, 0);                                       // reserved_for_word_align
    writer.putBits(8, static_cast<int>(processStream.size()));  // number_of_stream_PID_entries
    std::vector<UnalignedPtr<uint32_t>> epStartAddrPos;

    for (auto& i : processStream)
    {
        writer.putBits(16, i.streamPID);  // stream_PID[k]
        writer.putBits(10, 0);            // reserved_for_word_align
        writer.putBits(4, EP_stream_type);
        std::vector<BluRayCoarseInfo> coarseInfo = buildCoarseInfo(i);
        writer.putBits(16, static_cast<int>(coarseInfo.size()));  // number_of_EP_coarse_entries[k]
        // A stream has an index for each clip up to the last one it has an entry in. With clips of a few bytes each
        // (--split-size) the later clips have none, and the index of a clip past the list was read past its end.
        if (m_clpiNum < i.m_index.size())
            writer.putBits(18, static_cast<int>(i.m_index[m_clpiNum].size()));  // number_of_EP_fine_entries[k]
        else
            writer.putBits(18, 0);
        epStartAddrPos.push_back(UnalignedPtr<uint32_t>(writer.getBuffer() + writer.getBitsCount() / 8));
        writer.putBits(32, 0);  // EP_map_for_one_stream_PID_start_address[k]
    }
    if (writer.getBitsCount() % 16 != 0)
        writer.putBits(8, 0);  // padding_word

    for (size_t i = 0; i < processStream.size(); ++i)
    {
        *epStartAddrPos[i] = my_htonl(writer.getBitsCount() / 8 - beforeCount);
        composeEP_map_for_one_stream_PID(writer, processStream[i]);
        if (writer.getBitsCount() % 16 != 0)
            writer.putBits(8, 0);  // padding_word
    }
}

std::vector<BluRayCoarseInfo> CLPIParser::buildCoarseInfo(M2TSStreamInfo& streamInfo) const
{
    std::vector<BluRayCoarseInfo> rez;
    if (m_clpiNum >= streamInfo.m_index.size())
        return rez;
    uint32_t cnt = 0;
    int64_t lastPktCnt = 0;
    int64_t lastCoarsePts = 0;
    PMTIndex& curIndex = streamInfo.m_index[m_clpiNum];
    for (const auto& [fst, snd] : curIndex)
    {
        const PMTIndexData& indexData = snd;
        const auto newCoarsePts = static_cast<uint32_t>(fst >> 19);
        const uint32_t lastCoarseSPN = lastPktCnt & 0xfffe0000;
        const uint32_t newCoarseSPN = indexData.m_pktCnt & 0xfffe0000;
        if (rez.empty() || newCoarsePts != lastCoarsePts || lastCoarseSPN != newCoarseSPN)
        {
            rez.emplace_back(newCoarsePts, cnt, indexData.m_pktCnt);
        }
        lastCoarsePts = newCoarsePts;
        lastPktCnt = indexData.m_pktCnt;
        cnt++;
    }
    return rez;
}

void CLPIParser::composeEP_map_for_one_stream_PID(BitStreamWriter& writer, M2TSStreamInfo& streamInfo) const
{
    const auto epFineStartAddr = UnalignedPtr<uint32_t>(writer.getBuffer() + writer.getBitsCount() / 8);
    const uint32_t beforePos = writer.getBitsCount() / 8;
    writer.putBits(32, 0);  // EP_fine_table_start_address
    const std::vector<BluRayCoarseInfo> coarseInfo = buildCoarseInfo(streamInfo);
    for (const auto& i : coarseInfo)
    {
        writer.putBits(18, i.m_fineRefID);  // ref_to_EP_fine_id[i]
        writer.putBits(14, i.m_coarsePts);  // PTS_EP_coarse[i]
        writer.putBits(32, i.m_pktCnt);     // SPN_EP_coarse[i]
    }
    if (writer.getBitsCount() % 16 != 0)
        writer.putBits(8, 0);  // padding_word
    *epFineStartAddr = my_htonl(writer.getBitsCount() / 8 - beforePos);
    if (m_clpiNum < streamInfo.m_index.size())
    {
        const PMTIndex& curIndex = streamInfo.m_index[m_clpiNum];
        for (const auto& [fst, snd] : curIndex)
        {
            const PMTIndexData& indexData = snd;
            writer.putBit(0);  // is_angle_change_point[EP_fine_id]
            int endCode = 0;
            if (indexData.m_frameLen > 0)
            {
                if (is4K())
                {
                    if (indexData.m_frameLen < 786432)
                        endCode = 1;
                    else if (indexData.m_frameLen < 1572864)
                        endCode = 2;
                    else if (indexData.m_frameLen < 2359296)
                        endCode = 3;
                    else if (indexData.m_frameLen < 3145728)
                        endCode = 4;
                    else if (indexData.m_frameLen < 3932160)
                        endCode = 5;
                    else if (indexData.m_frameLen < 4718592)
                        endCode = 6;
                    else
                        endCode = 7;
                }
                else
                {
                    if (indexData.m_frameLen < 131072)
                        endCode = 1;
                    else if (indexData.m_frameLen < 262144)
                        endCode = 2;
                    else if (indexData.m_frameLen < 393216)
                        endCode = 3;
                    else if (indexData.m_frameLen < 589824)
                        endCode = 4;
                    else if (indexData.m_frameLen < 917504)
                        endCode = 5;
                    else if (indexData.m_frameLen < 1310720)
                        endCode = 6;
                    else
                        endCode = 7;
                }
            }
            writer.putBits(3, endCode);                          // I_end_position_offset[EP_fine_id]
            writer.putBits(11, (fst >> 9) % (1 << 11));          // PTS_EP_fine[EP_fine_id]
            writer.putBits(17, indexData.m_pktCnt % (1 << 17));  // SPN_EP_fine[EP_fine_id]
        }
    }
}

void CLPIParser::parseClipMark(uint8_t* buffer, const uint8_t* end)
{
    BitStreamReader reader{};
    reader.setBuffer(buffer, end);
    reader.skipBits(32);  // length
}

void CLPIParser::composeClipMark(BitStreamWriter& writer) { writer.putBits(32, 0); }

bool CLPIParser::parse(const char* fileName)
{
    File file;
    if (!file.open(fileName, File::ofRead))
        return false;
    int64_t fileSize;
    if (!file.size(&fileSize))
        return false;
    // A playlist or clip information file is small: one of more than 64 MiB is not one, and on a 32 bit build its
    // size would not fit the buffer
    if (fileSize < 0 || fileSize > 64 * 1024 * 1024)
        return false;
    const auto buffer = new uint8_t[static_cast<size_t>(fileSize)];
    if (!file.read(buffer, static_cast<uint32_t>(fileSize)))
    {
        delete[] buffer;
        return false;
    }
    try
    {
        parse(buffer, fileSize);
        delete[] buffer;
        return true;
    }
    catch (...)
    {
        delete[] buffer;
        return false;
    }
}

void CLPIParser::HDMV_LPCM_down_mix_coefficient(uint8_t* buffer, unsigned dataLength) {}

void CLPIParser::Extent_Start_Point(uint8_t* buffer, const unsigned dataLength)
{
    BitStreamReader reader{};
    reader.setBuffer(buffer, buffer + dataLength);
    reader.skipBits(32);  // length

    reader.skipBits(16);  // reserved_for_future_use
    const int number_of_extent_start_points = reader.getBits<int>(16);
    SPN_extent_start.resize(number_of_extent_start_points);
    for (int i = 0; i < number_of_extent_start_points; ++i)
    {
        SPN_extent_start[i] = reader.getBits(32);
    }
}

void CLPIParser::ProgramInfo_SS(uint8_t* buffer, const unsigned dataLength)
{
    parseProgramInfo(buffer, buffer + dataLength, m_programInfoMVC, m_streamInfoMVC);
}

void CLPIParser::CPI_SS(uint8_t* buffer, const unsigned dataLength) { parseCPI(buffer, buffer + dataLength); }

void CLPIParser::composeExtentStartPoint(BitStreamWriter& writer) const
{
    const auto lengthPos = UnalignedPtr<uint32_t>(writer.getBuffer() + writer.getBitsCount() / 8);
    writer.putBits(32, 0);  // length
    const int beforeCount = writer.getBitsCount() / 8;

    writer.putBits(16, 0);  // reserved
    writer.putBits(16, static_cast<unsigned>(interleaveInfo.size()));

    uint32_t sum = 0;
    for (const auto& i : interleaveInfo)
    {
        sum += i;
        writer.putBits(32, sum);
    }

    *lengthPos = my_htonl(writer.getBitsCount() / 8 - beforeCount);
}

void CLPIParser::composeExtentInfo(BitStreamWriter& writer)
{
    const auto lengthPos = UnalignedPtr<uint32_t>(writer.getBuffer() + writer.getBitsCount() / 8);
    const int beforeCount = writer.getBitsCount() / 8;
    m_extensionDataStart = writer.getBitsCount();

    writer.putBits(32, 0);  // length

    if (interleaveInfo.empty())
        return;

    writer.putBits(32, 0);  // skip data block start address
    writer.putBits(24, 0);  // reserved for world align

    const int entries = isDependStream ? 3 : 1;
    writer.putBits(8, entries);

    // write Extent_Start_Point header

    writer.putBits(32, 0x00020004);  // extent start point
    const auto extentStartPos = UnalignedPtr<uint32_t>(writer.getBuffer() + writer.getBitsCount() / 8);

    writer.putBits(32, 0);  // skip extent start address
    writer.putBits(32, 0);  // skip extent dataLen

    UnalignedPtr<uint32_t> CPI_SS_StartPos;
    UnalignedPtr<uint32_t> ProgramInfo_StartPos;
    if (isDependStream)
    {
        // write ProgramInfo_SS header
        writer.putBits(32, 0x00020005);  // extent start point
        ProgramInfo_StartPos = UnalignedPtr<uint32_t>(writer.getBuffer() + writer.getBitsCount() / 8);

        writer.putBits(32, 0);  // skip extent start address
        writer.putBits(32, 0);  // skip extent dataLen

        // write CPI_SS header
        writer.putBits(32, 0x00020006);  // extent start point
        CPI_SS_StartPos = UnalignedPtr<uint32_t>(writer.getBuffer() + writer.getBitsCount() / 8);

        writer.putBits(32, 0);  // skip extent start address
        writer.putBits(32, 0);  // skip extent dataLen
    }

    if ((writer.getBitsCount() - m_extensionDataStart) % 32)
        writer.putBits(16, 0);

    lengthPos[1] =
        my_htonl(writer.getBitsCount() / 8 - beforeCount);  // data_block_start_address, same as extentStart point start

    // write Extent_Start_Point body
    *extentStartPos = my_htonl(writer.getBitsCount() / 8 - beforeCount);
    int beforeExtentCount = writer.getBitsCount() / 8;
    composeExtentStartPoint(writer);
    extentStartPos[1] = my_htonl(writer.getBitsCount() / 8 - beforeExtentCount);

    if (isDependStream)
    {
        // write ProgramInfo_SS body
        *ProgramInfo_StartPos = my_htonl(writer.getBitsCount() / 8 - beforeCount);
        beforeExtentCount = writer.getBitsCount() / 8;
        composeProgramInfo(writer, true);
        ProgramInfo_StartPos[1] = my_htonl(writer.getBitsCount() / 8 - beforeExtentCount);

        // write CPI_SS body
        *CPI_SS_StartPos = my_htonl(writer.getBitsCount() / 8 - beforeCount);
        beforeExtentCount = writer.getBitsCount() / 8;
        composeCPI(writer, true);
        CPI_SS_StartPos[1] = my_htonl(writer.getBitsCount() / 8 - beforeExtentCount);
    }

    *lengthPos = my_htonl(writer.getBitsCount() / 8 - beforeCount - 4);
}

void CLPIParser::parseExtensionData(uint8_t* buffer, const uint8_t* end)
{
    // added for 3D compatibility
    BitStreamReader reader{};
    reader.setBuffer(buffer, end);
    if (reader.getBits(32) == 0)  // length
        return;

    reader.skipBits(32);  // data_block_start_address
    reader.skipBits(24);
    const int entries = reader.getBits<int>(8);
    for (int i = 0; i < entries; ++i)
    {
        const uint32_t dataID = reader.getBits(32);
        const uint32_t dataAddress = reader.getBits(32);
        const uint32_t dataLength = reader.getBits(32);

        if (dataAddress + dataLength > static_cast<uint32_t>(end - buffer))
        {
            LTRACE(LT_WARN, 2, "Invalid extended clip info entry skipped.");
            continue;
        }

        switch (dataID)
        {
        case 0x00010002:
            HDMV_LPCM_down_mix_coefficient(buffer + dataAddress, dataLength);
            break;
        case 0x00020004:
            Extent_Start_Point(buffer + dataAddress, dataLength);
            break;
        case 0x00020005:
            ProgramInfo_SS(buffer + dataAddress, dataLength);
            break;
        case 0x00020006:
            CPI_SS(buffer + dataAddress, dataLength);
            break;
        default:;
        }
    }
}

void CLPIParser::parse(uint8_t* buffer, const int64_t len)
{
    BitStreamReader reader{};
    try
    {
        reader.setBuffer(buffer, buffer + len);

        CLPIStreamInfo::readString(type_indicator, reader, 4);
        CLPIStreamInfo::readString(version_number, reader, 4);
        const uint32_t sequenceInfo_start_address = reader.getBits(32);
        const uint32_t programInfo_start_address = reader.getBits(32);
        const uint32_t CPI_start_address = reader.getBits(32);
        const uint32_t clipMark_start_address = reader.getBits(32);
        const uint32_t extensionData_start_address = reader.getBits(32);
        for (int i = 0; i < 3; i++) reader.skipBits(32);  // reserved_for_future_use
        parseClipInfo(reader);
        parseSequenceInfo(buffer + sequenceInfo_start_address, buffer + len);
        parseProgramInfo(buffer + programInfo_start_address, buffer + len, m_programInfo, m_streamInfo);
        parseCPI(buffer + CPI_start_address, buffer + len);
        parseClipMark(buffer + clipMark_start_address, buffer + len);
        if (extensionData_start_address)
            parseExtensionData(buffer + extensionData_start_address, buffer + len);
    }
    catch (BitStreamException&)
    {
        THROW(ERR_COMMON, "Can't parse clip info file: unexpected end of data")
    }
}

int CLPIParser::compose(uint8_t* buffer, const int bufferSize)
{
    BitStreamWriter writer{};
    writer.setBuffer(buffer, buffer + bufferSize);
    CLPIStreamInfo::writeString("HDMV", writer, 4);
    CLPIStreamInfo::writeString(version_number, writer, 4);
    const auto sequenceInfo_pos = UnalignedPtr<uint32_t>(buffer + writer.getBitsCount() / 8);
    writer.putBits(32, 0);  // sequenceInfo_start_address
    const auto programInfo_pos = UnalignedPtr<uint32_t>(buffer + writer.getBitsCount() / 8);
    writer.putBits(32, 0);  // programInfo_start_address
    const auto CPI_pos = UnalignedPtr<uint32_t>(buffer + writer.getBitsCount() / 8);
    writer.putBits(32, 0);  // CPI start address
    const auto clipMark_pos = UnalignedPtr<uint32_t>(buffer + writer.getBitsCount() / 8);
    writer.putBits(32, 0);  // clipMark
    const auto extentInfo_pos = UnalignedPtr<uint32_t>(buffer + writer.getBitsCount() / 8);
    writer.putBits(32, 0);                              // extent info
    for (int i = 0; i < 3; i++) writer.putBits(32, 0);  // reserved_for_future_use

    // The blocks follow each other without a gap, as the format has it: the format pads between blocks in 16-bit words
    // only, and padding to 32 bits left 2 bytes free after a block whose length was not a multiple of 4.
    composeClipInfo(writer);
    while (writer.getBitsCount() % 16 != 0) writer.putBits(8, 0);
    *sequenceInfo_pos = my_htonl(writer.getBitsCount() / 8);
    composeSequenceInfo(writer);
    while (writer.getBitsCount() % 16 != 0) writer.putBits(8, 0);

    *programInfo_pos = my_htonl(writer.getBitsCount() / 8);
    composeProgramInfo(writer, false);
    while (writer.getBitsCount() % 16 != 0) writer.putBits(8, 0);

    *CPI_pos = my_htonl(writer.getBitsCount() / 8);
    composeCPI(writer, false);
    while (writer.getBitsCount() % 16 != 0) writer.putBits(8, 0);

    *clipMark_pos = my_htonl(writer.getBitsCount() / 8);
    composeClipMark(writer);
    while (writer.getBitsCount() % 16 != 0) writer.putBits(8, 0);

    // A UHD clip (version 0300) has ExtensionData only where there is some, as on the UHD discs seen, none of which has
    // the empty one written for every clip. A Blu-ray clip keeps the empty one, as most Blu-ray discs seen have it.
    if (!interleaveInfo.empty() || memcmp(version_number, "0300", 4) != 0)
    {
        *extentInfo_pos = my_htonl(writer.getBitsCount() / 8);
        composeExtentInfo(writer);
        while (writer.getBitsCount() % 16 != 0) writer.putBits(8, 0);
    }

    writer.flushBits();
    return writer.getBitsCount() / 8;
}

// -------------------------- MPLSParser ----------------------------

MPLSParser::MPLSParser()
    : PlayList_playback_type(0),
      playback_count(0),
      number_of_SubPaths(0),
      is_multi_angle(false),
      ref_to_STC_id(0),
      PlayItem_random_access_flag(false),
      number_of_angles(0),
      is_different_audios(false),
      is_seamless_angle_change(false),
      m_chapterLen(0),
      IN_time(0),
      OUT_time(0),
      m_m2tsOffset(0),
      isDependStreamExist(false),
      mvc_base_view_r(false),
      subPath_type(0)
{
    number_of_primary_video_stream_entries = 0;
    number_of_primary_audio_stream_entries = 0;
    number_of_PG_textST_stream_entries = 0;
    number_of_IG_stream_entries = 0;
    number_of_secondary_audio_stream_entries = 0;
    number_of_secondary_video_stream_entries = 0;
    number_of_PiP_PG_textST_stream_entries_plus = 0;
    number_of_DolbyVision_video_stream_entries = 0;
}

bool MPLSParser::parse(const char* fileName)
{
    File file;
    if (!file.open(fileName, File::ofRead))
        return false;
    int64_t fileSize;
    if (!file.size(&fileSize))
        return false;
    // A playlist or clip information file is small: one of more than 64 MiB is not one, and on a 32 bit build its
    // size would not fit the buffer
    if (fileSize < 0 || fileSize > 64 * 1024 * 1024)
        return false;
    const auto buffer = new uint8_t[static_cast<size_t>(fileSize)];
    if (!file.read(buffer, static_cast<uint32_t>(fileSize)))
    {
        delete[] buffer;
        return false;
    }
    try
    {
        parse(buffer, static_cast<int>(fileSize));
        delete[] buffer;
        return true;
    }
    catch (...)
    {
        delete[] buffer;
        return false;
    }
}

void MPLSParser::parse(uint8_t* buffer, const int len)
{
    BitStreamReader reader{};
    try
    {
        reader.setBuffer(buffer, buffer + len);
        char type_indicator[5];
        char version_number[5];
        CLPIStreamInfo::readString(type_indicator, reader, 4);
        CLPIStreamInfo::readString(version_number, reader, 4);
        const int playList_start_address = reader.getBits<int>(32);
        const int playListMark_start_address = reader.getBits<int>(32);
        const int extensionData_start_address = reader.getBits<int>(32);
        for (int i = 0; i < 5; i++) reader.skipBits(32);  // reserved_for_future_use
        AppInfoPlayList(reader);
        parsePlayList(buffer + playList_start_address, len - playList_start_address);
        parsePlayListMark(buffer + playListMark_start_address, len - playListMark_start_address);
        // Before the extension data, which patches m_streamInfo in place for 3D playlists and so
        // needs the final one, and after the marks, which the drop below has to renumber.
        chooseRepresentativeStnTable();

        if (extensionData_start_address)
        {
            parseExtensionData(buffer + extensionData_start_address, buffer + len);
        }
        dropContradictingPlayItems();
    }
    catch (BitStreamException&)
    {
        THROW(ERR_COMMON, "Can't parse media playlist file: unexpected end of data")
    }
}

void MPLSParser::SubPath_extension(BitStreamWriter& writer)
{
    const auto lengthPos = UnalignedPtr<uint32_t>(writer.getBuffer() + writer.getBitsCount() / 8);
    writer.putBits(32, 0);  // length
    const int beforeCount = writer.getBitsCount() / 8;

    writer.putBits(8, 0);   // reserved
    writer.putBits(8, 8);   // SubPath_type = 8
    writer.putBits(15, 0);  // reserved
    writer.putBit(0);       // is_repeat_SubPath
    writer.putBits(8, 0);   // reserved

    const std::vector<PMTIndex> pmtIndexList = getMVCDependStream().m_index;
    writer.putBits(8, static_cast<unsigned>(pmtIndexList.size()));  // number_of_SubPlayItems
    for (size_t i = 0; i < pmtIndexList.size(); ++i) composeSubPlayItem(writer, i, 0, pmtIndexList);

    *lengthPos = my_htonl(writer.getBitsCount() / 8 - beforeCount);
}

int MPLSParser::composeSubPathEntryExtension(uint8_t* buffer, const int bufferSize)
{
    BitStreamWriter writer{};
    writer.setBuffer(buffer, buffer + bufferSize);
    try
    {
        const auto lengthPos = UnalignedPtr<uint32_t>(writer.getBuffer() + writer.getBitsCount() / 8);
        writer.putBits(32, 0);  // length
        const int beforeCount = writer.getBitsCount() / 8;

        writer.putBits(16, 1);  // one subpath
        SubPath_extension(writer);

        if (writer.getBitsCount() % 32 != 0)
            writer.putBits(16, 0);

        writer.flushBits();
        *lengthPos = my_htonl(writer.getBitsCount() / 8 - beforeCount);
        return writer.getBitsCount() / 8;
    }
    catch (...)
    {
        return 0;
    }
}

int MPLSParser::composeSTN_tableSS(uint8_t* buffer, const int bufferSize)
{
    BitStreamWriter writer{};
    writer.setBuffer(buffer, buffer + bufferSize);
    try
    {
        const MPLSStreamInfo streamInfoMVC = getMVCDependStream();
        for (size_t PlayItem_id = 0; PlayItem_id < streamInfoMVC.m_index.size(); PlayItem_id++)
        {
            composeSTN_table(writer, PlayItem_id, true);
            // connection_condition = 6;
        }
        writer.flushBits();
        return writer.getBitsCount() / 8;
    }
    catch (...)
    {
        return 0;
    }
};

// The dynamic range of the playlist's video, with the mastering display and light levels of its stream. A UHD
// disc writes no entry for a playlist without HEVC video, one entry of type 0 for SDR HEVC (with the stream's own
// values, zero where it has none) and one of type 1 for HDR10, HDR10+ and Dolby Vision. Every playlist had an entry
// of type 1, also for SDR video and for H.264.
int MPLSParser::composeUHD_metadata(uint8_t* buffer, const int bufferSize, const MPLSStreamInfo& mainStream)
{
    BitStreamWriter writer{};
    writer.setBuffer(buffer, buffer + bufferSize);
    try
    {
        if (mainStream.stream_coding_type != StreamType::VIDEO_H265)
        {
            writer.putBits(32, 4);
            writer.putBits(32, 0);  // no entry
            writer.flushBits();
            return writer.getBitsCount() / 8;
        }
        const bool hdr = (mainStream.HDR & 18) || mainStream.HDR == 4;  // as in the stream attributes
        writer.putBits(32, 0x20);
        writer.putBits(32, 1 << 24);
        writer.putBits(32, hdr ? 1 << 28 : 0);
        for (const unsigned i : HDR10_metadata) writer.putBits(32, i);
        writer.flushBits();
        return writer.getBitsCount() / 8;
    }
    catch (...)
    {
        return 0;
    }
}

int MPLSParser::compose(uint8_t* buffer, int bufferSize, const DiskType dt)
{
    for (const MPLSStreamInfo& si : m_streamInfo)
    {
        const StreamType stream_coding_type = si.stream_coding_type;
        if (isVideoStreamType(stream_coding_type))
        {
            if (si.isSecondary)
            {
                number_of_SubPaths++;
                subPath_type = 7;  // PIP not fully implemented yet
            }
            else if (si.isDvEnhancement)
            {
                number_of_SubPaths++;
                subPath_type = 10;
            }
        }
    }

    BitStreamWriter writer{};
    writer.setBuffer(buffer, buffer + bufferSize);

    const std::string type_indicator = "MPLS";
    std::string version_number;
    if (dt == DiskType::BLURAY)
        version_number = (isV3() ? "0300" : "0200");
    else
        version_number = "0100";
    CLPIStreamInfo::writeString(type_indicator.c_str(), writer, 4);
    CLPIStreamInfo::writeString(version_number.c_str(), writer, 4);
    const auto playList_bit_pos = UnalignedPtr<uint32_t>(buffer + writer.getBitsCount() / 8);
    writer.putBits(32, 0);
    const auto playListMark_bit_pos = UnalignedPtr<uint32_t>(buffer + writer.getBitsCount() / 8);
    writer.putBits(32, 0);
    const auto extDataStartAddr = UnalignedPtr<uint32_t>(buffer + writer.getBitsCount() / 8);
    writer.putBits(32, 0);                              // extension data start address
    for (int i = 0; i < 5; i++) writer.putBits(32, 0);  // reserved_for_future_use
    composeAppInfoPlayList(writer);

    if (writer.getBitsCount() % 16 != 0)
        writer.putBits(8, 0);
    *playList_bit_pos = my_htonl(writer.getBitsCount() / 8);
    composePlayList(writer);

    if (writer.getBitsCount() % 16 != 0)
        writer.putBits(8, 0);
    *playListMark_bit_pos = my_htonl(writer.getBitsCount() / 8);
    composePlayListMark(writer);

    if (writer.getBitsCount() % 16 != 0)
        writer.putBits(8, 0);

    if (number_of_SubPaths > 0 || isDependStreamExist || isV3())
    {
        *extDataStartAddr = my_htonl(writer.getBitsCount() / 8);
        uint8_t buff[1024 * 4];
        const MPLSStreamInfo& mainStreamInfo = getMainStream();
        vector<ExtDataBlockInfo> blockVector;

        if (number_of_SubPaths > 0 && subPath_type == 7)
        {
            bufferSize = composePip_metadata(buff, sizeof(buff), mainStreamInfo.m_index);
            const ExtDataBlockInfo extDataBlock(buff, bufferSize, 1, 1);
            blockVector.push_back(extDataBlock);
        }

        if (isDependStreamExist)
        {
            bufferSize = composeSTN_tableSS(buff, sizeof(buff));
            const ExtDataBlockInfo extDataBlock(buff, bufferSize, 2, 1);
            blockVector.push_back(extDataBlock);

            bufferSize = composeSubPathEntryExtension(buff, sizeof(buff));
            const ExtDataBlockInfo extDataBlock2(buff, bufferSize, 2, 2);
            blockVector.push_back(extDataBlock2);
        }

        if (isV3())
        {
            bufferSize = composeUHD_metadata(buff, sizeof(buff), mainStreamInfo);
            const ExtDataBlockInfo extDataBlock(buff, bufferSize, 3, 5);
            blockVector.push_back(extDataBlock);
        }

        composeExtensionData(writer, blockVector);
        if (writer.getBitsCount() % 16 != 0)
            writer.putBits(8, 0);
    }

    writer.flushBits();
    return writer.getBitsCount() / 8;
}

void MPLSParser::AppInfoPlayList(BitStreamReader& reader)
{
    reader.skipBits(32);  // length
    reader.skipBits(8);   // reserved_for_future_use
    PlayList_playback_type = reader.getBits<int>(8);
    if (PlayList_playback_type == 2 || PlayList_playback_type == 3)
    {  // 1 == Sequential playback of PlayItems
        playback_count = reader.getBits<int>(16);
    }
    else
    {
        reader.skipBits(16);  // reserved_for_future_use
    }
    UO_mask_table(reader);
    reader.skipBits(3);  // PlayList_random_access_flag, audio_mix_app_flag, lossless_may_bypass_mixer_flag
    mvc_base_view_r = reader.getBit();
    reader.skipBits(12);  // reserved_for_future_use
}

void MPLSParser::composeAppInfoPlayList(BitStreamWriter& writer) const
{
    const auto lengthPos = UnalignedPtr<uint32_t>(writer.getBuffer() + writer.getBitsCount() / 8);
    writer.putBits(32, 0);  // length
    const int beforeCount = writer.getBitsCount() / 8;

    writer.putBits(8, 0);  // reserved_for_future_use
    writer.putBits(8, PlayList_playback_type);
    if (PlayList_playback_type == 2 || PlayList_playback_type == 3)
    {  // 1 == Sequential playback of PlayItems
        writer.putBits(16, playback_count);
    }
    else
    {
        writer.putBits(16, 0);  // reserved_for_future_use
    }
    writer.putBits(28, 0);               // UO_mask_table;
    writer.putBits(4, isV3() ? 15 : 0);  // UO_mask_table;
    writer.putBit(0);                    // reserved
    writer.putBit(isV3() ? 1 : 0);       // UO_mask_table: SecondaryPGStreamNumberChange
    writer.putBits(30, 0);               // UO_mask_table cont;
    writer.putBit(0);                    // PlayList_random_access_flag
    writer.putBit(1);  // audio_mix_app_flag. 0 == no secondary audio, 1- allow secondary audio if exist
    writer.putBit(0);  // lossless_may_bypass_mixer_flag
    writer.putBit(mvc_base_view_r);
    writer.putBits(12, 0);  // reserved_for_future_use
    *lengthPos = my_htonl(writer.getBitsCount() / 8 - beforeCount);
}

void MPLSParser::UO_mask_table(BitStreamReader& reader)
{
    reader.skipBits(32);  // UO_mask_table;
    reader.skipBits(32);  // UO_mask_table cont;
}

void MPLSParser::parsePlayList(uint8_t* buffer, const int len)
{
    BitStreamReader reader{};
    reader.setBuffer(buffer, buffer + len);
    reader.skipBits(32);  // length
    reader.skipBits(16);  // reserved_for_future_use
    const int number_of_PlayItems = reader.getBits<int>(16);
    number_of_SubPaths = reader.getBits<int>(16);
    for (int PlayItem_id = 0; PlayItem_id < number_of_PlayItems; PlayItem_id++)
    {
        parsePlayItem(reader, PlayItem_id);
    }
    // The sub paths follow the play items, found by the play items' own lengths: a play item can hold more than its
    // reading takes
    int subPathsStart = 10;
    for (int i = 0; i < number_of_PlayItems && subPathsStart + 2 <= len; i++)
        subPathsStart += 2 + (buffer[subPathsStart] << 8 | buffer[subPathsStart + 1]);
    if (subPathsStart < len)
        parseSubPaths(buffer + subPathsStart, len - subPathsStart, number_of_SubPaths);
}

// The sub paths were not read at all. A stream entry of the STN table that names one (entry type 2, 3 or 4) points
// at a stream of ANOTHER clip, as a disc's text subtitles always are and other subtitles, audio or video can be, and
// its PID is that clip's, not the play item's. Read only as far as the listing and the mux need: the kind of each
// sub path and the clips it plays. A damaged sub path ends the reading, and the playlist is still read as before.
void MPLSParser::parseSubPaths(const uint8_t* data, const int len, const int count)
{
    m_subPaths.clear();
    int pos = 0;
    for (int i = 0; i < count; ++i)
    {
        if (pos + 10 > len)
            return;
        const int64_t length =
            static_cast<int64_t>(data[pos]) << 24 | data[pos + 1] << 16 | data[pos + 2] << 8 | data[pos + 3];
        const int64_t end = pos + 4 + length;
        if (end > len)
            return;
        MPLSSubPath subPath;
        subPath.type = data[pos + 5];
        const int subPlayItems = data[pos + 9];
        int q = pos + 10;
        for (int k = 0; k < subPlayItems && q + 7 <= end; ++k)
        {
            const int itemLength = data[q] << 8 | data[q + 1];
            subPath.clips.emplace_back(reinterpret_cast<const char*>(data + q + 2), 5);
            if (q + 30 <= end)
            {
                const auto be32 = [data](const int at)
                {
                    return static_cast<uint32_t>(data[at]) << 24 | static_cast<uint32_t>(data[at + 1]) << 16 |
                           static_cast<uint32_t>(data[at + 2]) << 8 | data[at + 3];
                };
                MPLSSubPlayItem item;
                item.IN_time = be32(q + 16);
                item.OUT_time = be32(q + 20);
                item.syncPlayItem = data[q + 24] << 8 | data[q + 25];
                item.syncStart = be32(q + 26);
                subPath.items.push_back(item);
            }
            q += 2 + itemLength;
        }
        m_subPaths.push_back(subPath);
        pos = static_cast<int>(end);
    }
}

// A playlist joins clips, and nothing obliges those clips to carry the same streams. A short
// intro or a distributor's logo is often a separate clip with its own audio, and the feature
// follows it. Only the FIRST play item's STN table used to be kept, so such a playlist was
// described by its intro: the feature's own tracks were never offered, and muxing what was
// offered read the feature's payload through the wrong reader (upstream issue 724).
//
// Pick the stream set that the greatest amount of the playlist actually uses. Clips are grouped
// by the streams they declare, the run time of each group is added up, and the largest wins. On
// a playlist whose clips all agree, which is nearly every playlist, there is one group and this
// changes nothing.
void MPLSParser::chooseRepresentativeStnTable()
{
    if (m_stnTables.empty())
        return;
    m_stnTables.resize(m_playItems.size());

    // A video stream counts with its picture format and frame rate: a short intro at 24 or 25 fps before an episode at
    // 23.976 shares the PIDs and codecs of the episode, and with them alone the intro could describe the playlist and
    // give its rate to the whole output, which then played the episode too fast and away from its sound
    // (justdan96/tsMuxer#900).
    auto signatureOf = [](const MPLSStnTable& table)
    {
        std::vector<std::pair<int, std::string>> pairs;
        pairs.reserve(table.streams.size());
        for (const MPLSStreamInfo& stream : table.streams)
        {
            std::string kind = int32ToStr(static_cast<int>(stream.stream_coding_type));
            if (isVideoStreamType(stream.stream_coding_type))
                kind += '/' + int32ToStr(stream.video_format) + '/' + int32ToStr(stream.frame_rate_index);
            pairs.emplace_back(stream.streamPID, kind);
        }
        std::sort(pairs.begin(), pairs.end());
        std::string rez;
        for (const auto& [pid, kind] : pairs) rez += int32ToStr(pid) + ':' + kind + ',';
        return rez;
    };

    std::map<std::string, int64_t> durationBySignature;
    std::vector<std::string> signatures;
    signatures.reserve(m_stnTables.size());
    for (size_t i = 0; i < m_stnTables.size(); ++i)
    {
        signatures.push_back(signatureOf(m_stnTables[i]));
        // OUT_time is not guaranteed to be past IN_time in a damaged playlist, and a negative
        // span must not be allowed to vote.
        const int64_t span = static_cast<int64_t>(m_playItems[i].OUT_time) - m_playItems[i].IN_time;
        durationBySignature[signatures[i]] += span > 0 ? span : 0;
    }

    // Which SET of streams describes the playlist is decided by the total run time of each
    // signature, so a short odd clip cannot outvote the feature.
    std::string bestSignature;
    int64_t bestDuration = -1;
    for (size_t i = 0; i < signatures.size(); ++i)
    {
        const int64_t duration = durationBySignature[signatures[i]];
        // Strictly greater, so an all-equal playlist keeps the first play item and behaves
        // exactly as it did before.
        if (duration > bestDuration)
        {
            bestDuration = duration;
            bestSignature = signatures[i];
        }
    }

    // ** WHICH play item of that group provides the DETAILS is a separate question. ** Two
    // items can share a signature, having the same PIDs and the same codecs, and still
    // disagree about the languages. A disc that opens with a short intro and then plays the
    // feature has exactly that, and taking the first item made a 17 second intro name the
    // languages of a two hour film. The longest run of its own wins.
    size_t best = 0;
    int64_t bestOwnDuration = -1;
    for (size_t i = 0; i < signatures.size(); ++i)
    {
        if (signatures[i] != bestSignature)
            continue;
        const int64_t span = static_cast<int64_t>(m_playItems[i].OUT_time) - m_playItems[i].IN_time;
        const int64_t own = span > 0 ? span : 0;
        if (own > bestOwnDuration)
        {
            bestOwnDuration = own;
            best = i;
        }
    }

    // swap rather than assign: MPLSStreamInfo owns its stereo PG halves and has a deep copy
    // constructor but no copy assignment, so assigning a vector of them would share the pointers.
    m_chosenStnItem = best;
    m_streamInfo.swap(m_stnTables[best].streams);
    const MPLSStnTable& chosen = m_stnTables[best];
    number_of_primary_video_stream_entries = chosen.counts[0];
    number_of_primary_audio_stream_entries = chosen.counts[1];
    number_of_PG_textST_stream_entries = chosen.counts[2];
    number_of_IG_stream_entries = chosen.counts[3];
    number_of_secondary_audio_stream_entries = chosen.counts[4];
    number_of_secondary_video_stream_entries = chosen.counts[5];
    number_of_PiP_PG_textST_stream_entries_plus = chosen.counts[6];
    number_of_DolbyVision_video_stream_entries = chosen.counts[7];

    // A clip that declares a DIFFERENT codec on a PID the chosen set also uses cannot be muxed
    // into the same track: its payload would reach the wrong reader. A clip that merely carries
    // fewer streams is left alone, because that costs nothing but a gap. Nor can a clip whose
    // video on such a PID has another picture format or frame rate: an output has one rate, and
    // the clip's pictures would be timed by the rate of the rest. A value of 0 (not given) is
    // taken to agree with any.
    std::map<int, const MPLSStreamInfo*> chosenStream;
    for (const MPLSStreamInfo& stream : m_streamInfo) chosenStream[stream.streamPID] = &stream;
    const auto videoText = [](const MPLSStreamInfo& s)
    {
        static const char* const formats[] = {"", "480i", "576i", "480p", "1080i", "720p", "1080p", "576p", "2160p"};
        static const char* const rates[] = {"", "23.976", "24", "25", "29.97", "", "50", "59.94"};
        std::string text = s.video_format < 9 ? formats[s.video_format] : "";
        const std::string rate = s.frame_rate_index < 8 ? rates[s.frame_rate_index] : "";
        if (!rate.empty())
            text += (text.empty() ? "" : " at ") + rate + " fps";
        return text.empty() ? std::string("another format") : text;
    };

    for (size_t i = 0; i < m_stnTables.size(); ++i)
    {
        if (i == best)
            continue;
        for (const MPLSStreamInfo& stream : m_stnTables[i].streams)
        {
            const auto itr = chosenStream.find(stream.streamPID);
            if (itr == chosenStream.end())
                continue;
            const MPLSStreamInfo& chosenOne = *itr->second;
            if (chosenOne.stream_coding_type != stream.stream_coding_type)
            {
                m_playItemsToSkip.push_back(i);
                m_skipReasons.emplace_back();
                break;
            }
            const auto differ = [](const uint8_t a, const uint8_t b) { return a != 0 && b != 0 && a != b; };
            if (isVideoStreamType(stream.stream_coding_type) &&
                (differ(chosenOne.video_format, stream.video_format) ||
                 differ(chosenOne.frame_rate_index, stream.frame_rate_index)))
            {
                m_playItemsToSkip.push_back(i);
                m_skipReasons.push_back("its video is " + videoText(stream) + " and that of the rest of the playlist " +
                                        videoText(chosenOne) +
                                        ", and a single output cannot change that part way through");
                break;
            }
        }
    }
}

// Applied after the extension data, because that is where a 3D playlist's MVC clip names arrive
// and they are indexed the same way. Dropping a play item here keeps every consumer consistent
// without any of them knowing: the file list, the chapter marks, the clip timing and the MVC
// halves all come from m_playItems.
void MPLSParser::dropContradictingPlayItems()
{
    if (m_playItemsToSkip.empty())
        return;

    std::vector<std::string> reasons;
    for (size_t k = m_playItemsToSkip.size(); k-- > 0;)
    {
        const size_t index = m_playItemsToSkip[k];
        if (index >= m_playItems.size())
            continue;
        m_skippedClips.push_back(m_playItems[index].fileName);
        reasons.push_back(k < m_skipReasons.size() ? m_skipReasons[k] : std::string());
        m_playItems.erase(m_playItems.begin() + index);
        // The chosen play item is never dropped; it keeps naming the same play item
        if (index < m_chosenStnItem)
            --m_chosenStnItem;
        // m_mvcFiles holds each dependent clip at the play item its sub play item names
        // (sync_PlayItem_id), so the dropped play item's entry goes with it whenever there is one.
        // This used to be done only when the list was exactly as long as the play items, a test
        // from when it was a flat list. A sub play item naming a play item that does not exist, or
        // a last play item that none names, made the lengths differ; the erase was then skipped,
        // and every play item after the dropped one got the dependent clip of the one before it
        // (T2-F1).
        if (index < m_mvcFiles.size())
            m_mvcFiles.erase(m_mvcFiles.begin() + index);

        // A mark belongs to a play item by index, so the marks of the dropped clip go with it and
        // everything after it moves down one. Left alone, the chapters of a playlist whose intro
        // was dropped would all be one clip late.
        for (size_t m = m_marks.size(); m-- > 0;)
        {
            if (static_cast<size_t>(m_marks[m].m_playItemID) == index)
                m_marks.erase(m_marks.begin() + m);
            else if (static_cast<size_t>(m_marks[m].m_playItemID) > index)
                m_marks[m].m_playItemID--;
        }
    }
    std::reverse(m_skippedClips.begin(), m_skippedClips.end());
    std::reverse(reasons.begin(), reasons.end());

    for (size_t k = 0; k < m_skippedClips.size() && !m_quietSkips; ++k)
    {
        if (!reasons[k].empty())
            LTRACE(LT_WARN, 2,
                   "Warning: clip " << m_skippedClips[k] << " is left out of this playlist: " << reasons[k] << ".");
        else
            LTRACE(LT_WARN, 2,
                   "Warning: clip " << m_skippedClips[k]
                                    << " is left out of this playlist. Its streams are not the ones the rest of the "
                                       "playlist uses, and a single output cannot change codec part way through.");
    }
}

MPLSStreamInfo& MPLSParser::getMainStream()
{
    for (auto& i : m_streamInfo)
    {
        const StreamType coding_type = i.stream_coding_type;
        if (isVideoStreamType(coding_type))
            return i;
    }
    for (auto& i : m_streamInfo)
    {
        const StreamType coding_type = i.stream_coding_type;
        if (isAudioStreamType(coding_type))
            return i;
    }
    THROW(ERR_COMMON, "Can't find stream index. One audio or video stream is needed.")
}

// The place in m_streamInfo of entry `value` of the PG and text subtitle list: STN_table() stores the primary video
// entries, then the primary audio entries, then this list in its own order. -1 when the list has no such entry.
int MPLSParser::pgTextIndexToFullIndex(const int value) const
{
    if (value < 0 || value >= number_of_PG_textST_stream_entries)
        return -1;
    const size_t idx = static_cast<size_t>(number_of_primary_video_stream_entries) +
                       number_of_primary_audio_stream_entries + static_cast<size_t>(value);
    return idx < m_streamInfo.size() ? static_cast<int>(idx) : -1;
}

// Whether the stream of an STN entry is in the play item's own clip. An entry of type 1 names a stream of that clip,
// and so does one of type 3 or 4 whose sub path plays in the same clip (SubPath_type 7, picture-in-picture in the
// main clip, which is how discs usually declare their secondary video and audio; tsMuxeR writes its secondary video so,
// and its secondary audio as type 1). An entry of type 2 always names a stream of the clip a sub path plays (it
// carries the sub clip's entry number), and so does one whose sub path is of type 4 (text subtitles), 5 or 6
// (out-of-mux streams and picture-in-picture).
bool MPLSParser::isInMainClip(const MPLSStreamInfo& entry) const
{
    if (entry.type == 2)
        return false;
    if ((entry.type == 3 || entry.type == 4) && entry.refSubPathId >= 0 &&
        static_cast<size_t>(entry.refSubPathId) < m_subPaths.size())
    {
        const int subPathType = m_subPaths[entry.refSubPathId].type;
        return subPathType != 4 && subPathType != 5 && subPathType != 6;
    }
    return true;
}

// Only an entry whose stream is in the play item's clip is that clip's stream of this PID. A sub path entry
// carries the PID of its own clip, and its subtitles start at 0x1200 there as they do in the main clip: matched by
// PID alone, a playlist whose sub path subtitles are Japanese listed and muxed the main clip's first English PG
// stream as Japanese.
MPLSStreamInfo MPLSParser::getStreamByPID(const int pid) const
{
    for (auto& i : m_streamInfo)
    {
        if (i.streamPID == pid && isInMainClip(i))
            return i;
    }
    return {};
}

bool MPLSParser::subPathShiftMs(const int subPath, int& shiftMs) const
{
    if (subPath < 0 || static_cast<size_t>(subPath) >= m_subPaths.size() || m_subPaths[subPath].items.empty())
        return false;
    const MPLSSubPlayItem& item = m_subPaths[subPath].items[0];
    if (item.syncPlayItem < 0 || static_cast<size_t>(item.syncPlayItem) >= m_playItems.size())
        return false;
    // the play items before the one it is tied to, then its place in that play item, less where its own clock stood
    int64_t at = 0;
    for (int i = 0; i < item.syncPlayItem; ++i)
        at += static_cast<int64_t>(m_playItems[i].OUT_time) - m_playItems[i].IN_time;
    at += static_cast<int64_t>(item.syncStart) - m_playItems[item.syncPlayItem].IN_time;
    at -= item.IN_time;
    shiftMs = static_cast<int>(at >= 0 ? (at + 22) / 45 : (at - 22) / 45);
    return true;
}

std::vector<MPLSStreamInfo> MPLSParser::getOutOfMuxStreams() const
{
    std::vector<MPLSStreamInfo> rez;
    for (auto& i : m_streamInfo)
        if (!isInMainClip(i))
            rez.push_back(i);
    return rez;
}

MPLSStreamInfo MPLSParser::getStreamReferencing(const int pid, std::string& role) const
{
    for (auto& i : m_streamInfo)
    {
        if (i.stream_coding_type != StreamType::SUB_PGS || !isInMainClip(i))
            continue;
        if (i.isSSPG && i.leftEye && i.leftEye->type == 1 && i.leftEye->streamPID == pid)
            role = "left eye";
        else if (i.isSSPG && i.rightEye && i.rightEye->type == 1 && i.rightEye->streamPID == pid)
            role = "right eye";
        else if (i.topAS.pid == pid && i.topAS.type == 1)
            role = "top aligned";
        else if (i.bottomAS.pid == pid && i.bottomAS.type == 1)
            role = "bottom aligned";
        else
            continue;
        return i;
    }
    return {};
}

std::string MPLSParser::outOfMuxTrackKind(const MPLSStreamInfo& entry)
{
    if (isAudioStreamType(entry.stream_coding_type))
        return "Audio";
    if (isVideoStreamType(entry.stream_coding_type))
        return entry.isSecondary ? "Picture-in-picture video" : "Video";
    return {};
}

// A mux of a playlist muxes the first angle of each play item, as moving pictures, from the clips its play items
// play. What a playlist has beyond that was left out without a word: the other angles of a multi-angle play item,
// the hold of a still picture (a slideshow, held until the viewer goes on or for a set time, became a flash of single
// pictures), and the audio and video a sub path plays from a clip of its own, as the music of a slideshow. Muxing
// them is not built yet; until then the log says what is left out.
void MPLSParser::selectAngle(const int angle)
{
    int most = 1;
    for (const MPLSPlayItem& item : m_playItems) most = (std::max)(most, item.angles);
    if (angle < 1 || angle > most)
        THROW(ERR_COMMON, "angle=" << angle << " is not an angle of this playlist, which has "
                                   << (most == 1 ? std::string("one angle") : std::to_string(most) + " angles"))
    m_angle = angle;
    for (MPLSPlayItem& item : m_playItems)
        if (angle > 1 && item.angles >= angle && static_cast<size_t>(angle - 2) < item.angleFiles.size())
            item.fileName = item.angleFiles[static_cast<size_t>(angle - 2)];
}

void MPLSParser::reportLeftOut(const std::string& playlistName, const bool listing) const
{
    const std::string name = extractFileName2(playlistName);
    int angleItems = 0;
    int anglesLeft = 0;
    int stills = 0;
    for (const MPLSPlayItem& item : m_playItems)
    {
        if (item.angles > 1)
        {
            ++angleItems;
            anglesLeft += item.angles - 1;
        }
        if (item.still_mode != 0)
            ++stills;
    }
    if (angleItems > 0)
        LTRACE(LT_WARN, 2,
               "Warning: playlist " << name << " has more than one angle in " << angleItems
                                    << (angleItems == 1 ? " play item" : " play items") << ". Only "
                                    << (m_angle == 1 ? std::string("the first angle") : "angle " + std::to_string(m_angle))
                                    << " is " << (listing ? "listed and muxed" : "muxed") << ": " << anglesLeft
                                    << (anglesLeft == 1 ? " angle is" : " angles are")
                                    << " left out (angle= on a meta line chooses another). The rest of the playlist "
                                       "is muxed.");
    if (stills > 0)
        LTRACE(LT_WARN, 2,
               "Warning: playlist " << name << " has " << stills << (stills == 1 ? " still picture" : " still pictures")
                                    << ", play items held on the screen for a time or until the viewer goes on. "
                                       "The hold is left out: each is muxed as moving video and shown only for the "
                                       "time of its pictures. The rest of the playlist is muxed.");
    if (listing)
        return;
    std::string tracks;
    int trackCount = 0;
    for (const MPLSStreamInfo& entry : getOutOfMuxStreams())
    {
        const std::string kind = outOfMuxTrackKind(entry);
        if (kind.empty())
            continue;
        std::ostringstream track;
        track << strToLowerCase(kind) << " PID 0x" << std::hex << entry.streamPID << std::dec;
        if (entry.refSubPathId >= 0 && static_cast<size_t>(entry.refSubPathId) < m_subPaths.size() &&
            !m_subPaths[entry.refSubPathId].clips.empty())
            track << " (clip " << m_subPaths[entry.refSubPathId].clips[0] << ")";
        tracks += (tracks.empty() ? "" : ", ") + track.str();
        ++trackCount;
    }
    if (trackCount > 0)
        LTRACE(LT_WARN, 2,
               "Warning: playlist " << name << " plays " << tracks
                                    << (trackCount == 1 ? " from a separate clip" : " from separate clips")
                                    << ", which a mux of the playlist does not read, so "
                                    << (trackCount == 1 ? "it is" : "they are")
                                    << " left out. The rest of the playlist is muxed.");
    // Subtitles from a clip of their own, as a disc's text subtitles always are: the listing says "Not supported" for
    // each, and a mux left them out without a word. One warning for each track, with what the listing gives for it.
    // A pop-up menu (interactive graphics) is no track a mux could have had, as in the listing.
    for (const MPLSStreamInfo& entry : getOutOfMuxStreams())
    {
        const auto codingType = static_cast<int>(entry.stream_coding_type);
        if (codingType != 0x90 && codingType != 0x92)
            continue;
        std::string clip;
        if (entry.refSubPathId >= 0 && static_cast<size_t>(entry.refSubPathId) < m_subPaths.size() &&
            !m_subPaths[entry.refSubPathId].clips.empty())
            clip = "clip " + m_subPaths[entry.refSubPathId].clips[0] + ", ";
        const std::string lang = applyLangCodeStyle(entry.language_code);
        std::ostringstream where;
        where << "stream type 0x" << std::hex << codingType << ", PID 0x" << entry.streamPID << std::dec
              << (isLanguageCode(lang) ? ", language " + lang : std::string()) << ", " << clip << "sub path "
              << entry.refSubPathId;
        LTRACE(LT_WARN, 2,
               "Warning: playlist " << name << " plays "
                                    << (codingType == 0x92 ? "text subtitles (Text-ST)"
                                                           : "presentation graphics subtitles")
                                    << " (" << where.str()
                                    << (codingType == 0x92
                                            ? ") from a separate clip. They are in the output when a line of the "
                                              "meta file names that clip, as the listing of the playlist gives it."
                                            : ") from a separate clip, which a mux of the playlist does not read, so "
                                              "the track is left out. The rest of the playlist is muxed."));
    }
}

std::vector<MPLSStreamInfo> MPLSParser::getPgStreams() const
{
    std::vector<MPLSStreamInfo> pgStreams;
    for (auto& i : m_streamInfo)
    {
        const StreamType coding_type = i.stream_coding_type;
        if (coding_type == StreamType::SUB_PGS)
            pgStreams.push_back(i);
    }
    return pgStreams;
}

MPLSStreamInfo& MPLSParser::getMVCDependStream()
{
    for (auto& i : m_streamInfoMVC)
    {
        const StreamType coding_type = i.stream_coding_type;
        if (coding_type == StreamType::VIDEO_MVC)
            return i;
    }
    THROW(ERR_COMMON, "Can't find stream index. One audio or video stream is needed.")
}

void MPLSParser::composePlayList(BitStreamWriter& writer)
{
    const auto lengthPos = UnalignedPtr<uint32_t>(writer.getBuffer() + writer.getBitsCount() / 8);
    writer.putBits(32, 0);  // length
    const int beforeCount = writer.getBitsCount() / 8;
    writer.putBits(16, 0);  // reserved_for_future_use
    const MPLSStreamInfo& mainStreamInfo = getMainStream();
    writer.putBits(16, static_cast<unsigned>(mainStreamInfo.m_index.size()));  // number_of_PlayItems
    writer.putBits(16, number_of_SubPaths);                                    // number_of_SubPaths
    // connection_condition = 1;
    for (size_t PlayItem_id = 0; PlayItem_id < mainStreamInfo.m_index.size(); PlayItem_id++)
    {
        composePlayItem(writer, PlayItem_id, mainStreamInfo.m_index);
        // connection_condition = 6;
    }

    const MPLSStreamInfo& dependStreamInfo = mainStreamInfo;

    // One SubPath() for each sub path the count above declares. Each one already holds a sub play item for every play
    // item (composeSubPath), and the stream entries name sub path 0. The loop ran to number_of_SubPaths times the
    // number of play items, so a disc split into several clips, with picture-in-picture or a Dolby Vision
    // enhancement layer, had as many SubPath() structures as clips under a count of 1.
    for (size_t SubPath_id = 0; SubPath_id < static_cast<size_t>(number_of_SubPaths); SubPath_id++)
    {
        composeSubPath(writer, SubPath_id, dependStreamInfo.m_index, subPath_type);  // pip
    }

    *lengthPos = my_htonl(writer.getBitsCount() / 8 - beforeCount);
}

void MPLSParser::composeSubPath(BitStreamWriter& writer, const size_t subPathNum,
                                const std::vector<PMTIndex>& pmtIndexList, const int type) const
{
    const auto lengthPos = UnalignedPtr<uint32_t>(writer.getBuffer() + writer.getBitsCount() / 8);
    writer.putBits(32, 0);  // length
    const int beforeCount = writer.getBitsCount() / 8;

    writer.putBits(8, 0);     // reserved_for_future_use
    writer.putBits(8, type);  // SubPath_type = 7 (In-mux and Synchronous type of Picture-in-Picture), 5
                              // - MVC depend stream
    writer.putBits(15, 0);    // reserved_for_future_use
    writer.putBits(1, 0);     // is_repeat_SubPath = false
    writer.putBits(8, 0);     // reserved_for_future_use

    writer.putBits(8, static_cast<unsigned>(pmtIndexList.size()));  // number_of_SubPlayItems
    for (size_t i = 0; i < pmtIndexList.size(); i++)
    {
        composeSubPlayItem(writer, i, subPathNum, pmtIndexList);
    }
    *lengthPos = my_htonl(writer.getBitsCount() / 8 - beforeCount);
}

void MPLSParser::composeSubPlayItem(BitStreamWriter& writer, const size_t playItemNum, size_t subPathNum,
                                    const std::vector<PMTIndex>& pmtIndexList) const
{
    const auto lengthPos = UnalignedPtr<uint16_t>(writer.getBuffer() + writer.getBitsCount() / 8);
    writer.putBits(16, 0);  // length
    const int beforeCount = writer.getBitsCount() / 8;

    int fileNum = static_cast<int>(playItemNum);
    if (isDependStreamExist)
    {
        fileNum *= 2;
        fileNum++;
    }

    const std::string clip_Information_file_name = strPadLeft(int32ToStr(fileNum + m_m2tsOffset), 5, '0');
    CLPIStreamInfo::writeString(clip_Information_file_name.c_str(), writer, 5);
    constexpr char clip_codec_identifier[] = "M2TS";
    CLPIStreamInfo::writeString(clip_codec_identifier, writer, 4);
    const int connection_condition = playItemNum == 0 ? 1 : 6;
    writer.putBits(27, 0);  // reserved_for_future_use
    writer.putBits(4, connection_condition);
    writer.putBit(0);  // is_multi_Clip_entries
    writer.putBits(8, ref_to_STC_id);

    if (playItemNum > 0 && !pmtIndexList[playItemNum - 1].empty())
        writer.putBits(32, static_cast<unsigned>(pmtIndexList[playItemNum].begin()->first / 2));
    else
        writer.putBits(32, IN_time);

    if (playItemNum != pmtIndexList.size() - 1 && !pmtIndexList[playItemNum + 1].empty())
        writer.putBits(32, static_cast<unsigned>(pmtIndexList[playItemNum + 1].begin()->first / 2));
    else
        writer.putBits(32, OUT_time);

    writer.putBits(16, static_cast<unsigned>(playItemNum));  // sync_PlayItem_id. reference to play_item id.

    // sync_start_PTS_of_PlayItem
    if (playItemNum > 0 && !pmtIndexList[playItemNum - 1].empty())
        writer.putBits(32, static_cast<unsigned>(pmtIndexList[playItemNum].begin()->first) / 2);
    else
        writer.putBits(32, IN_time);

    // writer.flushBits();
    *lengthPos = my_htons(static_cast<uint16_t>(writer.getBitsCount() / 8 - beforeCount));
}

int MPLSParser::composePip_metadata(uint8_t* buffer, const int bufferSize,
                                    const std::vector<PMTIndex>& pmtIndexList) const
{
    // The ID1 value and the ID2 value of the ExtensionData() shall be set to 0x0001 and 0x0001
    BitStreamWriter writer{};
    writer.setBuffer(buffer, buffer + bufferSize);
    const auto lengthPos = UnalignedPtr<uint32_t>(buffer);
    writer.putBits(32, 0);  // length

    vector<MPLSStreamInfo> pipStreams;
    unsigned mainVSize = 0, mainHSize = 0;
    for (auto& si : m_streamInfo)
    {
        const StreamType stream_coding_type = si.stream_coding_type;
        if (isVideoStreamType(stream_coding_type))
        {
            if (si.isSecondary)
            {
                pipStreams.push_back(si);
            }
            else
            {
                mainHSize = si.width;
                mainVSize = si.height;
            }
        }
    }

    const auto pipStreamsSize = static_cast<unsigned>(pipStreams.size());
    const auto pmtIndexListSize = static_cast<unsigned>(pmtIndexList.size());

    writer.putBits(16, pipStreamsSize * pmtIndexListSize);
    vector<UnalignedPtr<uint32_t>> blockDataAddressPos;
    for (unsigned i = 0; i < pmtIndexListSize; ++i)
    {
        for (unsigned k = 0; k < pipStreamsSize; k++)
        {
            const PIPParams pipParams = pipStreams[k].pipParams;
            // metadata_block_header[k]() {
            writer.putBits(16, i);  // ref_to_PlayItem_id
            writer.putBits(8, k);   // ref_to_secondary_video_stream_id
            writer.putBits(8, 0);   // reserved_for_future_use
            // pip_timeline_type == 1. Synchronous type of Picture-in-Picture
            writer.putBits(4, pipParams.lumma >= 0 ? 1 : 0);
            writer.putBit(1);       // is_luma_key = 0
            writer.putBit(1);       // trick_playing_flag. keep PIP windows when tricking
            writer.putBits(10, 0);  // reserved_for_word_align
            writer.putBits(8, 0);   // reserved_for_future_use
            if (pipParams.lumma >= 0)
            {                                        // is_luma_key==1b
                writer.putBits(8, pipParams.lumma);  // transparent Y pixels
            }
            else
            {
                writer.putBits(8, 0);  // reserved_for_future_use
            }
            writer.putBits(16, 0);  // reserved_for_future_use
            blockDataAddressPos.push_back(UnalignedPtr<uint32_t>(writer.getBuffer() + writer.getBitsCount() / 8));
            writer.putBits(32, 0);  // metadata_block_data_start_address
        }
    }
    while (writer.getBitsCount() % 16 != 0) writer.putBit(0);
    for (size_t i = 0; i < pmtIndexList.size(); ++i)
    {
        for (size_t k = 0; k < pipStreams.size(); ++k)
        {
            PIPParams pipParams = pipStreams[k].pipParams;

            *(blockDataAddressPos[i * pipStreams.size() + k]) = my_htonl(writer.getBitsCount() / 8);

            writer.putBits(16, 1);  // number_of_pip_metadata_entries
            {
                if (i != 0 && !pmtIndexList[i - 1].empty())
                    writer.putBits(32, static_cast<unsigned>(pmtIndexList[i].begin()->first / 2));
                else
                    writer.putBits(32, IN_time);

                unsigned hPos = 0;
                unsigned vPos = 0;

                if (!pipParams.isFullScreen())
                {
                    hPos = pipParams.hOffset;
                    vPos = pipParams.vOffset;

                    const int pipWidth =
                        static_cast<int>(static_cast<float>(pipStreams[k].width) * pipParams.getScaleCoeff());
                    const int pipHeight =
                        static_cast<int>(static_cast<float>(pipStreams[k].height) * pipParams.getScaleCoeff());

                    if (pipParams.corner == PIPParams::PipCorner::TopRight ||
                        pipParams.corner == PIPParams::PipCorner::BottomRight)
                        hPos = mainHSize - pipWidth - pipParams.hOffset;
                    if (pipParams.corner == PIPParams::PipCorner::BottomRight ||
                        pipParams.corner == PIPParams::PipCorner::BottomLeft)
                        vPos = mainVSize - pipHeight - pipParams.vOffset;
                    // An offset or a scale that puts the window outside the main picture was written without a
                    // word, where a position before the picture's edge wrapped round in its 12 bits
                    const int64_t left = static_cast<int64_t>(pipParams.hOffset) + pipWidth;
                    const int64_t top = static_cast<int64_t>(pipParams.vOffset) + pipHeight;
                    if (i == 0 && mainHSize > 0 && mainVSize > 0 && (left > mainHSize || top > mainVSize))
                        LTRACE(LT_WARN, 2,
                               "Warning: the picture-in-picture window of secondary video "
                                   << k + 1 << " (" << pipWidth << "x" << pipHeight
                                   << " at pipHOffset=" << pipParams.hOffset << ", pipVOffset=" << pipParams.vOffset
                                   << ") does not fit in the main picture of " << mainHSize << "x" << mainVSize
                                   << ", so a player shows it cut off or in the wrong place. Use smaller offsets or "
                                      "a smaller pipScale.");
                }

                writer.putBits(12, hPos);
                writer.putBits(12, vPos);

                writer.putBits(4, pipParams.scaleIndex);  // pip_scale[i]. 1 == no_scale
                writer.putBits(4, 0);                     // reserved_for_future_use
            }
            while (writer.getBitsCount() % 16 != 0) writer.putBit(0);
        }
    }

    writer.flushBits();
    *lengthPos = my_htonl(writer.getBitsCount() / 8 - 4);
    return writer.getBitsCount() / 8;
}

// A 3D playlist has a 3D table for EACH play item, one after the other, each led by its length, as a real 3D playlist
// of many play items has them. Only the first was read, and its entries were given to the PG streams of the play
// item whose STN table describes the playlist (chooseRepresentativeStnTable), which need not be the first: a short
// clip in front of the film gave the film's subtitles its own offsets and stereo streams, or none. The table of the
// chosen play item is read now; the first is still read when the playlist has fewer tables than play items.
void MPLSParser::parseStnTableSS(uint8_t* data, uint32_t dataLength)
{
    try
    {
        std::vector<uint32_t> starts;
        for (uint32_t pos = 0; starts.size() < std::max<size_t>(m_playItems.size(), 1) && pos + 2 <= dataLength;)
        {
            starts.push_back(pos);
            pos += 2 + (static_cast<uint32_t>(data[pos]) << 8 | data[pos + 1]);
        }
        if (starts.empty())
            return;
        const uint32_t start = m_chosenStnItem < starts.size() ? starts[m_chosenStnItem] : starts[0];
        BitStreamReader reader{};
        reader.setBuffer(data + start, data + dataLength);
        parseStnTableSSItem(reader);
    }
    catch (...)
    {
    }
}

void MPLSParser::parseStnTableSSItem(BitStreamReader& reader)
{
    reader.skipBits(32);  // len, fixedOffsetDuringPopup, reserved

    for (int i = 0; i < number_of_primary_video_stream_entries; ++i)
    {
        MPLSStreamInfo streamInfo;

        streamInfo.parseStreamEntry(reader);

        reader.skipBits(32);  // attrSSLen, coding type 0x20, format, frameRate
        reader.skipBits(32);  // reserved, number_of_offset_sequences
    }

    for (int i = 0; i < number_of_PG_textST_stream_entries; ++i)
    {
        auto PG_textST_offset_sequence_id = reader.getBits<uint8_t>(8);
        // Entry i here describes entry i of the PG and text subtitle list of the STN table, which STN_table()
        // stored right after the video and audio entries, PG and text subtitles together in their own order.
        // It used to be looked up as the i-th PG stream, so a text subtitle in front of a PG stream gave its
        // values to that PG stream, and each PG stream after it received the values of the entry before.
        const int idx = pgTextIndexToFullIndex(i);
        if (idx != -1)
            m_streamInfo[idx].offsetId = PG_textST_offset_sequence_id;

        // The stereo details belong to a PG stream only. They are always read, so that everything after them
        // stays in step, and kept only for an entry that is a PG stream.
        const int pgIdx = (idx != -1 && m_streamInfo[idx].stream_coding_type == StreamType::SUB_PGS) ? idx : -1;
        reader.skipBits(5);  // reserved, dialog_region_offset_valid
        const bool isSSPG = reader.getBit();
        bool isTopAS = reader.getBit();
        bool isBottomAS = reader.getBit();
        if (pgIdx != -1)
            m_streamInfo[pgIdx].isSSPG = isSSPG;
        if (isSSPG)
        {
            auto leftEye = std::make_unique<MPLSStreamInfo>();
            leftEye->parseStreamEntry(reader);  // left eye
            auto rightEye = std::make_unique<MPLSStreamInfo>();
            rightEye->parseStreamEntry(reader);  // right eye
            reader.skipBits(8);                  // reserved
            const auto ssOffsetId = reader.getBits<uint8_t>(8);
            if (pgIdx != -1)
            {
                m_streamInfo[pgIdx].leftEye = leftEye.release();
                m_streamInfo[pgIdx].rightEye = rightEye.release();
                m_streamInfo[pgIdx].SS_PG_offset_sequence_id = ssOffsetId;
            }
        }
        // The aligned subtitles were read and dropped: the stream was then offered as an unrelated subtitle of
        // the clip, and a disc made from the playlist lost the choice of the subtitle position
        for (int k = 0; k < 2; ++k)
        {
            if (!(k == 0 ? isTopAS : isBottomAS))
                continue;
            MPLSStreamInfo streamInfo;
            streamInfo.parseStreamEntry(reader);
            reader.skipBits(8);  // reserved
            const auto asOffsetId = reader.getBits<uint8_t>(8);
            if (pgIdx != -1)
            {
                MPLSStreamInfo::AlignedRef& ref = k == 0 ? m_streamInfo[pgIdx].topAS : m_streamInfo[pgIdx].bottomAS;
                ref.pid = streamInfo.streamPID;
                ref.type = streamInfo.type;
                ref.offsetId = asOffsetId;
            }
        }
    }
}

void MPLSParser::parseSubPathEntryExtension(uint8_t* data, const uint32_t dataLen)
{
    BitStreamReader reader{};
    reader.setBuffer(data, data + dataLen);
    try
    {
        reader.skipBits(32);  // length
        const auto size = reader.getBits<uint16_t>(16);
        // for (int i = 0; i < size; ++i)
        if (size > 0)
        {
            // subpath extension
            reader.skipBits(32);  // length
            reader.skipBits(8);   // reserved field
            const auto type = reader.getBits<uint8_t>(8);
            if (type == 8 || type == 9)
            {
                reader.skipBits(24);
                // The dependent-view clips used to go into one flat list, every sub play item's clip and
                // every extra clip entry of a multi-clip sub play item alike, and the list is read by play
                // item index. With extra entries (3D with angles), or sub play items that are not one per
                // play item, every later play item would have got another play item's dependent clip
                // (T2-F1). Each play item now gets the clip of the sub play item that names it in its
                // sync_PlayItem_id. The extra entries are the other angles' clips; the main path is read at
                // its first angle, so the first clip is the one that goes with it.
                const auto subPlayItems = reader.getBits<uint8_t>(8);
                for (int i = 0; i < subPlayItems; ++i)
                {
                    reader.skipBits(16);
                    char clip_Information_file_name[6];
                    CLPIStreamInfo::readString(clip_Information_file_name, reader, 5);
                    const std::string clip = clip_Information_file_name;
                    reader.skipBits(32);  // clip codec identifier
                    reader.skipBits(31);  // reserved, condition
                    const bool isMulticlip = reader.getBit();
                    reader.skipBits(8);   // ref to stc id
                    reader.skipBits(32);  // in time
                    reader.skipBits(32);  // out time
                    const auto syncPlayItem = reader.getBits<uint16_t>(16);
                    reader.skipBits(32);  // sync start
                    if (isMulticlip)
                    {
                        const auto numberOfClipEntries = reader.getBits<uint8_t>(8);
                        reader.skipBits(8);  // reserved
                        for (int j = 1; j < numberOfClipEntries; ++j)
                        {
                            CLPIStreamInfo::readString(clip_Information_file_name, reader, 5);
                            reader.skipBits(32);  // clip codec identifier
                            reader.skipBits(8);   // ref to stc id
                        }
                    }
                    if (syncPlayItem >= 1000)  // a playlist has at most 999 play items
                        continue;
                    if (syncPlayItem >= m_mvcFiles.size())
                        m_mvcFiles.resize(syncPlayItem + 1);
                    if (m_mvcFiles[syncPlayItem].empty())
                        m_mvcFiles[syncPlayItem] = clip;
                }
            }
        }
    }
    catch (...)
    {
    }
}

void MPLSParser::parseExtensionData(uint8_t* data, const uint8_t* dataEnd)
{
    try
    {
        BitStreamReader reader{};
        reader.setBuffer(data, dataEnd);
        if (reader.getBits(32) == 0)  // length
            return;

        reader.skipBits(32);  // data_block_start_address
        reader.skipBits(24);
        const auto entries = reader.getBits<uint8_t>(8);
        for (int i = 0; i < entries; ++i)
        {
            const uint32_t dataID = reader.getBits(32);
            const uint32_t dataAddress = reader.getBits(32);
            const uint32_t dataLength = reader.getBits(32);

            if (dataAddress + dataLength > static_cast<uint32_t>(dataEnd - data))
            {
                LTRACE(LT_WARN, 2, "Invalid playlist extension entry skipped.");
                continue;
            }

            if (dataAddress + dataLength > static_cast<uint32_t>(dataEnd - data))
                continue;  // invalid entry

            switch (dataID)
            {
            case 0x00010001:
                // picture-in-picture metadata
                hasPipMetadata = true;
                break;
            case 0x00020001:
                // stn table ss
                isDependStreamExist = true;
                parseStnTableSS(data + dataAddress, dataLength);
                break;
            case 0x00020002:
                // stn table ss
                isDependStreamExist = true;
                parseSubPathEntryExtension(data + dataAddress, dataLength);
                break;
            default:;
            }
        }
    }
    catch (...)
    {
    }
}

void MPLSParser::composeExtensionData(BitStreamWriter& writer, const vector<ExtDataBlockInfo>& extDataBlockInfo)
{
    vector<UnalignedPtr<uint32_t>> extDataStartAddrPos;
    extDataStartAddrPos.resize(extDataBlockInfo.size());
    const auto lengthPos = UnalignedPtr<uint32_t>(writer.getBuffer() + writer.getBitsCount() / 8);
    writer.putBits(32, 0);  // length
    const int initPos = writer.getBitsCount() / 8;
    if (!extDataBlockInfo.empty())
    {
        writer.putBits(32, 0);  // data_block_start_address
        writer.putBits(24, 0);  // reserved_for_word_align
        const auto extDataBlockInfoSize = static_cast<unsigned>(extDataBlockInfo.size());
        writer.putBits(8, extDataBlockInfoSize);
        for (unsigned i = 0; i < extDataBlockInfoSize; ++i)
        {
            writer.putBits(16, extDataBlockInfo[i].id1);
            writer.putBits(16, extDataBlockInfo[i].id2);
            extDataStartAddrPos[i] = UnalignedPtr<uint32_t>(writer.getBuffer() + writer.getBitsCount() / 8);
            writer.putBits(32, 0);                                                       // ext_data_start_address
            writer.putBits(32, static_cast<unsigned>(extDataBlockInfo[i].data.size()));  // ext_data_length
        }
        while ((writer.getBitsCount() / 8 - initPos) % 4 != 0) writer.putBits(16, 0);
        lengthPos[1] = my_htonl(writer.getBitsCount() / 8 - initPos + 4);  // data_block_start_address
        for (unsigned i = 0; i < extDataBlockInfoSize; ++i)
        {
            *(extDataStartAddrPos[i]) = my_htonl(writer.getBitsCount() / 8 - initPos + 4);
            for (const auto& j : extDataBlockInfo[i].data) writer.putBits(8, j);
        }
    }

    *lengthPos = my_htonl(writer.getBitsCount() / 8 - initPos);
}

void MPLSParser::parsePlayItem(BitStreamReader& reader, const int PlayItem_id)
{
    MPLSPlayItem newItem;
    reader.skipBits(16);  // length
    char clip_Information_file_name[6];
    char clip_codec_identifier[5];
    CLPIStreamInfo::readString(clip_Information_file_name, reader, 5);
    newItem.fileName = clip_Information_file_name;
    CLPIStreamInfo::readString(clip_codec_identifier, reader, 4);
    reader.skipBits(11);  // reserved_for_future_use
    is_multi_angle = reader.getBit();
    newItem.connection_condition = reader.getBits<uint8_t>(4);
    ref_to_STC_id = reader.getBits<uint8_t>(8);

    newItem.IN_time = reader.getBits(32);
    newItem.OUT_time = reader.getBits(32);
    m_playItems.push_back(newItem);

    UO_mask_table(reader);
    PlayItem_random_access_flag = reader.getBit();
    reader.skipBits(7);  // reserved_for_future_use
    m_playItems.back().still_mode = reader.getBits<uint8_t>(8);
    reader.skipBits(16);  // still_time, or reserved_for_future_use

    if (is_multi_angle == 1)
    {
        number_of_angles = reader.getBits<uint8_t>(8);
        m_playItems.back().angles = number_of_angles;
        reader.skipBits(6);  // reserved_for_future_use
        is_different_audios = reader.getBit();
        is_seamless_angle_change = reader.getBit();
        for (int angle_id = 1;  // angles except angle_id=0
             angle_id < number_of_angles; angle_id++)
        {
            CLPIStreamInfo::readString(clip_Information_file_name, reader, 5);
            CLPIStreamInfo::readString(clip_codec_identifier, reader, 4);
            ref_to_STC_id = reader.getBits<uint8_t>(8);
            m_playItems.back().angleFiles.emplace_back(clip_Information_file_name);
        }
    }
    STN_table(reader, PlayItem_id);
}

void MPLSParser::composePlayItem(BitStreamWriter& writer, const size_t playItemNum,
                                 const std::vector<PMTIndex>& pmtIndexList)
{
    const auto lengthPos = UnalignedPtr<uint16_t>(writer.getBuffer() + writer.getBitsCount() / 8);
    writer.putBits(16, 0);  // length
    const int beforeCount = writer.getBitsCount() / 8;
    int fileNum = static_cast<int>(playItemNum);
    if (isDependStreamExist)
        fileNum *= 2;
    const std::string clip_Information_file_name = strPadLeft(int32ToStr(fileNum + m_m2tsOffset), 5u, '0');
    CLPIStreamInfo::writeString(clip_Information_file_name.c_str(), writer, 5);
    constexpr char clip_codec_identifier[] = "M2TS";
    CLPIStreamInfo::writeString(clip_codec_identifier, writer, 4);
    writer.putBits(11, 0);  // reserved_for_future_use
    writer.putBit(0);       // is_multi_angle
    const int connection_condition = playItemNum == 0 ? 1 : 6;
    writer.putBits(4, connection_condition);
    writer.putBits(8, ref_to_STC_id);

    if (playItemNum != 0 && !pmtIndexList[playItemNum - 1].empty())
        writer.putBits(32, static_cast<unsigned>(pmtIndexList[playItemNum].begin()->first) / 2);
    else
        writer.putBits(32, IN_time);

    if (playItemNum != pmtIndexList.size() - 1 && !pmtIndexList[playItemNum + 1].empty())
        writer.putBits(32, static_cast<unsigned>(pmtIndexList[playItemNum + 1].begin()->first / 2));
    else
        writer.putBits(32, OUT_time);

    writer.putBits(28, 0);               // UO_mask_table;
    writer.putBits(4, isV3() ? 15 : 0);  // UO_mask_table;
    writer.putBit(0);                    // reserved
    writer.putBit(isV3() ? 1 : 0);       // UO_mask_table: SecondaryPGStreamNumberChange
    writer.putBits(30, 0);               // UO_mask_table cont;

    writer.putBit(PlayItem_random_access_flag);
    writer.putBits(7, 0);   // reserved_for_future_use
    writer.putBits(8, 0);   // still_mode
    writer.putBits(16, 0);  // reserved after stillMode != 0x01
    composeSTN_table(writer, playItemNum, false);
    *lengthPos = my_htons(static_cast<uint16_t>(writer.getBitsCount() / 8 - beforeCount));
}

void MPLSParser::parsePlayListMark(uint8_t* buffer, const int len)
{
    BitStreamReader reader{};
    reader.setBuffer(buffer, buffer + len);
    reader.skipBits(32);  // length
    const auto number_of_PlayList_marks = reader.getBits<uint16_t>(16);
    for (int PL_mark_id = 0; PL_mark_id < number_of_PlayList_marks; PL_mark_id++)
    {
        reader.skipBits(8);  // reserved_for_future_use
        const auto mark_type = reader.getBits<uint8_t>(8);
        const auto ref_to_PlayItem_id = reader.getBits<uint16_t>(16);
        const uint32_t mark_time_stamp = reader.getBits(32);
        reader.skipBits(16);  // entry_ES_PID
        reader.skipBits(32);  // duration
        if (mark_type == 1)   // mark_type 0x01 = Chapter search
            m_marks.emplace_back(ref_to_PlayItem_id, mark_time_stamp);
    }
}

int MPLSParser::calcPlayItemID(const MPLSStreamInfo& streamInfo, const uint32_t pts)
{
    for (size_t i = 0; i < streamInfo.m_index.size(); i++)
    {
        if (!streamInfo.m_index[i].empty())
        {
            if (streamInfo.m_index[i].begin()->first > pts)
                return FFMAX((int)i, 1) - 1;
        }
    }
    return static_cast<int>(streamInfo.m_index.size()) - 1;
}

void MPLSParser::composePlayListMark(BitStreamWriter& writer)
{
    const auto lengthPos = UnalignedPtr<uint32_t>(writer.getBuffer() + writer.getBitsCount() / 8);
    writer.putBits(32, 0);  // length
    const int beforeCount = writer.getBitsCount() / 8;
    const MPLSStreamInfo& streamInfo = getMainStream();
    if (m_marks.empty())
    {
        if (m_chapterLen <= 0)
            m_marks.emplace_back(-1, IN_time);
        else
        {
            // The step and the sum in 64 bits. In 32 bits a long chapter length wrapped, and so did the
            // sum near the top of the range, so marks came every few seconds, came before the start, or
            // never stopped coming.
            const int64_t step = static_cast<int64_t>(m_chapterLen) * 45000;
            for (int64_t t = IN_time; t < OUT_time; t += step) m_marks.emplace_back(-1, static_cast<uint32_t>(t));
            // A playlist holds at most 999 marks; a short step on a long title gave more, and the playlist outgrew
            // the room it is written into at the end of the mux.
            if (m_marks.size() > 999)
            {
                LTRACE(LT_WARN, 2,
                       "Warning: --auto-chapters gives " << m_marks.size()
                                                         << " chapter marks, and a Blu-ray playlist holds at most "
                                                            "999. The first 999 are kept.");
                m_marks.erase(m_marks.begin() + 999, m_marks.end());
            }
        }
    }
    writer.putBits(16, static_cast<unsigned>(m_marks.size()));
    for (const auto& i : m_marks)
    {
        writer.putBits(8, 0);  // reserved_for_future_use
        writer.putBits(8, 1);  // mark_type 0x01 = Chapter search
        if (i.m_playItemID >= 0)
            writer.putBits(16, i.m_playItemID);  // play item ID
        else
            writer.putBits(16, calcPlayItemID(streamInfo, i.m_markTime * 2));  // play item ID
        writer.putBits(32, i.m_markTime);
        writer.putBits(16, 0xffff);  // entry_ES_PID always 0xffff for mark_type 1
        writer.putBits(32, 0);       // duration always 0 for mark_type 1
    }
    *lengthPos = my_htonl(writer.getBitsCount() / 8 - beforeCount);
}

void MPLSParser::composeSTN_table(BitStreamWriter& writer, const size_t PlayItem_id, const bool isSSEx)
{
    const auto lengthPos = UnalignedPtr<uint16_t>(writer.getBuffer() + writer.getBitsCount() / 8);
    writer.putBits(16, 0);  // length
    const int beforeCount = writer.getBitsCount() / 8;
    writer.putBit(0);       // Fixed_offset_during_PopUp_flag for ext mode
    writer.putBits(15, 0);  // reserved_for_future_use

    number_of_primary_video_stream_entries = 0;
    number_of_secondary_video_stream_entries = 0;
    number_of_primary_audio_stream_entries = 0;
    number_of_secondary_audio_stream_entries = 0;
    number_of_PG_textST_stream_entries = 0;
    number_of_DolbyVision_video_stream_entries = 0;

    std::vector<MPLSStreamInfo>& streamInfo = isSSEx ? m_streamInfoMVC : m_streamInfo;
    int numberOfPiPPg = 0;

    for (const auto& i : streamInfo)
    {
        StreamType stream_coding_type = i.stream_coding_type;
        if (isVideoStreamType(stream_coding_type))
        {
            if (i.isSecondary)
                number_of_secondary_video_stream_entries++;
            else if (i.isDvEnhancement)
                number_of_DolbyVision_video_stream_entries++;
            else
                number_of_primary_video_stream_entries++;
        }
        else if (isAudioStreamType(stream_coding_type))
        {
            if (!i.isSecondary)
                number_of_primary_audio_stream_entries++;
            else
                number_of_secondary_audio_stream_entries++;
        }
        else if (stream_coding_type == StreamType::SUB_PGS)
        {
            // An eye of a stereo subtitle or an aligned subtitle is named by the entry it belongs to, in the 3D
            // table, and is no entry of its own; picture-in-picture subtitles are counted on their own below
            if (i.isPiP)
                numberOfPiPPg++;
            else if (!i.hangsOffEntry)
                number_of_PG_textST_stream_entries++;
        }
        else
        {
            // Not a Blu-ray/AVCHD stream type this muxer can place in the playlist STN table: DVB
            // subtitles (0x06), SCTE-27, BD text subtitles (textST 0x92) and interactive menus (IG
            // 0x91) all land here. Skip the stream from the playlist with a warning instead of a THROW
            // that would destroy a finished, multi-hour mux at the playlist stage. PGS is
            // the supported subtitle type; text/SRT input is rendered to PGS upstream and arrives as
            // SUB_PGS. IG (menus) stays uncounted until menu authoring is implemented.
            LTRACE(LT_WARN, 2,
                   "Blu-ray playlist: skipping a stream with unsupported coding type "
                       << static_cast<int>(stream_coding_type)
                       << " (not PGS) - it will not be selectable. Menus (IG) and DVB/text subtitles "
                          "are not supported for authoring yet.");
        }
    }

    if (!isSSEx)
    {
        writer.putBits(8, number_of_primary_video_stream_entries);  // test subpath
        writer.putBits(8, number_of_primary_audio_stream_entries);
        writer.putBits(8, number_of_PG_textST_stream_entries);
        writer.putBits(8, 0);  // int number_of_IG_stream_entries = reader.getBits(8);
        writer.putBits(8, number_of_secondary_audio_stream_entries);
        writer.putBits(8,
                       number_of_secondary_video_stream_entries);  // int number_of_secondary_video_stream_entries.
                                                                   // Now subpath used for second video stream
        // The subtitles of the picture-in-picture video, when that video is on the disc too; without it they are
        // left out of the table (BlurayHelper::createMPLSFile says so), as no player could show them
        if (number_of_secondary_video_stream_entries == 0)
            numberOfPiPPg = 0;
        writer.putBits(8, numberOfPiPPg);  // number_of_PiP_PG_textST_stream_entries_plus
        writer.putBits(8, number_of_DolbyVision_video_stream_entries);
        writer.putBits(32, 0);  // reserved_for_future_use
    }

    // video
    for (auto& i : streamInfo)
    {
        const StreamType stream_coding_type = i.stream_coding_type;
        if (isVideoStreamType(stream_coding_type) && !i.isSecondary && !i.isDvEnhancement)
        {
            i.composeStreamEntry(writer, PlayItem_id);
            i.composeStreamAttributes(writer);
            if (stream_coding_type == StreamType::VIDEO_MVC)
            {
                writer.putBits(10, 0);  // reserved_for_future_use
                writer.putBits(6, FFMAX(1, i.number_of_offset_sequences));
            }
        }
    }

    // primary audio
    for (auto& i : streamInfo)
    {
        const StreamType stream_coding_type = i.stream_coding_type;
        if (isAudioStreamType(stream_coding_type) && !i.isSecondary)
        {
            i.composeStreamEntry(writer, PlayItem_id);
            i.composeStreamAttributes(writer);
        }
    }

    // PG: the ordinary entries, then the picture-in-picture ones, which have no 3D table entry. Every PG stream used
    // to be written as an ordinary entry: the eyes of a stereo subtitle came first, without a language, and took
    // the numbers the disc start and a player select subtitles by; picture-in-picture subtitles became ordinary
    // ones after the film's own.
    for (int pass = 0; pass < (isSSEx || numberOfPiPPg == 0 ? 1 : 2); ++pass)
    {
        for (auto& i : m_streamInfo)
        {
            const StreamType stream_coding_type = i.stream_coding_type;
            if (stream_coding_type != StreamType::SUB_PGS || i.hangsOffEntry || i.isPiP != (pass == 1))
                continue;
            if (isSSEx)
            {
                i.composePGS_SS_StreamEntry(writer, PlayItem_id);
            }
            else
            {
                i.composeStreamEntry(writer, PlayItem_id);
                i.composeStreamAttributes(writer);
            }
        }
    }

    // secondary audio
    for (auto& i : streamInfo)
    {
        if (isAudioStreamType(i.stream_coding_type) && i.isSecondary)
        {
            i.composeStreamEntry(writer, PlayItem_id);
            i.composeStreamAttributes(writer);
            if (number_of_secondary_video_stream_entries == 0)
            {
                writer.putBits(8, number_of_primary_audio_stream_entries);  // number_of_primary_audio_ref_entries
                                                                            // allowed to join with this stream
                writer.putBits(8, 0);                                       // word align
                uint8_t primaryAudioNum = 0;
                for (const auto& j : streamInfo)
                {
                    if (isAudioStreamType(j.stream_coding_type) && !j.isSecondary)
                        writer.putBits(8, primaryAudioNum++);
                }
                if (number_of_primary_audio_stream_entries % 2 == 1)
                    writer.putBits(8, 0);  // word align
            }
            else
            {
                writer.putBits(16, 0);
            }
        }
    }

    // secondary video
    int secondaryVNum = 0;
    for (auto& i : streamInfo)
    {
        const StreamType stream_coding_type = i.stream_coding_type;
        if (i.isSecondary && isVideoStreamType(stream_coding_type))
        {
            i.type = 3;
            i.composeStreamEntry(writer, PlayItem_id, secondaryVNum);
            i.composeStreamAttributes(writer);

            // Every secondary audio stream, as a disc names its commentaries for its picture-in-picture video.
            // Only the one with the number of this secondary video was named, so of two commentaries the second was
            // not tied to the picture-in-picture. As with its subtitles below, the meta does not say which audio
            // belongs to which secondary video.
            const int secondaryAudioRefs = number_of_secondary_audio_stream_entries;
            writer.putBits(8, secondaryAudioRefs);                              // number_of_secondary_audio_ref_entries
            writer.putBits(8, 0);                                               // reserved_for_word_align
            for (int k = 0; k < secondaryAudioRefs; ++k) writer.putBits(8, k);  // secondary_audio_stream_id_ref
            if (secondaryAudioRefs & 1)
                writer.putBits(8, 0);  // reserved_for_word_align

            // The picture-in-picture subtitles by their place in the PG list, after the ordinary entries, as a real
            // disc names them. Each secondary video is given all of them: the meta does not say which belongs to
            // which, and a disc has one picture-in-picture video.
            writer.putBits(8, numberOfPiPPg);  // number_of_PiP_PG_textST_ref_entries
            writer.putBits(8, 0);              // reserved_for_word_align
            for (int k = 0; k < numberOfPiPPg; ++k)
                writer.putBits(8, number_of_PG_textST_stream_entries + k);  // PiP_PG_textST_stream_id_ref
            if (numberOfPiPPg & 1)
                writer.putBits(8, 0);  // reserved_for_word_align
            secondaryVNum++;
        }
    }

    // DV video
    for (auto& i : streamInfo)
    {
        const StreamType stream_coding_type = i.stream_coding_type;
        if (isVideoStreamType(stream_coding_type) && i.isDvEnhancement)
        {
            i.type = 4;
            i.composeStreamEntry(writer, PlayItem_id);
            i.composeStreamAttributes(writer);
        }
    }

    if (isSSEx && writer.getBitsCount() % 32 != 0)
        writer.putBits(16, 0);

    *lengthPos = my_htons(static_cast<uint16_t>(writer.getBitsCount() / 8 - beforeCount));
}

void MPLSParser::STN_table(BitStreamReader& reader, int PlayItem_id)
{
    reader.skipBits(32);  // length, reserved_for_future_use
    number_of_primary_video_stream_entries = reader.getBits<uint8_t>(8);
    number_of_primary_audio_stream_entries = reader.getBits<uint8_t>(8);
    number_of_PG_textST_stream_entries = reader.getBits<uint8_t>(8);
    number_of_IG_stream_entries = reader.getBits<uint8_t>(8);
    number_of_secondary_audio_stream_entries = reader.getBits<uint8_t>(8);
    number_of_secondary_video_stream_entries = reader.getBits<uint8_t>(8);
    number_of_PiP_PG_textST_stream_entries_plus = reader.getBits<uint8_t>(8);
    number_of_DolbyVision_video_stream_entries = reader.getBits<uint8_t>(8);
    reader.skipBits(32);  // reserved_for_future_use

    // Each play item has its own STN table and they need not agree. Keep them all, with the entry
    // counts that belong to each, and decide afterwards which one describes the playlist.
    if (m_stnTables.size() <= static_cast<size_t>(PlayItem_id))
        m_stnTables.resize(static_cast<size_t>(PlayItem_id) + 1);
    MPLSStnTable& stnTable = m_stnTables[PlayItem_id];
    stnTable.counts[0] = number_of_primary_video_stream_entries;
    stnTable.counts[1] = number_of_primary_audio_stream_entries;
    stnTable.counts[2] = number_of_PG_textST_stream_entries;
    stnTable.counts[3] = number_of_IG_stream_entries;
    stnTable.counts[4] = number_of_secondary_audio_stream_entries;
    stnTable.counts[5] = number_of_secondary_video_stream_entries;
    stnTable.counts[6] = number_of_PiP_PG_textST_stream_entries_plus;
    stnTable.counts[7] = number_of_DolbyVision_video_stream_entries;

    for (int primary_video_stream_id = 0; primary_video_stream_id < number_of_primary_video_stream_entries;
         primary_video_stream_id++)
    {
        MPLSStreamInfo streamInfo;
        streamInfo.parseStreamEntry(reader);
        streamInfo.parseStreamAttributes(reader);
        m_stnTables[PlayItem_id].streams.push_back(streamInfo);
    }
    for (int primary_audio_stream_id = 0; primary_audio_stream_id < number_of_primary_audio_stream_entries;
         primary_audio_stream_id++)
    {
        MPLSStreamInfo streamInfo;
        streamInfo.parseStreamEntry(reader);
        streamInfo.parseStreamAttributes(reader);
        m_stnTables[PlayItem_id].streams.push_back(streamInfo);
    }

    for (int PG_textST_stream_id = 0;
         PG_textST_stream_id < number_of_PG_textST_stream_entries + number_of_PiP_PG_textST_stream_entries_plus;
         PG_textST_stream_id++)
    {
        MPLSStreamInfo streamInfo;
        // The entries after the ordinary ones are the subtitles of the picture-in-picture video: the secondary video
        // entry names them by their place in this list (PiP_PG_textST_stream_id_ref: on a disc the numbers
        // that follow those of the ordinary entries). They were read as ordinary subtitles, and offered and written
        // as ones, numbered after the film's own.
        streamInfo.isPiP = PG_textST_stream_id >= number_of_PG_textST_stream_entries;
        streamInfo.parseStreamEntry(reader);
        streamInfo.parseStreamAttributes(reader);
        m_stnTables[PlayItem_id].streams.push_back(streamInfo);
    }

    for (int IG_stream_id = 0; IG_stream_id < number_of_IG_stream_entries; IG_stream_id++)
    {
        MPLSStreamInfo streamInfo;
        streamInfo.parseStreamEntry(reader);
        streamInfo.parseStreamAttributes(reader);
        m_stnTables[PlayItem_id].streams.push_back(streamInfo);
    }

    for (int secondary_audio_stream_id = 0; secondary_audio_stream_id < number_of_secondary_audio_stream_entries;
         secondary_audio_stream_id++)
    {
        MPLSStreamInfo streamInfo;
        streamInfo.isSecondary = true;
        streamInfo.parseStreamEntry(reader);
        streamInfo.parseStreamAttributes(reader);
        m_stnTables[PlayItem_id].streams.push_back(streamInfo);

        const auto number_of_primary_audio_ref_entries = reader.getBits<uint8_t>(8);
        reader.skipBits(8);  // reserved_for_word_align

        for (int i = 0; i < number_of_primary_audio_ref_entries; i++)
            reader.skipBits(8);  // primary_audio_stream_id_ref

        if (number_of_primary_audio_ref_entries & 1)
            reader.skipBits(8);  // reserved_for_word_align
    }

    for (int secondary_video_stream_id = 0; secondary_video_stream_id < number_of_secondary_video_stream_entries;
         secondary_video_stream_id++)
    {
        MPLSStreamInfo streamInfo;
        streamInfo.isSecondary = true;
        streamInfo.parseStreamEntry(reader);
        streamInfo.parseStreamAttributes(reader);
        m_stnTables[PlayItem_id].streams.push_back(streamInfo);

        const auto number_of_secondary_audio_ref_entries = reader.getBits<uint8_t>(8);
        reader.skipBits(8);  // reserved_for_word_align

        for (int i = 0; i < number_of_secondary_audio_ref_entries; i++)
            reader.skipBits(8);  // secondary_audio_stream_id_ref

        if (number_of_secondary_audio_ref_entries & 1)
            reader.skipBits(8);  // reserved_for_word_align

        const auto number_of_PiP_PG_textST_ref_entries = reader.getBits<uint8_t>(8);
        reader.skipBits(8);  // reserved_for_word_align

        for (int i = 0; i < number_of_PiP_PG_textST_ref_entries; i++)
            reader.skipBits(8);  // PiP_PG_textST_stream_id_ref

        if (number_of_PiP_PG_textST_ref_entries & 1)
            reader.skipBits(8);  // reserved_for_word_align
    }

    for (int DV_video_stream_id = 0; DV_video_stream_id < number_of_DolbyVision_video_stream_entries;
         DV_video_stream_id++)
    {
        MPLSStreamInfo streamInfo;
        streamInfo.parseStreamEntry(reader);
        streamInfo.parseStreamAttributes(reader);
        m_stnTables[PlayItem_id].streams.push_back(streamInfo);
    }
}

// ------------- M2TSStreamInfo -----------------------

void M2TSStreamInfo::blurayStreamParams(const double fps, const bool interlaced, const unsigned width,
                                        const unsigned height, const VideoAspectRatio ar, uint8_t* video_format,
                                        uint8_t* frame_rate_index, uint8_t* aspect_ratio_index)
{
    *video_format = 0;
    *frame_rate_index = 0;
    *aspect_ratio_index = 3;  // 16:9; 2 = 4:3

    const bool isNtsc = width <= 854 && height <= 480 && (fabs(25 - fps) >= 0.5 && fabs(50 - fps) >= 0.5);
    const bool isPal = width <= 1024 && height <= 576 && (fabs(25 - fps) < 0.5 || fabs(50 - fps) < 0.5);
    if (isNtsc)
        *video_format = interlaced ? 1 : 3;
    else if (isPal)
        *video_format = interlaced ? 2 : 7;
    else if (width >= 2600)
        *video_format = 8;
    else if (width >= 1300)
        *video_format = interlaced ? 4 : 6;  // as 1920x1080
    else
        *video_format = 5;  // as 1280x720

    if (fabs(fps - 23.976) < 1e-4)
        *frame_rate_index = 1;
    else if (fabs(fps - 24.0) < 1e-4)
        *frame_rate_index = 2;
    else if (fabs(fps - 25.0) < 1e-4)
        *frame_rate_index = 3;
    else if (fabs(fps - 29.97) < 1e-4)
        *frame_rate_index = 4;
    else if (fabs(fps - 50.0) < 1e-4)
        *frame_rate_index = 6;
    else if (fabs(fps - 59.94) < 1e-4)
        *frame_rate_index = 7;

    if (ar == VideoAspectRatio::AR_3_4 || ar == VideoAspectRatio::AR_VGA)
        *aspect_ratio_index = 2;  // 4x3
}

M2TSStreamInfo::M2TSStreamInfo(const PMTStreamInfo& pmtStreamInfo)
{
    streamPID = pmtStreamInfo.m_pid;
    stream_coding_type = pmtStreamInfo.m_streamType;
    m_index = pmtStreamInfo.m_index;
    isSecondary = pmtStreamInfo.isSecondary;
    isDvEnhancement = pmtStreamInfo.isDvEnhancement;
    // A stream without a language is "und" (undetermined, ISO 639-2), as the listing and a Matroska output name it: the
    // three zero bytes it got in the playlist and the clip information are no language code.
    memset(&language_code, 0, 4);
    memcpy(language_code, *pmtStreamInfo.m_lang ? pmtStreamInfo.m_lang : "und", 3);

    character_code = 0;
    video_format = 0;
    frame_rate_index = 0;
    number_of_offset_sequences = 0;
    audio_presentation_type = 0;
    sampling_frequency_index = 0;
    aspect_ratio_index = 3;  // 16:9; 2 = 4:3

    if (pmtStreamInfo.m_codecReader != nullptr)
    {
        const auto vStream = dynamic_cast<MPEGStreamReader*>(pmtStreamInfo.m_codecReader);
        if (vStream)
        {
            width = vStream->getStreamWidth();
            height = vStream->getStreamHeight();
            HDR = vStream->getStreamHDR();
            // The low four bits of the byte after the frame rate are the colour space (1 BT.709, 2 BT.2020), the high
            // four the dynamic range. An HDR stream writes 0x12 and Dolby Vision 0x22. An SDR stream wrote 0x00, BT.709
            // or BT.2020; a UHD disc has 0x01 there for SDR HEVC with BT.709 colours and 0x02 for BT.2020, in
            // every playlist and clip information file, for the same signalling in its stream (the colour primaries,
            // no HDR). HEVC that gives no primaries keeps 0. H.264 has no such field; the disc writes 0 there.
            uint8_t primaries = 0, transfer = 0, matrix = 0;
            if (pmtStreamInfo.m_streamType == StreamType::VIDEO_H265 &&
                vStream->getColourDesc(primaries, transfer, matrix))
            {
                if (primaries == 9)
                    colour_space = 2;
                else if (primaries == 1)
                    colour_space = 1;
            }
            const VideoAspectRatio ar = vStream->getStreamAR();
            blurayStreamParams(vStream->getFPS(), vStream->getInterlaced(), width, height, ar, &video_format,
                               &frame_rate_index, &aspect_ratio_index);
            if (ar == VideoAspectRatio::AR_3_4)
                width = height * 4 / 3;
            else if (ar == VideoAspectRatio::AR_16_9)
                width = height * 16 / 9;
            else if (ar == VideoAspectRatio::AR_221_100)
                width = height * 221 / 100;
        }
        const auto h264Stream = dynamic_cast<H264StreamReader*>(pmtStreamInfo.m_codecReader);
        if (h264Stream)
            number_of_offset_sequences = h264Stream->getOffsetSeqCnt();

        const auto aStream = dynamic_cast<SimplePacketizerReader*>(pmtStreamInfo.m_codecReader);
        if (aStream)
        {
            // The first layout, the one the PMT gives (PMTStreamInfo::m_firstChannels); the reader holds the last.
            const bool first = pmtStreamInfo.m_firstFreq > 0;
            audio_presentation_type = first ? pmtStreamInfo.m_firstChannels : aStream->getChannels();
            if (audio_presentation_type == 2)
                audio_presentation_type = 3;
            else if (audio_presentation_type > 3)
                audio_presentation_type = 6;
            // A 48 kHz core under a part at 96 or 192 kHz is code 14 or 12, as a disc writes it: an AC-3 reader gives
            // its core as the rate, and DTS-HD and TrueHD merged with an AC-3 core give it apart. They were 4 or 5,
            // as for a stream without a core, and the DTS core alone of down-to-dts was 4 at 48 kHz.
            const int coreFreq = first ? pmtStreamInfo.m_firstCoreFreq : aStream->getCoreFreq();
            const int freq = coreFreq ? coreFreq : (first ? pmtStreamInfo.m_firstFreq : aStream->getFreq());
            const int altFreq = first ? pmtStreamInfo.m_firstAltFreq : aStream->getAltFreq();
            switch (freq)
            {
            case 48000:
                if (altFreq == 96000)
                    sampling_frequency_index = 14;
                else if (altFreq == 192000)
                    sampling_frequency_index = 12;
                else
                    sampling_frequency_index = 1;
                break;
            case 96000:
                if (altFreq == 192000)
                    sampling_frequency_index = 12;
                else
                    sampling_frequency_index = 4;
                break;
            case 192000:
                sampling_frequency_index = 5;
                break;
            default:;
            }
        }
    }
}

M2TSStreamInfo::M2TSStreamInfo(const M2TSStreamInfo& other)
{
    streamPID = other.streamPID;
    stream_coding_type = other.stream_coding_type;
    video_format = other.video_format;
    frame_rate_index = other.frame_rate_index;
    number_of_offset_sequences = other.number_of_offset_sequences;
    width = other.width;
    height = other.height;
    HDR = other.HDR;
    colour_space = other.colour_space;
    aspect_ratio_index = other.aspect_ratio_index;
    audio_presentation_type = other.audio_presentation_type;
    sampling_frequency_index = other.sampling_frequency_index;
    character_code = other.character_code;
    memcpy(language_code, other.language_code, sizeof(language_code));
    isSecondary = other.isSecondary;
    isDvEnhancement = other.isDvEnhancement;
    m_index = other.m_index;
}

MPLSStreamInfo::MPLSStreamInfo(const MPLSStreamInfo& other) : M2TSStreamInfo(other)
{
    type = other.type;
    character_code = other.character_code;
    offsetId = other.offsetId;
    pipParams = other.pipParams;
    isSSPG = other.isSSPG;
    SS_PG_offset_sequence_id = other.SS_PG_offset_sequence_id;
    refSubPathId = other.refSubPathId;
    isPiP = other.isPiP;
    hangsOffEntry = other.hangsOffEntry;
    topAS = other.topAS;
    bottomAS = other.bottomAS;
    leftEye = nullptr;
    rightEye = nullptr;
    if (other.leftEye)
        leftEye = new MPLSStreamInfo(*other.leftEye);
    if (other.rightEye)
        rightEye = new MPLSStreamInfo(*other.rightEye);
}

// -------------- MPLSStreamInfo -----------------------

MPLSStreamInfo::MPLSStreamInfo()
    : type(0), offsetId(0xff), isSSPG(false), SS_PG_offset_sequence_id(0xff), leftEye(nullptr), rightEye(nullptr)
{
}

MPLSStreamInfo::MPLSStreamInfo(const PMTStreamInfo& pmtStreamInfo)
    : M2TSStreamInfo(pmtStreamInfo),
      type(1),
      offsetId(0xff),
      isSSPG(false),
      SS_PG_offset_sequence_id(0xff),
      leftEye(nullptr),
      rightEye(nullptr)
{
    pipParams = pmtStreamInfo.m_codecReader->getPipParams();
}

MPLSStreamInfo::~MPLSStreamInfo()
{
    delete leftEye;
    delete rightEye;
}

void MPLSStreamInfo::parseStreamEntry(BitStreamReader& reader)
{
    reader.skipBits(8);  // length
    type = reader.getBits<uint8_t>(8);
    if (type == 1)
    {
        streamPID = reader.getBits<int>(16);
        reader.skipBits(32);
        reader.skipBits(16);
    }
    else if (type == 2)
    {
        refSubPathId = reader.getBits<int>(8);
        reader.skipBits(8);  // ref_to_subClip_entry_id
        streamPID = reader.getBits<int>(16);
        reader.skipBits(32);
    }
    else if (type == 3 || type == 4)
    {
        refSubPathId = reader.getBits<int>(8);
        streamPID = reader.getBits<int>(16);
        reader.skipBits(32);
        reader.skipBits(8);
    }
}

void MPLSStreamInfo::composePGS_SS_StreamEntry(BitStreamWriter& writer, const size_t entryNum) const
{
    writer.putBits(8, offsetId);
    writer.putBits(4, 0);              // reserved
    writer.putBit(0);                  // dialog region offset valid
    writer.putBit(isSSPG);             // is SS PG
    writer.putBit(topAS.pid != 0);     // top AS PG
    writer.putBit(bottomAS.pid != 0);  // bottom AS PG
    if (isSSPG)
    {
        leftEye->composeStreamEntry(writer, entryNum);
        rightEye->composeStreamEntry(writer, entryNum);
        writer.putBits(8, 0);  // reserved
        writer.putBits(8, SS_PG_offset_sequence_id);
    }
    for (const AlignedRef* ref : {&topAS, &bottomAS})
    {
        if (ref->pid == 0)
            continue;
        MPLSStreamInfo entry;
        entry.type = ref->type;
        entry.streamPID = ref->pid;
        entry.composeStreamEntry(writer, entryNum);
        writer.putBits(8, 0);  // reserved
        writer.putBits(8, ref->offsetId);
    }
}

void MPLSStreamInfo::composeStreamEntry(BitStreamWriter& writer, size_t entryNum, const int subPathID) const
{
    uint8_t* lengthPos = writer.getBuffer() + writer.getBitsCount() / 8;
    writer.putBits(8, 0);  // length
    const int initPos = writer.getBitsCount() / 8;
    writer.putBits(8, type);
    if (type == 1)
    {
        writer.putBits(16, streamPID);
        writer.putBits(32, 0);
        writer.putBits(16, 0);
    }
    else if (type == 2)
    {
        writer.putBits(8, 0);  // ref_to_SubPath_id
        writer.putBits(8, 0);  // entryNum - ref_to_subClip_entry_id
        writer.putBits(16, streamPID);
        writer.putBits(32, 0);
    }
    else if (type == 3)
    {
        writer.putBits(8, subPathID);  // ref_to_SubPath_id. constant subPathID == 0
        writer.putBits(16, streamPID);
        writer.putBits(32, 0);
        writer.putBits(8, 0);
    }
    else if (type == 4)
    {
        writer.putBits(8, 0);
        writer.putBits(16, streamPID);
        writer.putBits(32, 0);
        writer.putBits(8, 0);
    }
    else
        THROW(ERR_COMMON, "Unsupported media type for AVCHD/Blu-ray muxing")
    *lengthPos = static_cast<uint8_t>(writer.getBitsCount() / 8 - initPos);
}

void MPLSStreamInfo::parseStreamAttributes(BitStreamReader& reader)
{
    reader.skipBits(8);  // length
    stream_coding_type = static_cast<StreamType>(reader.getBits(8));
    if (isVideoStreamType(stream_coding_type))
    {
        video_format = reader.getBits<uint8_t>(4);
        frame_rate_index = reader.getBits<uint8_t>(4);
        reader.skipBits(24);  // reserved_for_future_use
    }
    else if (isAudioStreamType(stream_coding_type))
    {
        audio_presentation_type = reader.getBits<uint8_t>(4);
        reader.skipBits(4);  // sampling_frequency_index
        CLPIStreamInfo::readString(language_code, reader, 3);
    }
    else if (stream_coding_type == StreamType::SUB_PGS || stream_coding_type == StreamType::SUB_IGS)
    {
        CLPIStreamInfo::readString(language_code, reader, 3);
        reader.skipBits(8);  // reserved_for_future_use
    }
    else if (stream_coding_type == StreamType::SUB_TGS)
    {
        // Text subtitle stream
        character_code = reader.getBits<uint8_t>(8);
        CLPIStreamInfo::readString(language_code, reader, 3);
    }
}

void MPLSStreamInfo::composeStreamAttributes(BitStreamWriter& writer)
{
    uint8_t* lengthPos = writer.getBuffer() + writer.getBitsCount() / 8;
    writer.putBits(8, 0);  // length
    const int initPos = writer.getBitsCount() / 8;

    writer.putBits(8, static_cast<int>(stream_coding_type));
    if (isVideoStreamType(stream_coding_type))
    {
        writer.putBits(4, video_format);
        writer.putBits(4, frame_rate_index);
        if (HDR & 18)
            writer.putBits(8, 0x12);  // HDR10 or HDR10plus
        else if (HDR == 4)
            writer.putBits(8, 0x22);  // DV
        else
            writer.putBits(8, colour_space);  // SDR: 1 for BT.709 HEVC, 2 for BT.2020 HEVC, else 0
        if (HDR == 16)
            writer.putBits(8, 0x40);  // HDR10plus
        else
            writer.putBits(8, 0);
        writer.putBits(8, 0);  // reserved_for_future_use
    }
    else if (isAudioStreamType(stream_coding_type))
    {
        writer.putBits(4, audio_presentation_type);
        writer.putBits(4, sampling_frequency_index);
        CLPIStreamInfo::writeString(language_code, writer, 3);
    }
    else if (stream_coding_type == StreamType::SUB_PGS)
    {
        // Presentation Graphics stream
        CLPIStreamInfo::writeString(language_code, writer, 3);
        writer.putBits(8, 0);  // reserved_for_future_use
    }
    else
        THROW(ERR_COMMON, "Unsupported media type for AVCHD/Blu-ray muxing")
    *lengthPos = static_cast<uint8_t>(writer.getBitsCount() / 8 - initPos);
}
