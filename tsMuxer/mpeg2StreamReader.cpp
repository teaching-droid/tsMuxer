#include "mpeg2StreamReader.h"

#include <fs/systemlog.h>

#include "avPacket.h"
#include "nalUnits.h"
#include "tsPacket.h"
#include "vodCoreException.h"
#include "vod_common.h"

int MPEG2StreamReader::getTSDescriptor(uint8_t* dstBuff, bool blurayMode, bool hdmvDescriptors)
{
    try
    {
        for (uint8_t* nal = MPEGHeader::findNextMarker(m_buffer, m_bufEnd); nal <= m_bufEnd - 32;
             nal = MPEGHeader::findNextMarker(nal + 4, m_bufEnd))
        {
            if (nal[3] == SEQ_START_SHORT_CODE)
            {
                const uint8_t* nextNal = MPEGHeader::findNextMarker(nal + 4, m_bufEnd);
                m_sequence.deserialize(nal + 4, nextNal - nal - 4);
                m_streamAR = static_cast<VideoAspectRatio>(m_sequence.aspect_ratio_info);
            }
            else if (nal[3] == EXT_START_SHORT_CODE)
            {
                BitStreamReader bitReader{};
                bitReader.setBuffer(nal + 4, m_bufEnd);
                const auto extType = bitReader.getBits<uint8_t>(4);
                if (extType == SEQUENCE_EXT)
                {
                    m_sequence.deserializeExtension(bitReader);
                }
                else if (extType == SEQUENCE_DISPLAY_EXT)
                {
                    m_sequence.deserializeDisplayExtension(bitReader);
                }
            }
            else if (nal[3] == GOP_START_SHORT_CODE)
            {
                const uint8_t* nextNal = MPEGHeader::findNextMarker(nal + 4, m_bufEnd);
                m_gop.deserialize(nal + 4, nextNal - nal - 4);
            }
        }
    }
    catch (BitStreamException&)
    {
    }

    if (m_ar != VideoAspectRatio::AR_KEEP_DEFAULT)
    {
        m_sequence.aspect_ratio_info = static_cast<uint8_t>(m_ar);
        m_streamAR = m_ar;
    }

    // Without HDMV descriptors (--no-hdmv-descriptors), the video stream descriptor of Rec. ITU-T H.222.0 (2.6.2,
    // Table 2-46), as H.264 and HEVC write their own standard descriptor then. The HDMV descriptor was written all the
    // same, so the option did nothing for MPEG-2 video.
    if (!hdmvDescriptors)
    {
        const bool mpeg1Only = m_sequence.profile < 0;  // no sequence extension: MPEG-1 video
        *dstBuff++ = static_cast<uint8_t>(TSDescriptorTag::VIDEO_STREAM);
        *dstBuff++ = mpeg1Only ? 1 : 3;
        // multiple_frame_rate_flag 0, frame_rate_code, MPEG_1_only_flag, constrained_parameter_flag (1 for MPEG-2
        // video, as the standard requires), still_picture_flag 0
        *dstBuff++ = static_cast<uint8_t>((m_sequence.frame_rate_index & 0x0f) << 3 | (mpeg1Only ? 1 : 0) << 2 |
                                          (mpeg1Only ? (m_sequence.constParameterFlag ? 1 : 0) : 1) << 1);
        if (mpeg1Only)
            return 3;
        *dstBuff++ = static_cast<uint8_t>((m_sequence.profile_escape ? 0x80 : 0) | (m_sequence.profile & 0x07) << 4 |
                                          (m_sequence.level & 0x0f));
        const bool rateExtension = m_sequence.frame_rate_ext.num != 1 || m_sequence.frame_rate_ext.den != 1;
        *dstBuff = static_cast<uint8_t>((m_sequence.chroma_format & 0x03) << 6 | (rateExtension ? 1 : 0) << 5 | 0x1f);
        return 5;
    }

    // HDMV registration descriptor
    *dstBuff++ = static_cast<uint8_t>(TSDescriptorTag::HDMV);  // registration descriptor tag
    *dstBuff++ = 8;                                            // descriptor length
    *dstBuff++ = 'H';
    *dstBuff++ = 'D';
    *dstBuff++ = 'M';
    *dstBuff++ = 'V';
    *dstBuff++ = 0xff;

    *dstBuff++ = static_cast<uint8_t>(StreamType::VIDEO_MPEG2);  // stream_coding_type
    // The video format here is the Blu-ray code for the picture size and scan (1 480i, 2 576i, 4 1080i, 6 1080p and so
    // on), the one the clip information file carries and H.264 and HEVC write here too. The video_format of the MPEG-2
    // display extension (PAL, NTSC and the like) was written instead, or 5 where that extension is missing, so a 1080
    // line stream was declared 720p while the clip information file said 1080. The frame rate and aspect ratio codes
    // of MPEG-2 are the ones Blu-ray uses.
    uint8_t video_format = 0, frame_rate_index = 0, aspect_ratio_index = 0;
    const double fps = getFPS() > 0 ? getFPS() : m_sequence.getFrameRate();
    M2TSStreamInfo::blurayStreamParams(fps, getInterlaced(), getStreamWidth(), getStreamHeight(), getStreamAR(),
                                       &video_format, &frame_rate_index, &aspect_ratio_index);
    *dstBuff++ = static_cast<uint8_t>(video_format << 4 | m_sequence.frame_rate_index);
    *dstBuff = static_cast<uint8_t>(m_sequence.aspect_ratio_info << 4 | 0xf);

    return 10;  // total descriptor length
}

void MPEG2StreamReader::applyDiscoveryData(const StreamDiscoveryData& data)
{
    if (data.fps > 0.0 && m_fps == 0.0)
        setFPS(data.fps);
}

CheckStreamRez MPEG2StreamReader::checkStream(uint8_t* buffer, int len)
{
    CheckStreamRez rez;
    uint8_t* end = buffer + len;
    BitStreamReader bitReader{};
    uint8_t* nextNal = nullptr;
    bool sliceFound = false;
    bool pictureFound = false;
    bool pulldownFound = false;
    uint8_t extType = 0;
    MPEGPictureHeader frame(0);
    for (uint8_t* nal = MPEGHeader::findNextMarker(buffer, end); nal <= end - 32;
         nal = MPEGHeader::findNextMarker(nal + 4, end))
    {
        uint8_t unitType = nal[3];
        try
        {
            if (unitType >= SLICE_MIN_START_SHORT_CODE && unitType <= SLICE_MAX_START_SHORT_CODE)
                sliceFound = true;
            else
                switch (unitType)
                {
                case EXT_START_SHORT_CODE:
                    bitReader.setBuffer(nal + 4, end);
                    extType = bitReader.getBits<uint8_t>(4);
                    if (extType == SEQUENCE_EXT)
                    {
                        m_sequence.deserializeExtension(bitReader);
                    }
                    else if (extType == PICTURE_CODING_EXT)
                    {
                        frame.deserializeCodingExtension(bitReader);
                        pulldownFound |= frame.repeat_first_field > 0;
                    }
                    break;
                case SEQ_END_SHORT_CODE:
                case GOP_START_SHORT_CODE:
                case USER_START_SHORT_CODE:
                    break;
                case PICTURE_START_SHORT_CODE:
                    if (frame.deserialize(nal + 4, end - nal - 4) == nullptr)
                        return rez;
                    pictureFound = true;
                    break;
                case SEQ_START_SHORT_CODE:
                    nextNal = MPEGHeader::findNextMarker(nal + 4, end);
                    if (m_sequence.deserialize(nal + 4, nextNal - nal - 4) == nullptr)
                        return rez;
                    m_streamAR = static_cast<VideoAspectRatio>(m_sequence.aspect_ratio_info);
                    spsFound = true;
                    break;
                default:
                    return rez;
                }
        }
        catch (BitStreamException&)
        {
        }
    }
    if (spsFound && pictureFound && sliceFound)
    {
        rez.codecInfo = mpeg2CodecInfo;
        rez.streamDescr = m_sequence.getStreamDescr();
        if (pulldownFound)
            rez.streamDescr += " (pulldown)";
    }
    return rez;
}

int MPEG2StreamReader::intDecodeNAL(uint8_t* buff)
{
    try
    {
        int rez;
        uint8_t* nextNal;
        switch (*buff)
        {
        case SEQ_START_SHORT_CODE:
            rez = processSeqStartCode(buff);
            if (rez != 0)
                return rez;
            nextNal = MPEGHeader::findNextMarker(buff, m_bufEnd) + 3;
            while (true)
            {
                if (nextNal >= m_bufEnd)
                    return NOT_ENOUGH_BUFFER;
                switch (*nextNal)
                {
                case EXT_START_SHORT_CODE:
                    rez = processExtStartCode(nextNal);
                    if (rez != 0)
                        return rez;
                    break;
                case GOP_START_SHORT_CODE:
                    m_framesAtGop = -1;
                    m_lastRef = -1;
                    break;
                case PICTURE_START_SHORT_CODE:
                    rez = decodePicture(nextNal);
                    if (rez == 0)
                    {
                        m_lastDecodedPos = nextNal;
                    }
                    return rez;
                default:;
                }
                nextNal = MPEGHeader::findNextMarker(nextNal, m_bufEnd) + 3;
            }
        case EXT_START_SHORT_CODE:
            return processExtStartCode(buff);
        case GOP_START_SHORT_CODE:
            m_framesAtGop = -1;
            m_lastRef = -1;
            break;
        case PICTURE_START_SHORT_CODE:
            rez = decodePicture(buff);
            return rez;
        default:;
        }
        return 0;
    }
    catch (BitStreamException&)
    {
        return NOT_ENOUGH_BUFFER;
    }
}

int MPEG2StreamReader::processSeqStartCode(uint8_t* buff)
{
    uint8_t* nextNal = MPEGHeader::findNextMarker(buff, m_bufEnd);
    if (nextNal == m_bufEnd)
    {
        return NOT_ENOUGH_BUFFER;
    }
    try
    {
        if (m_sequence.deserialize(buff + 1, nextNal - buff - 1) == nullptr)
            return NALUnit::UNSUPPORTED_PARAM;
        m_streamAR = static_cast<VideoAspectRatio>(m_sequence.aspect_ratio_info);
    }
    catch (BitStreamException&)
    {
        return NOT_ENOUGH_BUFFER;
    }
    constexpr int oldSpsLen = 0;
    updateFPS(nullptr, buff, nextNal, oldSpsLen);
    spsFound = true;
    m_lastIFrame = true;
    return 0;
}

int MPEG2StreamReader::processExtStartCode(uint8_t* buff)
{
    BitStreamReader bitReader{};
    try
    {
        bitReader.setBuffer(buff + 1, m_bufEnd);
        const auto extType = bitReader.getBits<uint8_t>(4);
        if (extType == SEQUENCE_EXT)
        {
            m_sequence.deserializeExtension(bitReader);
            m_seqExtFound = true;
        }
        return 0;
    }
    catch (BitStreamException&)
    {
        return NOT_ENOUGH_BUFFER;
    }
}

int MPEG2StreamReader::decodePicture(uint8_t* buff)
{
    if (!spsFound)
        return NALUnit::SPS_OR_PPS_NOT_READY;

    if (!m_streamMsgPrinted)
    {
        LTRACE(LT_INFO, 2, "Decoding MPEG2 stream (track " << m_streamIndex << "): " << m_sequence.getStreamDescr());
        m_streamMsgPrinted = true;
    }

    try
    {
        if (m_frame.deserialize(buff + 1, m_bufEnd - buff - 1) == nullptr)
            return NALUnit::UNSUPPORTED_PARAM;
    }
    catch (BitStreamException&)
    {
        return NOT_ENOUGH_BUFFER;
    }

    m_frame.picture_structure = 0;
    const int rez = findFrameExt(buff + 1);
    if (rez == NOT_ENOUGH_BUFFER)
        return rez;

    if (m_frame.pict_type == PictureCodingType::I_FRAME)
    {
        m_framesAtGop = -1;
        m_lastRef = -1;
    }

    if (m_frame.ref != m_lastRef)
    {
        m_framesAtGop++;
        m_lastRef = m_frame.ref;
        m_totalFrameNum++;

        m_curDts += m_prevFrameDelay;

        if (m_frame.repeat_first_field)
        {
            if (!m_sequence.progressive_sequence)
                m_prevFrameDelay = m_pcrIncPerFrame + m_pcrIncPerField;
            else if (m_frame.top_field_first)
                m_prevFrameDelay = m_pcrIncPerFrame * 3;
            else
                m_prevFrameDelay = m_pcrIncPerFrame * 2;
        }
        else
            m_prevFrameDelay = m_pcrIncPerFrame;

        if (m_removePulldown)
        {
            checkPulldownSync();
            m_testPulldownDts += m_prevFrameDelay;

            m_prevFrameDelay = m_pcrIncPerFrame;
        }
    }
    m_isFirstFrame = false;
    const int refDif = m_frame.ref - m_framesAtGop;
    m_curPts = m_curDts + refDif * m_pcrIncPerFrame;
    m_lastIFrame = m_frame.pict_type == PictureCodingType::I_FRAME;

    return 0;
}

int MPEG2StreamReader::findFrameExt(uint8_t* buffer)
{
    for (uint8_t* nal = MPEGHeader::findNextMarker(buffer, m_bufEnd); nal < m_bufEnd - 4;
         nal = MPEGHeader::findNextMarker(nal + 4, m_bufEnd))
    {
        if (nal[3] == EXT_START_SHORT_CODE)
        {
            BitStreamReader bitReader{};
            try
            {
                bitReader.setBuffer(nal + 4, m_bufEnd);
                const auto extType = bitReader.getBits<uint8_t>(4);
                if (extType == PICTURE_CODING_EXT)
                {
                    m_frame.deserializeCodingExtension(bitReader);

                    if (m_removePulldown)
                    {
                        updateBits(bitReader, m_frame.repeat_first_field_bitpos, 1, 0);
                        if (m_sequence.progressive_sequence)
                            updateBits(bitReader, m_frame.top_field_first_bitpos, 1, 0);
                    }
                    return 0;
                }
            }
            catch (BitStreamException&)
            {
                return NOT_ENOUGH_BUFFER;
            }
        }
        else
            return NALUnit::NOT_FOUND;
    }
    return NOT_ENOUGH_BUFFER;
}

void MPEG2StreamReader::updateStreamFps(void* nalUnit, uint8_t* buff, uint8_t* nextNal, int oldSpsLen)
{
    m_sequence.setFrameRate(buff + 1, m_fps);
}

void MPEG2StreamReader::updateStreamAR(void* nalUnit, uint8_t* buff, uint8_t* nextNal, int oldSpsLen)
{
    if (m_ar != VideoAspectRatio::AR_KEEP_DEFAULT)
    {
        MPEGSequenceHeader::setAspectRatio(buff + 1, m_ar);
        m_sequence.aspect_ratio_info = static_cast<uint8_t>(m_ar);
        m_streamAR = m_ar;
    }
}
