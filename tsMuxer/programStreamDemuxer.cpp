// Changed by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#include "programStreamDemuxer.h"

#include <fs/systemlog.h>

#include "dvdTitles.h"
#include "encryptedInput.h"
#include "mpegVideo.h"
#include "pesPacket.h"
#include "tsPacket.h"
#include "wave.h"

using namespace std;

static constexpr int LPCM_FREQS[4] = {48000, 96000, 44100, 32000};

// #define min(a,b) a<=b?a:b

// A program stream decoder also reads ISO/IEC 11172-1 (MPEG-1) system streams (H.222.0, 2.8), whose packets have a
// header of their own: up to 16 stuffing bytes 0xFF, the STD buffer size ('01' and 14 bits), then a PTS ('0010'), a PTS
// and a DTS ('0011'), each coded as in a PES header, or the byte 0x0F when there is no time stamp. A PES header of
// H.222.0 starts with '10' instead. Every packet was read as a PES header, so the payload was taken from a place that
// is not where it starts, and a short packet at the end stopped the read with "Invalid copyLen".
// Returns the length of an MPEG-1 packet header, with its PTS (-1 without one); 0 when the packet has none, and -1 when
// the header goes on past end.
static int mpeg1PacketHeader(const uint8_t* buff, const uint8_t* end, int64_t& pts)
{
    pts = -1;
    if (buff[3] == PES_PRIVATE_DATA2 || (buff[6] & 0xc0) == 0x80)
        return 0;
    const int64_t avail = end - buff;
    const int packetEnd = 6 + (buff[4] << 8 | buff[5]);
    int pos = 6;
    // A field of n bytes at pos: 1 when it is there, 0 when it would end past the packet, -1 past the data read so far
    const auto has = [&](const int n) { return pos + n > packetEnd ? 0 : pos + n > avail ? -1 : 1; };
    for (int stuffing = 0;; ++stuffing)
    {
        if (const int h = has(1); h < 1)
            return h;
        if (buff[pos] != 0xff)
            break;
        if (stuffing == 16)
            return 0;
        ++pos;
    }
    if ((buff[pos] & 0xc0) == 0x40)  // STD_buffer_scale and STD_buffer_size
    {
        pos += 2;
        if (const int h = has(1); h < 1)
            return h;
    }
    int fieldLen;
    if ((buff[pos] & 0xf0) == 0x20)
        fieldLen = 5;  // PTS
    else if ((buff[pos] & 0xf0) == 0x30)
        fieldLen = 10;  // PTS and DTS
    else if (buff[pos] == 0x0f)
        fieldLen = 1;
    else
        return 0;
    if (const int h = has(fieldLen); h < 1)
        return h;
    if (fieldLen > 1)
        pts = get_pts(buff + pos);
    return pos + fieldLen;
}

// The PTS of the packet at buff, for the duration: -1 when it has none, or its header is cut by end.
static int64_t packetPts(uint8_t* buff, const uint8_t* end)
{
    int64_t pts;
    const int mpeg1Len = mpeg1PacketHeader(buff, end, pts);
    if (mpeg1Len != 0)
        return pts;
    const auto pesPacket = reinterpret_cast<PESPacket*>(buff);
    return (pesPacket->flagsLo & 0x80) == 0x80 ? pesPacket->getPts() : -1;
}

ProgramStreamDemuxer::ProgramStreamDemuxer(const BufferedReaderManager& readManager)
    : m_tmpBuffer{}, m_readManager(readManager), m_dataProcessed(0)
{
    memset(m_psm_es_type, 0, sizeof(m_psm_es_type));
    memset(m_lpcpHeaderAdded, 0, sizeof(m_lpcpHeaderAdded));
    m_bufferedReader = m_readManager.getReader(m_streamName.c_str());
    m_readerID = m_bufferedReader->createReader(MAX_PES_HEADER_SIZE);
    m_lastReadRez = 0;
    m_lastPesLen = 0;
    m_lastPID = 0;
    m_tmpBufferLen = 0;
    m_firstPTS = -1;
    m_firstVideoPTS = -1;
}

void ProgramStreamDemuxer::setFileIterator(FileNameIterator* itr)
{
    const auto br = dynamic_cast<BufferedReader*>(m_bufferedReader);
    if (br)
        br->setFileIterator(itr, m_readerID);
    else if (itr != nullptr)
        THROW(ERR_COMMON, "Can not set file iterator. Reader does not support bufferedReader interface.")
}

ProgramStreamDemuxer::~ProgramStreamDemuxer() { m_bufferedReader->deleteReader(m_readerID); }

void ProgramStreamDemuxer::readClose() {}

int64_t ProgramStreamDemuxer::getDemuxedSize() { return m_dataProcessed; }

void ProgramStreamDemuxer::openFile(const std::string& streamName)
{
    m_streamName = streamName;
    readClose();
    // BufferedFileReader* fileReader = dynamic_cast <BufferedFileReader*> (m_bufferedReader);
    if (!m_bufferedReader->openStream(m_readerID, m_streamName.c_str()))
        THROW(ERR_FILE_NOT_FOUND, "Can't open stream " << m_streamName)
    checkEncryptedInput(m_streamName, false);  // a VOB still encrypted (encryptedInput.h)
    m_dataProcessed = 0;
}

int ProgramStreamDemuxer::mpegps_psm_parse(const uint8_t* buff, const uint8_t* end)
{
    if (end - buff < 7)
        return -1;
    const uint8_t* curBuff = buff + 4;
    // int map_stream_id = *curBuff++;
    const int psm_length = (*curBuff << 8) + curBuff[1];
    // A map cut by the end of a read is kept, with its 6-byte header, in m_tmpBuffer until the rest arrives, so the
    // whole map has to fit there. The limit used to be the room itself, without the header, and the kept part of a
    // map at the limit was then up to 6 bytes longer than the room.
    if (psm_length > MAX_PES_HEADER_SIZE - 6)
    {
        THROW(ERR_COMMON, "Can't parse Program Stream Map. Too large size " << psm_length << ". Max allowed size "
                                                                            << MAX_PES_HEADER_SIZE - 6 << " bytes.")
    }
    if (end - buff < psm_length + 7)
        return -1;

    curBuff += 4;
    const int ps_info_length = (*curBuff << 8) + curBuff[1];
    curBuff += ps_info_length + 2;
    int es_map_length = (*curBuff << 8) + curBuff[1];
    curBuff += 2;
    /* at least one es available? */
    while (es_map_length >= 4)
    {
        const unsigned char type = *curBuff++;
        const unsigned char es_id = *curBuff++;
        /* remember mapping from stream id to stream type */
        m_psm_es_type[es_id] = type;
        /* skip program_stream_info */
        const int es_info_length = *curBuff << 8 | curBuff[1];
        curBuff += 2;
        curBuff += es_info_length;
        es_map_length -= 4 + es_info_length;
    }
    return 6 + psm_length;
}

int ProgramStreamDemuxer::processPES(uint8_t* buff, uint8_t* end, const int headerLen, const bool mpeg1,
                                     int& afterPesHeader)
{
    afterPesHeader = 0;

    const auto pesPacket = reinterpret_cast<PESPacket*>(buff);
    int startcode = buff[3];

    // find matching stream
    if (!((startcode >= 0xc0 && startcode <= 0xef) /* audio or video */ || (startcode == PES_PRIVATE_DATA1) ||
          (startcode == PES_VC1_ID)))
        return 0;

    uint8_t* curBuf = buff + 9;
    if ((pesPacket->flagsLo & 0xc0) == 0xc0)  // PTS and DTS
        curBuf += 10;
    else if ((pesPacket->flagsLo & 0xc0) == 0x80)  // PTS only
        curBuf += 5;
    if (pesPacket->flagsLo & 0x20)  // ESCR_flag
        curBuf += 6;
    if (pesPacket->flagsLo & 0x10)  // ES_rate_flag
        curBuf += 3;
    if (pesPacket->flagsLo & 0x08)  // DSM_trick_mode_flag
        curBuf++;
    if (pesPacket->flagsLo & 0x04)  // additional_copy_info_flag
        curBuf++;
    if (pesPacket->flagsLo & 0x02)  // PES_CRC_flag
        curBuf += 2;
    if (!mpeg1 && (pesPacket->flagsLo & 0x01))  // PES_extension_flag, which an MPEG-1 packet header has not
    {
        const uint8_t extFlag = *curBuf++;
        if (extFlag & 0x80)  // PES_private_data_flag
            curBuf += 16;
        if (extFlag & 0x40)  // pack_header_field_flag
        {
            const int pack_field_length = *curBuf++;
            curBuf += pack_field_length;
        }
        if (extFlag & 0x20)  // program_packet_sequence_counter_flag
            curBuf += 2;
        if (extFlag & 0x10)  // P-STD_buffer_flag
            curBuf += 2;
        if (extFlag & 0x01)  // PES_extension_flag_2
        {
            const int ext2_len = *curBuf++ & 0x7f;  // PES_extension_field_length
            if (ext2_len > 0)
                startcode = startcode << 8 | *curBuf;
        }
    }
    curBuf = buff + headerLen;
    if (startcode == PES_PRIVATE_DATA1 && m_psm_es_type[startcode] == 0)
    {
        afterPesHeader++;
        startcode = *curBuf++;
        if (startcode >= 0xb0 && startcode <= 0xbf)
        {  // MLP/TrueHD audio has a 4-byte header, keep header
        }
        else if (startcode >= 0xa0 && startcode <= 0xaf)
        {  // PCM

            MemoryBlock& waveHeader = m_lpcmWaveHeader[startcode - 0xa0];

            const uint16_t bitdepth = 16 + (curBuf[4] >> 6 & 3) * 4;

            if (waveHeader.isEmpty())
            {
                const int samplerate = LPCM_FREQS[curBuf[4] >> 4 & 3];
                const uint16_t channels = 1 + (curBuf[4] & 7);
                wave_format::buildWaveHeader(waveHeader, samplerate, channels, channels >= 6, bitdepth);
            }

            afterPesHeader += 6;

            uint8_t* payloadData = curBuf + afterPesHeader - 1;
            const int pesPayloadLen = pesPacket->getPacketLength() - headerLen - afterPesHeader;
            wave_format::toLittleEndian(payloadData, payloadData, pesPayloadLen, bitdepth);
        }
        else if (startcode >= 0x80 && startcode <= 0xcf)
        {
            // audio: skip header
            afterPesHeader += 3;
        }
    }

    return startcode;
}

void ProgramStreamDemuxer::getTrackList(std::map<int32_t, TrackInfo>& trackList)
{
    for (int i = 0x20; i < 0xff; i++)
    {
        if (i >= 0xa0 && i <= 0xaf)
            trackList[i] = TrackInfo(static_cast<int>(StreamType::AUDIO_LPCM), "", 0);  // set track hint
        else
            trackList[i] = TrackInfo(0, "", 0);  // autodetect
        // Sub-streams 0x20 to 0x3F of private stream 1 are DVD subpictures, read as PGS (addSubPicture)
        if (i >= 0x20 && i <= 0x3f)
            trackList[i] = TrackInfo(static_cast<int>(StreamType::SUB_PGS), "", 0);
    }
}

bool ProgramStreamDemuxer::isVideoPID(const uint32_t pid) const
{
    return (pid >= 0x55 && pid <= 0x5f) ||  // vc1
           (pid >= 0xe0 && pid <= 0xef) ||  // mpeg video
           m_psm_es_type[pid & 0xff] == static_cast<int>(StreamType::VIDEO_H264) ||
           m_psm_es_type[pid & 0xff] == static_cast<int>(StreamType::VIDEO_MVC) ||
           m_psm_es_type[pid & 0xff] == static_cast<int>(StreamType::VIDEO_H265) ||
           m_psm_es_type[pid & 0xff] == static_cast<int>(StreamType::VIDEO_H266);
}

int ProgramStreamDemuxer::simpleDemuxBlock(DemuxedData& demuxedData, const PIDSet& acceptedPIDs, int64_t& discardSize)
{
    discardSize = 0;
    uint32_t readedBytes = 0;
    int readRez = 0;
    uint8_t* data = m_bufferedReader->readBlock(m_readerID, readedBytes, readRez);  // blocked read mode
    if (readRez == BufferedFileReader::DATA_NOT_READY)
    {
        m_lastReadRez = readRez;
        return BufferedFileReader::DATA_NOT_READY;
    }
    if (readedBytes + m_tmpBufferLen == 0 || (readedBytes == 0 && m_lastReadRez == BufferedReader::DATA_EOF))
    {
        m_lastReadRez = readRez;
        flushSubPictures(demuxedData, acceptedPIDs);  // the end of the file came with no data
        return BufferedReader::DATA_EOF;
    }
    if (readedBytes > 0)
        m_bufferedReader->notify(m_readerID, readedBytes);

    m_lastReadRez = readRez;
    data += MAX_PES_HEADER_SIZE;
    assert(m_tmpBufferLen <= MAX_PES_HEADER_SIZE);
    if (m_tmpBufferLen > 0)
    {
        memcpy(data - m_tmpBufferLen, m_tmpBuffer, m_tmpBufferLen);
        data -= m_tmpBufferLen;
        readedBytes += m_tmpBufferLen;
        m_tmpBufferLen = 0;
    }

    uint8_t* end = data + readedBytes;
    uint8_t* curBuf = data;

    if (m_lastPesLen > 0)
    {
        const int copyLen = FFMIN(readedBytes, m_lastPesLen);
        if (m_lastPID >= 0x20 && m_lastPID <= 0x3f)
        {
            addSubPicture(m_lastPID, curBuf, copyLen, -1, demuxedData[m_lastPID]);
            m_dataProcessed += copyLen;
        }
        else if (m_lastPID > 0)
        {
            StreamData& vect = demuxedData[m_lastPID];
            vect.append(curBuf, copyLen);
            m_dataProcessed += copyLen;
        }
        else
            discardSize += copyLen;
        if (m_lastPesVideo)
            scanVideo(curBuf, copyLen);
        curBuf += copyLen;
        m_lastPesLen -= copyLen;
        if (m_lastPesLen > 0)
        {
            if (readedBytes > 0)
                return 0;
            return BufferedReader::DATA_EOF;
        }
    }

    const uint8_t* prevBuf = curBuf;
    curBuf = MPEGHeader::findNextMarker(curBuf, end);
    discardSize += curBuf - prevBuf;

    while (curBuf <= end - 9)
    {
        const auto pesPacket = reinterpret_cast<PESPacket*>(curBuf);
        int startcode = curBuf[3];
        if ((startcode >= 0xc0 && startcode <= 0xef) || (startcode == PES_PRIVATE_DATA1) || (startcode == PES_VC1_ID) ||
            (startcode == PES_PRIVATE_DATA2))
        {
            int64_t mpeg1Pts;
            const int mpeg1HeaderLen = mpeg1PacketHeader(curBuf, end, mpeg1Pts);
            const int headerLen = mpeg1HeaderLen > 0 ? mpeg1HeaderLen : pesPacket->getHeaderLength();
            if (mpeg1HeaderLen < 0 || curBuf + headerLen + 4 > end)
                break;
            int afterPesHeader = 0;
            startcode = processPES(curBuf, end, headerLen, mpeg1HeaderLen > 0, afterPesHeader);
            const bool hasPts = mpeg1HeaderLen > 0 ? mpeg1Pts >= 0 : (pesPacket->flagsLo & 0x80) == 0x80;
            m_lastPesVideo = false;
            if (startcode == 0xe0)  // the video of a DVD: the time line of its subtitles
            {
                m_pesPts = -1;
                if (hasPts)
                {
                    const int64_t pts = mpeg1HeaderLen > 0 ? mpeg1Pts : pesPacket->getPts();
                    noteVideoTime(
                        pts, mpeg1HeaderLen == 0 && (pesPacket->flagsLo & 0xc0) == 0xc0 ? pesPacket->getDts() : pts);
                }
                const uint8_t* p = curBuf + headerLen + afterPesHeader;
                const int len = pesPacket->getPacketLength() - headerLen - afterPesHeader;
                const int have = static_cast<int>((std::min)(static_cast<int64_t>(len), static_cast<int64_t>(end - p)));
                if (have > 0)
                    scanVideo(p, have);
                m_lastPesVideo = have < len;
            }
            const bool subPicture = startcode >= 0x20 && startcode <= 0x3f;
            if (subPicture && acceptedPIDs.find(startcode) != acceptedPIDs.end())
            {
                const uint8_t* payloadData = curBuf + headerLen + afterPesHeader;
                const int pesPayloadLen = pesPacket->getPacketLength() - headerLen - afterPesHeader;
                const int copyLen = FFMIN(pesPayloadLen, (int)(end - payloadData));
                if (copyLen < 0)
                    THROW(ERR_COMMON, "Invalid copyLen")
                addSubPicture(startcode, payloadData, copyLen,
                              !hasPts              ? -1
                              : mpeg1HeaderLen > 0 ? mpeg1Pts
                                                   : pesPacket->getPts(),
                              demuxedData[startcode]);
                m_dataProcessed += copyLen;
                discardSize += payloadData - curBuf;
                if (copyLen < pesPayloadLen)
                {
                    m_lastPesLen = pesPayloadLen - copyLen;
                    m_lastPID = startcode;
                    return 0;
                }
                curBuf += pesPacket->getPacketLength();
            }
            else if (acceptedPIDs.find(startcode) != acceptedPIDs.end())
            {
                if (hasPts)
                {
                    const int64_t curPts = mpeg1HeaderLen > 0 ? mpeg1Pts : pesPacket->getPts();
                    if (m_firstPTS == -1 || curPts < m_firstPTS)
                        m_firstPTS = curPts;
                    if (isVideoPID(startcode) && (m_firstVideoPTS == -1 || curPts < m_firstVideoPTS))
                        m_firstVideoPTS = curPts;
                    if (m_firstPtsTime.find(startcode) == m_firstPtsTime.end() || curPts < m_firstPtsTime[startcode])
                        m_firstPtsTime[startcode] = curPts;
                }

                StreamData& vect = demuxedData[startcode];

                const int idx = startcode - 0xa0;
                if (idx >= 0 && idx <= 15 && !m_lpcpHeaderAdded[idx])
                {
                    vect.append(m_lpcmWaveHeader[idx].data(), m_lpcmWaveHeader[idx].size());
                    m_lpcpHeaderAdded[idx] = true;
                }

                const uint8_t* payloadData = curBuf + headerLen + afterPesHeader;
                const int pesPayloadLen = pesPacket->getPacketLength() - headerLen - afterPesHeader;
                const int copyLen = FFMIN(pesPayloadLen, (int)(end - payloadData));
                if (copyLen < 0)
                {
                    THROW(ERR_COMMON, "Invalid copyLen")
                }
                vect.append(payloadData, copyLen);
                m_dataProcessed += copyLen;
                discardSize += payloadData - curBuf;
                if (copyLen < pesPayloadLen)
                {
                    m_lastPesLen = pesPayloadLen - copyLen;
                    m_lastPID = startcode;
                    return 0;
                }
                curBuf += pesPacket->getPacketLength();
            }
            else
            {
                const int tmpLen = pesPacket->getPacketLength();
                if (tmpLen > end - curBuf)
                {
                    discardSize += end - curBuf;
                    m_lastPID = 0;
                    m_lastPesLen = tmpLen - static_cast<int>(end - curBuf);
                    return 0;
                }
                curBuf += tmpLen;
                discardSize += tmpLen;
            }
        }
        else if (startcode == PES_PROGRAM_STREAM_MAP)
        {
            const int psmLen = mpegps_psm_parse(curBuf, end);
            if (psmLen == -1)
                break;
            if (psmLen > end - curBuf)
            {
                discardSize += end - curBuf;
                m_lastPID = 0;
                m_lastPesLen = psmLen - static_cast<int>(end - curBuf);
                return 0;
            }
            discardSize += psmLen;
            curBuf += psmLen;
        }
        else
        {
            const int64_t rest = FFMIN(end - curBuf, 4);
            curBuf += rest;
            discardSize += rest;
        }
        prevBuf = curBuf;
        curBuf = MPEGHeader::findNextMarker(curBuf, end);
        discardSize += curBuf - prevBuf;
    }
    // What is left is a header cut by the end of the read; it is kept until the rest arrives. Nothing longer than the
    // room for it can be kept, and nothing that fits a program stream is.
    if (end - curBuf > MAX_PES_HEADER_SIZE)
        THROW(ERR_COMMON, "Can't parse the program stream: a header cut by the end of a read is "
                              << end - curBuf << " bytes long, more than the " << MAX_PES_HEADER_SIZE
                              << " bytes kept for one.")
    m_tmpBufferLen = static_cast<uint32_t>(end - curBuf);
    if (m_tmpBufferLen > 0)
        memmove(m_tmpBuffer, curBuf, end - curBuf);
    if (readRez == BufferedReader::DATA_EOF)
        flushSubPictures(demuxedData, acceptedPIDs);
    return 0;
}

void ProgramStreamDemuxer::noteVideoTime(const int64_t pts, const int64_t dts)
{
    // The decoding time of a picture only goes forward within a part, its presentation time does not: a picture that
    // is decoded no later than the one before, or a second after it, begins a new part, the next file of a join or
    // the clock of the VOB starting again (a title of the disc parted at a break of its clock, dvdTitleSource). The
    // part before is as long as the fields its pictures show.
    if (m_lastDts >= 0 && (dts <= m_lastDts || dts > m_lastDts + 90000))
    {
        const double length = static_cast<double>(m_segFields) * m_frameTicks / 2;
        LTRACE(LT_DEBUG, 0,
               "VOB " << m_streamName << " (or a file joined to it): the clock starts again at decoding time " << dts
                      << " after " << m_lastDts << "; the part before began at " << m_segFirst << " and showed "
                      << m_segFields << " fields, the next one starts at " << llround(m_segBase + length)
                      << " on the time line (90 kHz)");
        m_segBase += length;
        m_segFirst = -1;
        m_segFields = 0;
    }
    m_lastDts = dts;
    m_pesPts = pts;
}

// The start codes of the video (ISO/IEC 13818-2 6.2): the bytes after each are collected, across packets, for
// videoStartCode
void ProgramStreamDemuxer::scanVideo(const uint8_t* data, const int len)
{
    for (int i = 0; i < len; ++i)
    {
        const uint8_t b = data[i];
        if (m_scanWant > 0)
        {
            m_scanBytes[m_scanHave++] = b;
            if (m_scanHave == m_scanWant)
            {
                m_scanWant = 0;
                videoStartCode(m_scanCode, m_scanBytes);
            }
        }
        m_scanShift = m_scanShift << 8 | b;
        if ((m_scanShift & 0xffffff00) == 0x00000100)
        {
            m_scanCode = b;
            m_scanHave = 0;
            m_scanWant = b == 0x00 ? 2 : b == 0xb5 ? 4 : b == 0xb3 ? 4 : 0;
            if (b == 0x00)
                m_segFields += 2;  // a frame picture, or MPEG-1; corrected by its coding extension
        }
    }
}

void ProgramStreamDemuxer::videoStartCode(const uint8_t code, const uint8_t* b)
{
    if (code == 0xb3)
    {
        // sequence header: the picture height and the frame rate (Table 6-4)
        static constexpr double FRAME_TICKS[9] = {0, 3753.75, 3750, 3600, 3003, 3000, 1800, 1501.5, 1500};
        m_videoHeight = (b[1] & 0x0f) << 8 | b[2];
        if (const int rate = b[3] & 0x0f; rate >= 1 && rate <= 8)
            m_frameTicks = FRAME_TICKS[rate];
    }
    else if (code == 0xb5 && b[0] >> 4 == 1)
        m_progressiveSequence = (b[1] >> 3 & 1) != 0;  // sequence extension
    else if (code == 0xb5 && b[0] >> 4 == 8)
    {
        // picture coding extension: a field picture shows one field, a frame picture two and repeat_first_field one
        // more; in a progressive sequence repeat_first_field doubles or (with top_field_first) triples the frame
        const int structure = b[2] & 3;
        const bool repeat = (b[3] >> 1 & 1) != 0, topFirst = (b[3] >> 7 & 1) != 0;
        int fields = 2;
        if (structure != 3)
            fields = 1;
        else if (repeat)
            fields = m_progressiveSequence ? (topFirst ? 6 : 4) : 3;
        m_segFields += fields - 2;
    }
    else if (code == 0x00 && m_pesPts >= 0)
    {
        // the first picture that starts in a PES with a time: temporal_reference pictures are shown before it in its
        // group of pictures, so the part begins that much earlier
        const int temporalReference = b[0] << 2 | b[1] >> 6;
        const auto shown = static_cast<int64_t>(static_cast<double>(m_pesPts) - temporalReference * m_frameTicks);
        if (m_segFirst < 0 || shown < m_segFirst)
            m_segFirst = shown;
        m_pesPts = -1;
    }
}

void ProgramStreamDemuxer::addSubPicture(const int stream, const uint8_t* data, const int len, const int64_t pts,
                                         StreamData& out)
{
    SubPicture& sp = m_subPictures[stream];
    if (!sp.decoder)
    {
        // Without an IFO file (said where the track is muxed, METADemuxer) grey, at the size of the video
        std::string idx = dvdSubtitleIdx(m_streamName);
        if (idx.empty())
            idx = "size: 720x" + std::to_string(m_videoHeight > 0 ? m_videoHeight : 480) +
                  "\npalette: 000000, ffffff, 808080, 404040, 000000, ffffff, 808080, 404040, 000000, ffffff, "
                  "808080, 404040, 000000, ffffff, 808080, 404040\n";
        sp.decoder = std::make_unique<VobSubDecoder>(reinterpret_cast<const uint8_t*>(idx.data()),
                                                     static_cast<int>(idx.size()), 0.0);
    }
    // A packet with a time begins a unit; one without goes on with the unit begun. A unit not whole when the next
    // begins is damaged, and one whose start is lost is passed over.
    if (pts >= 0)
    {
        sp.unit.clear();
        sp.pts = pts;
    }
    else if (sp.unit.empty() && sp.pts < 0)
        return;
    sp.unit.insert(sp.unit.end(), data, data + len);
    if (sp.unit.size() < 2)
        return;
    const size_t size = static_cast<size_t>(sp.unit[0]) << 8 | sp.unit[1];
    if (sp.unit.size() < size)
        return;
    // on the time line of the video: where its part begins, and the time from the first picture of the part
    const int64_t first = m_segFirst >= 0 ? m_segFirst : sp.pts;
    const int64_t at = llround(m_segBase) + (sp.pts - first);
    std::vector<uint8_t> pgs;
    if (size >= 4)
        sp.decoder->decode(sp.unit.data(), static_cast<int>(size), at * (INTERNAL_PTS_FREQ / 90000), -1, pgs);
    out.append(pgs.data(), static_cast<int>(pgs.size()));
    sp.unit.clear();
    sp.pts = -1;
}

// The end of the last picture of each track, held until the next (BitmapToPgs::finish)
void ProgramStreamDemuxer::flushSubPictures(DemuxedData& demuxedData, const PIDSet& acceptedPIDs)
{
    if (m_subFlushed)
        return;
    m_subFlushed = true;
    for (auto& [stream, sp] : m_subPictures)
    {
        if (!sp.decoder || acceptedPIDs.find(stream) == acceptedPIDs.end())
            continue;
        std::vector<uint8_t> pgs;
        sp.decoder->finish(pgs);
        demuxedData[stream].append(pgs.data(), static_cast<int>(pgs.size()));
    }
}

int64_t getLastPCR(const File& file, const int bufferSize, const int64_t fileSize)
{
    file.seek(FFMAX(0, fileSize - bufferSize), File::SeekMethod::smBegin);
    const auto tmpBuffer = new uint8_t[bufferSize];
    const int len = file.read(tmpBuffer, bufferSize);
    if (len < 1)
        return -2;
    uint8_t* curPtr = tmpBuffer;
    uint8_t* bufEnd = tmpBuffer + len;
    int64_t lastPcrVal = -1;

    curPtr = MPEGHeader::findNextMarker(curPtr, bufEnd);
    while (curPtr <= bufEnd - 9 - 8)
    {
        const uint8_t startcode = curPtr[3];
        if ((startcode >= 0xc0 && startcode <= 0xef) || (startcode == PES_PRIVATE_DATA1) || (startcode == PES_VC1_ID) ||
            (startcode == PES_PRIVATE_DATA2))
        {
            if (const int64_t pts = packetPts(curPtr, bufEnd); pts >= 0)
                lastPcrVal = pts;
        }
        curPtr = MPEGHeader::findNextMarker(curPtr + 4, bufEnd);
    }
    delete[] tmpBuffer;
    return lastPcrVal;
}

int64_t getPSDuration(const char* fileName)
{
    constexpr int BUF_SIZE = 1024 * 256;

    try
    {
        int64_t fileSize;
        File file(fileName, File::ofRead);
        if (!file.size(&fileSize))
            return 0;

        const auto tmpBuffer = new uint8_t[BUF_SIZE];

        // pcr from start of file
        const int len = file.read(tmpBuffer, BUF_SIZE);
        if (len < 1)
        {
            delete[] tmpBuffer;
            return 0;
        }
        uint8_t* curPtr = tmpBuffer;
        int64_t firstPcrVal = 0;
        uint8_t* bufEnd = tmpBuffer + len;
        curPtr = MPEGHeader::findNextMarker(curPtr, bufEnd);
        while (curPtr <= bufEnd - 9)
        {
            const uint8_t startcode = curPtr[3];
            if ((startcode >= 0xc0 && startcode <= 0xef) || (startcode == PES_PRIVATE_DATA1) ||
                (startcode == PES_VC1_ID) || (startcode == PES_PRIVATE_DATA2))
            {
                if (const int64_t pts = packetPts(curPtr, bufEnd); pts >= 0)
                {
                    firstPcrVal = pts;
                    break;
                }
            }
            curPtr = MPEGHeader::findNextMarker(curPtr + 4, bufEnd);
        }
        delete[] tmpBuffer;

        // pcr from end of file

        int64_t lastPcrVal;
        int bufferSize = BUF_SIZE;
        do
        {
            lastPcrVal = getLastPCR(file, bufferSize, fileSize);
            bufferSize *= 4;
        } while (lastPcrVal == -1 && bufferSize <= 1024 * 1024);

        file.close();

        return lastPcrVal >= 0 ? lastPcrVal - firstPcrVal : 0;
    }
    catch (...)
    {
        return 0;
    }
}

int64_t ProgramStreamDemuxer::getFileDurationNano() const
{
    return getPSDuration(m_streamName.c_str()) * 1000000000ll / 90000ll;
}
