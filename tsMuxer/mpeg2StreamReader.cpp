// Largely rewritten by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#include "mpeg2StreamReader.h"

#include <fs/systemlog.h>

#include <cstring>

#include "avPacket.h"
#include "nalUnits.h"
#include "pictureEnd.h"
#include "tsMuxer.h"
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

    // frame_rate_code 0: the stream is muxed at the rate fps=, the times of its pictures or the default of 25 give, and
    // that code is written into its sequence headers as they are muxed (processSeqStartCode). The header read here
    // still has 0, and both descriptors gave rate 0 where no fps= was on the line, while the clip information file
    // and the stream itself gave the rate. Before updateFPS has run, getFPS() is still 0, so the rate the mux will
    // take is asked for, as H.264 does it.
    uint8_t rateCode = m_sequence.frame_rate_index;
    const double fps = headerFps(m_sequence.getFrameRate());
    if (m_sequence.getFrameRate() == 0.0)
        for (uint8_t i = 1; i < 9; i++)
            if (std::abs(frame_rates[i] - fps) < 3e-5)
                rateCode = i;

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
        *dstBuff++ = static_cast<uint8_t>((rateCode & 0x0f) << 3 | (mpeg1Only ? 1 : 0) << 2 |
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
    M2TSStreamInfo::blurayStreamParams(fps, getInterlaced(), getStreamWidth(), getStreamHeight(), getStreamAR(),
                                       &video_format, &frame_rate_index, &aspect_ratio_index);
    // The rate the stream is muxed at, as the clip information file gives it: with fps= or delPulldown the stream's own
    // code was written here, so the two disagreed (24 against 23.976, 29.97 against 23.976)
    *dstBuff++ = static_cast<uint8_t>(video_format << 4 | (frame_rate_index != 0 ? frame_rate_index : rateCode));
    *dstBuff = static_cast<uint8_t>(m_sequence.aspect_ratio_info << 4 | 0xf);

    return 10;  // total descriptor length
}

void MPEG2StreamReader::applyDiscoveryData(const StreamDiscoveryData& data)
{
    if (data.fps > 0.0 && m_fps == 0.0)
        setFPS(data.fps);
}

// ** ONE DAMAGED UNIT USED TO DECIDE THE WHOLE TRACK. ** checkStream looks at every start code in the up to 64 MiB
// it is given, and the first one of a type MPEG-2 video does not have, or the first picture or sequence header that
// did not parse, ended the check with "not detected". One TS packet in a hundred overwritten, as a weak broadcast
// signal does it, in the first 60 MB of a disc clip of MPEG-2 video: about 32,000 start codes, and a few of them, the
// ones whose type byte or header lies in a lost packet, are not MPEG-2: "Can't detect stream type", and the picture
// of the recording could not be listed or chosen at all, while a mux that named the codec read it.
//
// Such units are now counted instead, and the track is taken when they are few and the stream around them is MPEG-2
// video. Measured on that clip with 1, 5, 20 and 40 percent of its packets overwritten: 1, 4, 9 and 16 such start codes
// of 19,768 to 32,682 (at most 0.08 percent), 29 to 40 sequence headers and 272 to 465 pictures. Data that is not
// MPEG-2 video fails one of the three bounds: PCM has a quarter of its start codes out of place, and MP3 has 12,500
// start codes of which 0.03 to 0.08 percent are out of place but no sequence header or picture; other video has no
// sequence header at all (0xB3 is not a unit header of H.264 or HEVC). A track in which no unit is out of place is
// decided exactly as before, from a sequence header, a picture and a slice.
static constexpr int DAMAGED_SHARE_DIVISOR = 64;  // at most one start code in 64 out of place
static constexpr int DAMAGED_MIN_SEQUENCES = 2;   // a disc or a broadcast repeats it at every group of pictures
static constexpr int DAMAGED_MIN_PICTURES = 16;

CheckStreamRez MPEG2StreamReader::checkStream(uint8_t* buffer, int len)
{
    CheckStreamRez rez;
    uint8_t* end = buffer + len;
    BitStreamReader bitReader{};
    uint8_t* nextNal = nullptr;
    bool sliceFound = false;
    int pulldownPictures = 0;
    uint8_t extType = 0;
    MPEGPictureHeader frame(0);
    int units = 0;
    int damagedUnits = 0;
    int sequences = 0;
    int pictures = 0;
    // MPEG-1 video (ISO/IEC 11172-2) has no sequence extension, and MPEG-2 video has one right after every sequence
    // header (ISO/IEC 13818-2, 6.2.2: the start code after the header decides). isMpeg1() could not tell them apart
    // here, as only the mux sets m_seqExtFound, so every MPEG-2 stream was listed as "MPEG-1 video", a Main@8 720:576i
    // one of a broadcast or a disc as "720:576p". MPEG-1 now only where a sequence header of the probe is followed by
    // another unit and none by a sequence extension; a header at the end of the probe, its extension cut off, decides
    // nothing.
    bool afterSequence = false;  // the unit before was a sequence header
    bool sequenceExtended = false;
    bool sequenceAlone = false;
    for (uint8_t* nal = MPEGHeader::findNextMarker(buffer, end); nal <= end - 32;
         nal = MPEGHeader::findNextMarker(nal + 4, end))
    {
        uint8_t unitType = nal[3];
        ++units;
        if (afterSequence)
        {
            if (unitType == EXT_START_SHORT_CODE && (nal[4] >> 4) == SEQUENCE_EXT)
                sequenceExtended = true;
            else
                sequenceAlone = true;
            afterSequence = false;
        }
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
                        pulldownPictures += frame.repeat_first_field > 0 ? 1 : 0;
                    }
                    break;
                case SEQ_END_SHORT_CODE:
                case GOP_START_SHORT_CODE:
                case USER_START_SHORT_CODE:
                    break;
                case PICTURE_START_SHORT_CODE:
                    if (frame.deserialize(nal + 4, end - nal - 4) == nullptr)
                        ++damagedUnits;
                    else
                        ++pictures;
                    break;
                case SEQ_START_SHORT_CODE:
                {
                    // a damaged header is checked on a copy first, so that it does not overwrite the last good one,
                    // which the listing describes
                    nextNal = MPEGHeader::findNextMarker(nal + 4, end);
                    MPEGSequenceHeader check(0);
                    if (check.deserialize(nal + 4, nextNal - nal - 4) == nullptr)
                    {
                        ++damagedUnits;
                        break;
                    }
                    m_sequence.deserialize(nal + 4, nextNal - nal - 4);
                    m_streamAR = static_cast<VideoAspectRatio>(m_sequence.aspect_ratio_info);
                    spsFound = true;
                    afterSequence = true;
                    ++sequences;
                    break;
                }
                default:
                    ++damagedUnits;
                    break;
                }
        }
        catch (BitStreamException&)
        {
        }
    }
    const bool fewDamaged =
        damagedUnits == 0 || (static_cast<int64_t>(damagedUnits) * DAMAGED_SHARE_DIVISOR <= units &&
                              sequences >= DAMAGED_MIN_SEQUENCES && pictures >= DAMAGED_MIN_PICTURES);
    if (spsFound && pictures > 0 && sliceFound && fewDamaged)
    {
        rez.codecInfo = mpeg2CodecInfo;
        rez.streamDescr = m_sequence.getStreamDescr(sequenceAlone && !sequenceExtended);
        // A picture coding extension in a lost packet can say repeat_first_field by chance: with damage, pulldown is
        // only named where at least one picture in 16 repeats a field (3:2 pulldown repeats one in every two). Raw
        // MPEG-2 without pulldown, 40 percent of its blocks overwritten, was listed "(pulldown)" from one such picture.
        if (damagedUnits == 0 ? pulldownPictures > 0
                              : static_cast<int64_t>(pulldownPictures) * DAMAGED_MIN_PICTURES >= pictures)
            rez.streamDescr += " (pulldown)";
    }
    return rez;
}

// ** WHAT COMES BEFORE THE FIRST SEQUENCE HEADER IS LEFT OUT. ** A picture cannot be decoded without the sequence
// header before it, and decodePicture decodes none before the first one, but the bytes of those pictures went on into
// the output with the first picture after them: a stream cut inside a group of pictures, as a Matroska file cut without
// re-encoding begins, put the pictures left of that group ahead of its first sequence header, with the time of the
// first picture shown. The #47 test file so cut, with a P picture and two B pictures before the next group's I
// picture, began with those 3 pictures, 18909 bytes that no player can decode; with the headers of its first six
// groups taken out, with 75 pictures, 749764 bytes. The output now begins with the sequence header, and the sound
// stays in sync (0 ms, every picture matched by its bytes). Not where a header came before (spsFound), as in the next
// file of a join, whose first pictures follow the file before.
bool MPEG2StreamReader::skipNal(uint8_t* nal) { return !spsFound && *nal != SEQ_START_SHORT_CODE; }

int MPEG2StreamReader::intDecodeNAL(uint8_t* buff)
{
    if (skipNal(buff))
        return 0;  // left out by decodeNal
    try
    {
        int rez;
        uint8_t* nextNal;
        switch (*buff)
        {
        case SEQ_START_SHORT_CODE:
        {
            rez = processSeqStartCode(buff);
            if (rez != 0)
                return rez;
            nextNal = MPEGHeader::findNextMarker(buff, m_bufEnd) + 3;
            // The header as it goes out (frame rate and aspect ratio written in) and the sequence extensions after
            // it, without user data: what is repeated at an I picture that has no header of its own
            m_seqHeaderCopy.assign(buff - 3, nextNal - 3);
            bool seqLevel = true;  // still in the extensions and user data of the sequence header
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
                    if (seqLevel)
                    {
                        uint8_t* extEnd = MPEGHeader::findNextMarker(nextNal, m_bufEnd);
                        if (extEnd >= m_bufEnd)
                            return NOT_ENOUGH_BUFFER;
                        m_seqHeaderCopy.insert(m_seqHeaderCopy.end(), nextNal - 3, extEnd);
                    }
                    break;
                case USER_START_SHORT_CODE:
                    break;
                case GOP_START_SHORT_CODE:
                    seqLevel = false;
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
        }
        case EXT_START_SHORT_CODE:
            return processExtStartCode(buff);
        case GOP_START_SHORT_CODE:
            // ** A GROUP OF PICTURES WITHOUT A SEQUENCE HEADER BEGINS WITH ITS GROUP HEADER. ** Its picture is decoded
            // here, as after a sequence header, so the picture's packet begins at the group header: it was left at the
            // end of the picture before, and the I picture's PES, where a player starts, began without it. A stream
            // that repeats the sequence header comes here through the case above and is unchanged.
            m_framesAtGop = -1;
            m_lastRef = -1;
            nextNal = MPEGHeader::findNextMarker(buff, m_bufEnd) + 3;
            while (true)
            {
                if (nextNal >= m_bufEnd)
                    return NOT_ENOUGH_BUFFER;
                if (*nextNal == PICTURE_START_SHORT_CODE)
                {
                    rez = decodePicture(nextNal);
                    if (rez == 0)
                        m_lastDecodedPos = nextNal;
                    return rez;
                }
                if (*nextNal != EXT_START_SHORT_CODE && *nextNal != USER_START_SHORT_CODE)
                    return 0;  // not the syntax of H.262 6.2.2: the group header stays where it is
                nextNal = MPEGHeader::findNextMarker(nextNal, m_bufEnd) + 3;
            }
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
    // sequence_extension: 4 bits of its id (1), 8 of profile and level, then progressive_sequence
    m_nextProgressive = -1;
    if (m_bufEnd - nextNal >= 6 && nextNal[0] == 0 && nextNal[1] == 0 && nextNal[2] == 1 && nextNal[3] == 0xB5 &&
        nextNal[4] >> 4 == 1)
        m_nextProgressive = nextNal[5] >> 3 & 1;
    updateFPS(nullptr, buff, nextNal, oldSpsLen);
    // ** A SEQUENCE HEADER WITHOUT A FRAME RATE. ** frame_rate_code 0 is forbidden, and the stream got the rate fps=,
    // its picture times or the default gave, but its pictures were timed by the code: every one at the same time, the
    // whole stream in one PES, the HDMV descriptor with rate 0, and "Mux successful complete". The rate the stream is
    // muxed at is written into the header, so the pictures are timed by it and a player reads it.
    if (m_sequence.getFrameRate() == 0.0 && m_fps > 0.0)
    {
        m_sequence.setFrameRate(buff + 1, m_fps);
        if (!m_noRateSaid)
        {
            m_noRateSaid = true;
            LTRACE(
                LT_WARN, 2,
                "Warning: the MPEG-2 sequence header gives no frame rate (frame_rate_code 0). The stream is muxed at "
                    << m_fps << " fps, written into its header; give fps= on the track's line for another rate.");
        }
    }
    spsFound = true;
    m_matrixSinceSeqHeader = false;
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
        else if (extType == QUANT_MATRIX_EXT)
            m_matrixSinceSeqHeader = true;
        return 0;
    }
    catch (BitStreamException&)
    {
        return NOT_ENOUGH_BUFFER;
    }
}

// ** AN I PICTURE WHERE A PLAYER STARTS HAS A SEQUENCE HEADER IN FRONT OF IT. ** A disc's EP map has an entry at
// every I picture, and a player starts decoding there after a chapter jump, a seek or a resume. A decoder that starts
// there needs the sequence header: picture size, frame rate, profile and the quantiser matrices (H.262 6.1.1.6:
// repeating it is what makes random access into the sequence possible). H.262 lets a stream carry it at its start
// alone, and a stream so made gave a disc whose every entry but the first began with a bare picture header, where no
// picture decodes. On a disc, the last sequence header, with its sequence extension and display extension, is now
// written before each I picture that has none, as for H.264 and HEVC (muxerManager.cpp, setH264SPSCont;
// HEVCStreamReader::writeAdditionData); so is the first I picture of each part of a split and of a cut, on any
// output. A stream that repeats the header, as every disc and DVD does, is left exactly as it is. A repeated
// sequence header sets the quantiser matrices again (H.262 6.3.3, 6.3.11), so the copy is not written where a quant
// matrix extension changed them after the last header: the pictures that follow would decode with other matrices.
int MPEG2StreamReader::writeAdditionData(uint8_t* dstBuffer, uint8_t* dstEnd, AVPacket& avPacket,
                                         PriorityDataInfo* priorityData)
{
    if (!(avPacket.flags & AVPacket::IS_IFRAME) || avPacket.data == nullptr || avPacket.size < 4 ||
        m_seqHeaderCopy.empty())
        return 0;
    const bool entryPoint = (V3_flags & BLURAY_OUT) != 0;
    if (!entryPoint && !m_firstFileFrame)
        return 0;
    m_firstFileFrame = false;
    // the start code of the first unit of the picture's data
    const uint8_t* p = avPacket.data;
    const uint8_t* end = avPacket.data + avPacket.size;
    while (p < end && *p == 0) ++p;
    if (p + 1 >= end || *p != 1 || p - avPacket.data < 2 || p[1] == SEQ_START_SHORT_CODE)
        return 0;
    if (m_matrixSinceSeqHeader)
    {
        if (!m_seqRepeatSkipSaid)
        {
            m_seqRepeatSkipSaid = true;
            LTRACE(LT_WARN, 2,
                   "Warning: MPEG-2 track " << m_streamIndex
                                            << ": an I picture has no sequence header in front of it, and a quant "
                                               "matrix extension came after the last one, so none is repeated there. "
                                               "A player cannot start at such a picture.");
        }
        return 0;
    }
    const auto len = static_cast<int64_t>(m_seqHeaderCopy.size());
    if (dstEnd - dstBuffer < len)
        THROW(ERR_COMMON, "MPEG-2 stream error: not enough buffer to repeat the sequence header")
    if (!m_seqRepeatSaid)
    {
        m_seqRepeatSaid = true;
        LTRACE(LT_INFO, 2,
               "MPEG-2 track " << m_streamIndex
                               << ": an I picture where a player can start has no sequence header; a copy of the "
                                  "last one is written in front of it");
    }
    memcpy(dstBuffer, m_seqHeaderCopy.data(), static_cast<size_t>(len));
    return static_cast<int>(len);
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

    // An I or P picture is shown after the B pictures that follow it in the stream, so its time is known only when
    // it is known how long those B pictures are shown: they are read first, and the picture waits for them. This comes
    // before findFrameExt: with delPulldown that takes the marks out of the picture, which is read again after a wait.
    const bool anchor = m_frame.pict_type != PictureCodingType::B_FRAME;
    int64_t bShownFor = 0;
    if (anchor && (m_frame.pict_type == PictureCodingType::I_FRAME || m_frame.ref != m_lastRef))
    {
        const int bRez = timeOfFollowingB(buff, bShownFor);
        if (bRez != 0)
            return bRez;
    }

    m_frame.picture_structure = 0;
    const int rez = findFrameExt(buff + 1);
    if (rez == NOT_ENOUGH_BUFFER)
        return rez;

    // The second field of a picture coded as two fields has the temporal reference of the first and is no picture of
    // its own. When both fields of a picture are I pictures, the second one started a new count, as an I picture
    // does, and was counted as one more picture: a frame longer at each such picture.
    const bool field = m_frame.picture_structure == 1 || m_frame.picture_structure == 2;
    const bool secondField = field && m_firstFieldPending && m_frame.ref == m_lastRef;
    m_firstFieldPending = field && !secondField;

    if (m_frame.pict_type == PictureCodingType::I_FRAME && !secondField)
    {
        m_framesAtGop = -1;
        m_lastRef = -1;
    }

    if (m_frame.ref != m_lastRef)
    {
        m_framesAtGop++;
        m_lastRef = m_frame.ref;
        m_totalFrameNum++;

        const int64_t shownFor = pictureDuration(m_frame.repeat_first_field != 0, m_frame.top_field_first != 0);
        // Without the marks every picture is one frame of the new rate
        const int64_t duration = m_removePulldown ? m_pcrIncPerFrame : shownFor;

        // A picture with repeat_first_field is shown for 3 fields, one without it for 2, so the pictures of film
        // with pulldown are not shown for equal times. The times used to be counted as if each picture started when
        // the one before it in the stream ended, and the pictures in between were then moved by whole frames to the
        // order they are shown in: some pictures got the time of the one before them, others left a gap of two. Each
        // time is now where its picture is shown: a B picture when it is decoded, an I or P picture when the next I
        // or P picture is decoded, after the B pictures between them.
        int64_t dts;
        if (anchor)
        {
            if (m_anchorFound)
            {
                dts = m_anchorPts;
                m_nextShownPts = m_anchorPts + m_anchorDuration;
            }
            else
            {
                m_nextShownPts = PTS_CONST_OFFSET;
                dts = PTS_CONST_OFFSET - m_pcrIncPerFrame;
            }
            m_curPts = m_nextShownPts + bShownFor;
            m_anchorPts = m_curPts;
            m_anchorDuration = duration;
            m_anchorFound = true;
        }
        else
        {
            m_curPts = dts = m_nextShownPts;
            m_nextShownPts += duration;
        }
        // The packets take one frame off the decoding time (readPacket)
        m_curDts = dts + m_pcrIncPerFrame;

        if (m_removePulldown)
        {
            if (shownFor != m_pcrIncPerFrame)
                m_pulldownMarkSeen = true;
            checkPulldownSync();
            m_testPulldownDts += shownFor;
        }
    }
    m_isFirstFrame = false;
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

int64_t MPEG2StreamReader::pictureDuration(const bool repeatFirstField, const bool topFieldFirst) const
{
    if (!repeatFirstField)
        return m_pcrIncPerFrame;
    if (!m_sequence.progressive_sequence)
        return m_pcrIncPerFrame + m_pcrIncPerField;  // 3 fields
    // In a progressive sequence the whole frame is repeated: shown for 3 frames with top_field_first, else for 2
    return m_pcrIncPerFrame * (topFieldFirst ? 3 : 2);
}

// How long the B pictures after the I or P picture at buff are shown, up to the next I or P picture. They come after
// it in the stream and are shown before it. NOT_ENOUGH_BUFFER while they are not all read yet.
int MPEG2StreamReader::timeOfFollowingB(uint8_t* buff, int64_t& shownFor)
{
    // A limit for a stream that never comes to another I or P picture: the buffer is not filled with it
    static constexpr int MAX_B_PICTURES = 64;
    shownFor = 0;
    int lastRef = m_frame.ref;
    int bPictures = 0;
    for (uint8_t* nal = MPEGHeader::findNextMarker(buff + 1, m_bufEnd);;
         nal = MPEGHeader::findNextMarker(nal + 4, m_bufEnd))
    {
        if (m_bufEnd - nal < 6)
            return m_eof ? 0 : NOT_ENOUGH_BUFFER;
        if (nal[3] == SEQ_END_SHORT_CODE)
            return 0;
        if (nal[3] == GOP_START_SHORT_CODE)
            lastRef = -1;
        if (nal[3] != PICTURE_START_SHORT_CODE)
            continue;
        const int ref = nal[4] << 2 | nal[5] >> 6;
        const auto type = static_cast<PictureCodingType>((nal[5] >> 3) & 7);
        // The picture coding extension follows the picture header; MPEG-1 has none
        uint8_t* ext = MPEGHeader::findNextMarker(nal + 4, m_bufEnd);
        if (m_bufEnd - ext < 8)
            return m_eof ? 0 : NOT_ENOUGH_BUFFER;
        const bool codingExt =
            ext[3] == EXT_START_SHORT_CODE && static_cast<unsigned>(ext[4] >> 4) == PICTURE_CODING_EXT;
        const bool frame = !codingExt || (ext[6] & 3) == 3;
        // the second field of a picture coded as two fields, counted as decodePicture counts it, also when both
        // fields are I pictures: such an I field ended the B pictures early
        if (ref == lastRef && (type != PictureCodingType::I_FRAME || !frame))
            continue;
        if (type != PictureCodingType::B_FRAME)
            return 0;
        lastRef = ref;
        bool repeatFirstField = false, topFieldFirst = false;
        if (codingExt)
        {
            topFieldFirst = (ext[7] & 0x80) != 0;
            repeatFirstField = (ext[7] & 0x02) != 0;
        }
        shownFor += m_removePulldown ? m_pcrIncPerFrame : pictureDuration(repeatFirstField, topFieldFirst);
        if (++bPictures == MAX_B_PICTURES)
            return 0;
        nal = ext;
    }
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

// ** THE UNITS THAT CANNOT BE MPEG-2 VIDEO (see MPEGStreamReader::unusableUnit). ** ITU-T H.262 Table 6-1: the
// reserved start codes B0, B1 and B6, and B9 to FF, which are system start codes and not video. A picture header
// whose picture_coding_type is 0 or 5 to 7 (Table 6-12, forbidden and reserved) is a picture by its code alone. The
// longest unit of a type is the size its syntax comes to, with room to spare: a sequence header with both
// quantiser matrices is 140 bytes.
bool MPEG2StreamReader::unusableUnit(const uint8_t* unit, const uint8_t* end, int& maxLen)
{
    maxLen = 0;
    const int code = unit[0];
    if (code == 0xB0 || code == 0xB1 || code == 0xB6 || code >= 0xB9)
        return true;
    switch (code)
    {
    case 0x00:  // picture header
    {
        maxLen = 256;
        if (end - unit < 3)
            return false;
        const int codingType = (unit[2] >> 3) & 7;
        return codingType == 0 || codingType > 4;
    }
    case 0xB3:  // sequence header
        maxLen = 160;
        break;
    case 0xB5:  // extension
        maxLen = 512;
        break;
    case 0xB7:  // sequence end
    case 0xB8:  // group of pictures
        maxLen = 64;
        break;
    default:
        break;
    }
    return false;
}

// The last picture of an MPEG-2 file: the last 8 MiB hold it, with the sequence header in force or the one read
void MPEG2StreamReader::checkFileEnd(const std::string& fileName)
{
    if (!m_seqExtFound)
        return;  // MPEG-1
    const std::vector<uint8_t> tail = readFileTail(fileName, 8 << 20);
    if (tail.empty())
        return;
    reportPictureEnd(mpeg2PictureEnd(tail.data(), tail.data() + tail.size(), m_sequence.width, m_sequence.height,
                                     m_sequence.chroma_format, m_sequence.progressive_sequence != 0),
                     "macroblocks");
}
