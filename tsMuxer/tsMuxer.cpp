#include "tsMuxer.h"

#include <cmath>

#include <fs/systemlog.h>
#include <fs/textfile.h>

#include "ac3StreamReader.h"
#include "dtsStreamReader.h"
#include "h264StreamReader.h"
#include "hevcStreamReader.h"
#include "mpegAudioStreamReader.h"
#include "mpegStreamReader.h"
#include "muxerManager.h"
#include "pesPacket.h"
#include "srtStreamReader.h"
#include "trueHDAC3MergeReader.h"
#include "tsPacket.h"
#include "vodCoreException.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using namespace std;

int V3_flags = 0;
unsigned HDR10_metadata[6] = {0, 0, 0, 0, 0, 0};
bool isV3() { return V3_flags & HDMV_V3; }
bool is4K() { return V3_flags & FOUR_K; }

static constexpr int64_t M_PCR_DELTA = 7000;
static constexpr int64_t SIT_INTERVAL = 76900;
static constexpr int64_t M_CBR_PCR_DELTA = 7000;

static constexpr int64_t DEFAULT_VBV_BUFFER_LEN = 500;  // default 500 ms vbv buffer

// The muxing rate, in bits per second, at which the PCR pacing stops working. The pacing term
// used in three places below is (m_lastPCR + m_pcrBits) * PCR_FREQUENCY / m_cbrBitrate, and it
// adds m_pcrBits, a count of bits, to m_lastPCR, a count of 90 kHz ticks. The sum only stays
// under the media time while PCR_FREQUENCY / m_cbrBitrate is below one, so the term degenerates
// once the rate in bits per second falls to the tick rate. That is why this is the same number
// as PCR_FREQUENCY rather than a chosen one. Measured on a two second, 258,500 byte mux:
// 91 kbps and everything above it is unaffected, 90 kbps gives 624,348 bytes, 89 kbps gives
// 23.9 GB and 35 kbps gives 292 MB.
static constexpr int DEGENERATE_MUX_RATE = PCR_FREQUENCY;

// What a Blu-ray drive is required to supply. BD-ROM puts the rate at 48 Mbit/s for HD and 109 for
// UHD, and those put a floor under the gap between the arrival timestamps of consecutive packets.
// Below the floor the disc is asking for more than the drive has to give.
//
// ** THE RATE IS THE TRANSPORT STREAM MULTIPLEX RATE, SO IT IS COUNTED ON THE 188 BYTE PACKET. **
// The four byte arrival timestamp is what storing it as .m2ts adds on top, and it is not part of
// the multiplex. Counting the whole 192 makes the floor 864 ticks instead of 846 and flags a legal
// disc, and 846 is also what upstream issue 141 states.
//
// The official BDA physical white paper puts the user data transfer rate of a BD-ROM movie disc at
// 53.948 Mbit/s. 48 Mbit/s of transport stream is 31,915 packets a second, which the drive reads as
// 49.02 Mbit/s once each carries its timestamp, and that fits with headroom. Reading the limit the
// other way would leave nearly 6 Mbit/s of the physical rate unused, which is not how it is meant.
//
// The HD figure is also measured rather than only read: three commercially authored discs were
// checked and none of the 3.3 million packets between them has a gap below 846 ticks, which is
// exactly 48 Mbit/s on the 188 byte packet.
//
// The UHD figure comes from the BDA white paper for BD-ROM Version 3, where the maximum multiplex
// rate of the transport stream is given per disc capacity and per zone: 64 and 81.7 Mbit/s for a
// 50 GB disc, 81.7 and 109 for 66 and 100 GB, and 127.9 in the high transfer rate zone of the two
// larger ones. 109 is therefore not the only legal answer, it is the one that fits the capacities
// UHD titles are actually pressed on. A single number is a simplification and this is the useful
// place to put it.
//
// Which of the two applies is decided by isV3(), and that is set for ANY HEVC on a disc, not only
// for 3840 wide video. That is deliberate: HEVC exists on Blu-ray only in Version 3, so even a
// 1920 wide HEVC disc needs a player that reads at the Version 3 rate. Measured, a 1080p HEVC disc
// asking 57.0 Mbit/s says nothing, and that is the intended answer rather than a missed warning.
static constexpr double TS_PACKET_BITS = 188.0 * 8.0;
static constexpr double BD_READ_RATE_HD = 48.0e6;
static constexpr double BD_READ_RATE_UHD = 109.0e6;

static double atsGapFloor(const double bitsPerSecond) { return TS_PACKET_BITS * 27000000.0 / bitsPerSecond; }

// A pass of this many packets or fewer is the muxer's own tables written together with a PCR (PAT, PMT,
// SIT and the PCR packet), not the streams. See pcrForMaxRate and processM2TSPCR.
static constexpr int TABLE_BURST_PACKETS = 5;

// H.222.0 2.7.2: the time between two PCRs shall be 0.1 s or less, 9000 ticks of the 90 kHz base.
static constexpr int64_t PCR_MAX_INTERVAL = 9000;
// Kept free below that limit for what can still be added to a PCR after it is chosen: pcrForMaxRate holds a short
// pass of tables at the read-rate floor, which moves the PCR by a few 90 kHz ticks at most.
static constexpr int64_t PCR_INTERVAL_MARGIN = 90;

static double atsGapToRate(const double gap) { return TS_PACKET_BITS * 27000000.0 / gap; }

static constexpr int PAT_PID = 0;
static constexpr int SIT_PID = 0x1f;
static constexpr int NULL_PID = 8191;

uint8_t DefaultSitTableOne[] = {
    0x47, 0x40, 0x1f, 0x10, 0x00, 0x7f, 0xf0, 0x19, 0xff, 0xff, 0xc1, 0x00, 0x00, 0xf0, 0x0a, 0x63, 0x08, 0xc1, 0xd4,
    0xc0, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x01, 0x80, 0x00, 0x03, 0x00, 0x38, 0x6d, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

// for v3 Blu-ray, change "peak_rate" and CRC to 128 mbps
uint8_t SitTableHEVC[] = {0xc4, 0xe1, 0x06, 0xff, 0xff, 0xff, 0xff, 0xff,
                          0x00, 0x01, 0x80, 0x00, 0x24, 0xc4, 0xba, 0xf0};

TSMuxer::TSMuxer(MuxerManager* owner) : AbstractMuxer(owner)
{
    m_m2tsMode = false;
    m_vbvLen = DEFAULT_VBV_BUFFER_LEN * 90;
    m_lastPESDTS = -1;
    m_cbrBitrate = -1;  // mux CBR if bitrate specifed
    m_maxBitrate = -1;  // the ceiling, from --maxbitrate only
    m_minBitrate = -1;
    m_pcrOnVideo = true;
    m_endStreamDTS = 0;
    m_prevM2TSPCROffset = 0;
    m_pesPID = 0;
    m_lastTSIndex = -1;
    m_lastPesLen = -1;
    m_lastMuxedDts = -1;
    m_lastPCR = -1;
    m_lastPMTPCR = -1;
    m_lastSITPCR = -1;
    m_nullCnt = 0;
    m_pmtCnt = 0;
    m_patCnt = 0;
    m_sitCnt = 0;
    m_needTruncate = false;
    m_videoTrackCnt = 0;
    m_DVvideoTrackCnt = 0;
    m_videoSecondTrackCnt = 0;
    m_audioTrackCnt = 0;
    m_secondaryAudioTrackCnt = 0;
    m_pgsTrackCnt = 0;
    m_beforePCRDataWrited = false;
    m_useNewStyleAudioPES = false;
    m_minDts = -1;
    m_pcrBits = 0;
    m_minAtsGap = 0;
    m_atsPackets = 0;
    m_atsOverLimit = 0;
    m_cbrEvalPCR = -2;  // sentinel: differs from any real m_lastPCR incl. -1
    m_cbrNextEvalBits = 0;
    m_inplacePending = false;
    m_midPesPCR = false;
    m_pendingStartOffset = 0;
    m_cappedPictures = 0;
    m_latePictures = 0;
    m_worstLateness = 0;
    m_pendingStartPktCnt = 0;
    m_pendingTsPackets = 0;
    m_pendingPayloadStart = false;
    m_pendingTailPriority = false;
    m_pendingTailLen = 0;
    m_pendingPesPts = 0;
    m_pendingPesHasPts = false;
    m_outBufCapacity = 0;
    m_pcr_delta = -1;
    m_patPmtDelta = -1;
    m_firstPts.push_back(-1);
    m_lastPts.push_back(-1);
    m_muxedPacketCnt.push_back(0);
    m_interleaveInfo.emplace_back();
    m_pesIFrame = false;
    m_pesSpsPps = false;
    m_computeMuxStats = false;
    m_pmtFrames = 0;
    m_curFileStartPts = 0;  // FIXED_PTS_OFFSET;
    m_splitSize = 0;
    m_splitDuration = 0;
    m_splitByChapters = false;
    m_nextChapter = 0;
    m_curFileNum = 0;
    m_bluRayMode = false;
    m_hdmvDescriptors = true;
    m_lastGopNullCnt = 0;
    m_outBufLen = 0;
    m_pesData.reserve(1024 * 128);
    m_mainStreamIndex = -1;
    m_mainStreamRank = 0;
    m_muxFile = nullptr;
    m_isExternalFile = false;
    m_writeBlockSize = 0;
    m_frameSize = 188;

    m_processedBlockSize = 0;
    m_sublingMuxer = nullptr;
    m_outBuf = nullptr;
    m_masterMode = false;
    m_subMode = false;
    setPtsOffset(0);
    m_canSwithBlock = true;
    m_additionCLPISize = 0;
#ifdef _DEBUG
    m_lastProcessedDts = -1000000000;
    m_lastStreamIndex = -1;
#endif
}

void TSMuxer::setPtsOffset(const int64_t value)
{
    m_timeOffset = value;
    m_fixed_pcr_offset = m_timeOffset - DEFAULT_VBV_BUFFER_LEN * 90;
    m_prevM2TSPCR = m_fixed_pcr_offset * 300;
}

void TSMuxer::internalReset() { m_outBufLen = 0; }

TSMuxer::~TSMuxer()
{
    delete[] m_outBuf;
    if (!m_isExternalFile)
        delete m_muxFile;
}

void TSMuxer::setVBVBufferLen(const int value)
{
    m_vbvLen = static_cast<int64_t>(value) * 90;
    m_fixed_pcr_offset = m_timeOffset - m_vbvLen;
    if (m_fixed_pcr_offset < 0)
        m_fixed_pcr_offset = 0;
    m_prevM2TSPCR = m_fixed_pcr_offset * 300;
}

void TSMuxer::intAddStream(const std::string& streamName, const std::string& codecName, const int streamIndex,
                           const map<string, string>& params, AbstractStreamReader* codecReader)
{
    int descriptorLen = 0;
    uint8_t descrBuffer[1024];
    if (codecReader != nullptr)
        descriptorLen = codecReader->getTSDescriptor(descrBuffer, m_bluRayMode, m_hdmvDescriptors);

    // A Blu-ray disc carries audio at 48, 96 or 192 kHz, and its stream information has a value for those rates only
    // (M2TSStreamInfo). Audio at any other rate, such as AC-3 at 44.1 kHz from a CD or a video file, went onto the
    // disc without a word, with no valid rate in its stream information. The rate is read from the frames themselves.
    if (m_bluRayMode && !codecName.empty() && codecName[0] == 'A')
        if (auto* audio = dynamic_cast<SimplePacketizerReader*>(codecReader))
        {
            const int freq = audio->getFreq();
            if (freq > 0 && freq != 48000 && freq != 96000 && freq != 192000)
                LTRACE(LT_WARN, 2,
                       "Warning: track " << streamIndex << " (" << audio->getCodecInfo().displayName
                                         << ") has a sample rate of " << freq
                                         << " Hz. A Blu-ray disc carries audio at 48, 96 or 192 kHz only, and "
                                         << "the disc's stream information has no value for this rate, so a player may "
                                            "refuse the track. Convert the audio to 48 kHz before muxing.");
        }

    // Which stream is the main one settles the split points, the interleaving and, until this was
    // fixed, the entire title length. The original test was unguarded for video, so the LAST video
    // track added won it, and since only the main stream advanced the running duration the playlist
    // ended up describing whichever video the meta happened to list last. A second video track that
    // is legitimately shorter than the feature, a picture in picture stream or a Dolby Vision
    // enhancement layer, therefore produced a disc that a player ended early even though the feature
    // had been written in full, and every chapter mark past that point was dropped as out of range.
    //
    // Ordering is the wrong thing to key on in either direction, so the choice is made from what the
    // track IS. The classification is the one the PID assignment below already makes: a primary
    // video is neither secondary nor a Dolby Vision enhancement layer, which is exactly the track
    // that gets PID 0x1011. It outranks any other video, which outranks audio, and a higher rank
    // never loses to a lower one whatever order the meta lists them in.
    const bool isVideo = codecName[0] == 'V';
    const bool isSecondary = codecReader != nullptr && codecReader->isSecondary();
    // A stream carrying Dolby Vision is the ENHANCEMENT layer only when a base layer came before
    // it. In profile 5 and profile 8 there is none: the RPU travels in the base, so the only video
    // stream there is belongs on 0x1011 and IS the primary video. m_videoTrackCnt counts the base
    // layers seen so far and has not been touched for this stream yet.
    const bool isDvEnhancement = codecReader != nullptr && codecReader->getStreamHDR() == 4 && m_videoTrackCnt > 0;
    const bool isPrimaryVideo = isVideo && !isSecondary && !isDvEnhancement;

    const int rank = isPrimaryVideo ? 3 : (isVideo ? 2 : (codecName[0] == 'A' ? 1 : 0));
    if (rank > m_mainStreamRank)
    {
        m_mainStreamRank = rank;
        m_mainStreamIndex = streamIndex;
    }

    string lang;
    auto itr = params.find("lang");
    if (itr != params.end())
        lang = itr->second;
    else if (const auto src = params.find("srclang"); src != params.end())
    {
        // Carried over from the source container by muxerManager when the meta gave no lang=.
        // language_code is a mandatory fixed 3 byte field in the STN stream attributes
        // (tsPacket.cpp), so leaving it blank is the deviation, not filling it: a player then
        // shows an unnamed audio track. Video has no such field and is unaffected.
        lang = src->second;

        // A disc and a transport stream carry three letters, so the region or script of a BCP 47 tag is lost here, and
        // two source tracks tagged pt-PT and pt-BR both become "por". Say so once, so the tracks can be named by hand.
        if (const auto tag = params.find("srclangbcp47"); tag != params.end())
        {
            std::set<std::string>& tags = m_bcp47ByLang[lang];
            if (tags.insert(tag->second).second && tags.size() == 2)
                LTRACE(LT_WARN, 2,
                       "Warning: tracks tagged " << *tags.begin() << " and " << *std::next(tags.begin())
                                                 << " in the source both carry the language " << lang
                                                 << " here. A disc or a transport stream holds three letters only, so "
                                                    "they can no longer be told apart by their language.");
        }
    }

    int tsStreamIndex = streamIndex + 16;

    if (codecName[0] == 'V')
    {
        if (!isSecondary)
        {
            const uint16_t doubleMux = (m_subMode || m_masterMode) ? 2 : 1;
            // A stream that carries Dolby Vision is the ENHANCEMENT layer only when a base
            // layer came before it. getStreamHDR() == 4 says nothing more than "there is
            // Dolby Vision in here", and in profile 5 and profile 8 there is no enhancement
            // layer at all: the RPU travels in the base, so the only video stream there is
            // belongs on 0x1011.
            //
            // Without the count, such a disc put its only picture on 0x1015, where no player
            // looks for primary video. Measured on a player: it does not play at all, while
            // the same picture with the RPU stripped, which lands on 0x1011, plays as HDR.
            // The old behaviour was therefore strictly worse than the fallback it denied.
            //
            // It also disagreed with itself. BL_TRACK is set only in the branch below, and
            // HEVCStreamReader derives isDVBL from BL_TRACK, so a single layer stream was
            // described as a base layer while being placed on the enhancement layer PID.
            if (isDvEnhancement)
            {
                tsStreamIndex = 0x1015 + m_DVvideoTrackCnt * doubleMux;
                m_DVvideoTrackCnt++;
            }
            else
            {
                tsStreamIndex = 0x1011 + m_videoTrackCnt * doubleMux;
                m_videoTrackCnt++;
                V3_flags |= BL_TRACK;
            }
            if (m_subMode)
                tsStreamIndex++;
        }
        else
        {
            tsStreamIndex = 0x1B00 + m_videoSecondTrackCnt;
            m_videoSecondTrackCnt++;
        }
    }
    else if (codecName[0] == 'A')
    {
        if (isSecondary)
        {
            tsStreamIndex = 0x1A00 + m_secondaryAudioTrackCnt;
            m_secondaryAudioTrackCnt++;
        }
        else
        {
            tsStreamIndex = 0x1100 + m_audioTrackCnt;
            m_audioTrackCnt++;
        }
    }
    else if (codecName == "S_HDMV/PGS" || codecName == "S_TEXT/UTF8")
    {
        // A transport stream, and so a disc, has no common way to carry text subtitles: a track that
        // asked to keep them as text is drawn into pictures as usual, and says so
        if (const auto srt = dynamic_cast<SRTStreamReader*>(codecReader); srt && srt->keepText())
        {
            if (!srt->hasVideoInfo())
                THROW(ERR_COMMON, "Track "
                                      << streamIndex
                                      << ": keep-text applies to Matroska output and to a demux. This output draws the "
                                         "subtitles into pictures, which needs video-width, video-height and fps.")
            LTRACE(LT_WARN, 2,
                   "Warning: a transport stream has no common way to carry text subtitles, so track "
                       << streamIndex
                       << " is drawn into pictures as usual. keep-text applies to Matroska output and to a demux.");
            srt->setKeepText(false);
        }
        tsStreamIndex = (V3_flags & 0x1e ? 0x12A0 : 0x1200) + m_pgsTrackCnt;
        m_pgsTrackCnt++;
    }
    m_extIndexToTSIndex[streamIndex] = tsStreamIndex;

    m_pmt.program_number = 1;
    if (m_pcrOnVideo)
    {
        if (codecName[0] == 'V')
            m_pmt.pcr_pid = tsStreamIndex;
    }
    else
        m_pmt.pcr_pid = DEFAULT_PCR_PID;

    if (codecName[0] == 'V')
    {
        if (codecName == "V_MS/VFW/WVC1")
        {
            m_pesType[tsStreamIndex] = PES_VC1_ID;
        }
        else if (codecName == "V_MPEGH/ISO/HEVC")
        {
            m_pesType[tsStreamIndex] = PES_HEVC_ID;
        }
        else if (codecName == "V_MPEGI/ISO/VVC")
        {
            m_pesType[tsStreamIndex] = PES_VVC_ID;
        }
        else if (codecName == "V_AV1")
        {
            m_pesType[tsStreamIndex] = PES_PRIVATE_DATA1;
        }
        else
        {
            m_pesType[tsStreamIndex] = PES_VIDEO_ID;
        }
    }
    else if (codecName[0] == 'A')
    {
        if (codecName == "A_AC3")
            m_pesType[tsStreamIndex] = PES_INT_AC3_ID;
        else if (codecName == "A_MLP")
            // Only the AC-3 core merge gets the AC3 id, which --new-audio-pes then maps to
            // stream id 0xfd as on commercial discs. A merged track writes the matching PES
            // extension (TrueHDAC3MergeReader::writePESExtension), so 0xfd is honest there.
            // Plain TrueHD has no such extension, and giving it 0xfd would announce an
            // extended_stream_id that is not present, which is malformed per H.222.0 Table
            // 2-22. It keeps the private-stream id 0xbd it has always had.
            m_pesType[tsStreamIndex] =
                dynamic_cast<TrueHDAC3MergeReader*>(codecReader) != nullptr ? PES_INT_AC3_ID : PES_PRIVATE_DATA1;
        else if (codecName == "A_DTS")
            m_pesType[tsStreamIndex] = PES_INT_DTS_ID;
        else if (codecName == "A_MP3")
            m_pesType[tsStreamIndex] = PES_AUDIO_ID;
        else
            m_pesType[tsStreamIndex] = PES_PRIVATE_DATA1;
    }
    else if (codecName[0] == 'S')
    {
        if (codecName == "S_SUP" || codecName == "S_HDMV/PGS" || codecName == "S_TEXT/UTF8")
            m_pesType[tsStreamIndex] = PES_PRIVATE_DATA1;
        else
            m_pesType[tsStreamIndex] = PES_INT_SUB_ID;
    }

    if (codecName[0] == 'V')
    {
        itr = params.find("fps");
        if (itr != params.end())
        {
            double fps = strToDouble(itr->second.c_str());
            LTRACE(LT_DEBUG, 0, "Muxing fps: " << fps);
        }
    }

    if (codecName == "V_MPEG4/ISO/AVC")
    {
        m_pmt.pidList[tsStreamIndex] = PMTStreamInfo(StreamType::VIDEO_H264, tsStreamIndex, descrBuffer, descriptorLen,
                                                     codecReader, lang, isSecondary);
    }
    else if (codecName == "V_MPEG4/ISO/MVC")
    {
        m_pmt.pidList[tsStreamIndex] = PMTStreamInfo(StreamType::VIDEO_MVC, tsStreamIndex, descrBuffer, descriptorLen,
                                                     codecReader, lang, isSecondary);
    }
    else if (codecName == "V_MPEGH/ISO/HEVC")
    {
        StreamType stream_type = StreamType::VIDEO_H265;
        // Change "peak_rate" to 109 mbps + change descriptor CRC32
        memcpy(&DefaultSitTableOne[17], SitTableHEVC, 16);
        // For non-bluray, second Dolby Vision track must be stream_type 06 = private data
        if (!m_bluRayMode && tsStreamIndex == 0x1015 && (V3_flags & BL_TRACK))
            stream_type = StreamType::PRIVATE_DATA;
        // Dolby Vision profile 5 is not compatible with SDR or HDR and must be stream_type 06 = private data
        // else if (V3_flags & BL_NOTCOMPAT)
        //     stream_type = StreamType::PRIVATE_DATA;

        m_pmt.pidList[tsStreamIndex] =
            PMTStreamInfo(stream_type, tsStreamIndex, descrBuffer, descriptorLen, codecReader, lang, isSecondary);
    }
    else if (codecName == "V_MPEGI/ISO/VVC")
    {
        m_pmt.pidList[tsStreamIndex] = PMTStreamInfo(StreamType::VIDEO_H266, tsStreamIndex, descrBuffer, descriptorLen,
                                                     codecReader, lang, isSecondary);
    }
    else if (codecName == "V_AV1")
    {
        m_pmt.pidList[tsStreamIndex] = PMTStreamInfo(StreamType::PRIVATE_DATA, tsStreamIndex, descrBuffer,
                                                     descriptorLen, codecReader, lang, isSecondary);
    }
    else if (codecName == "V_MS/VFW/WVC1")
        m_pmt.pidList[tsStreamIndex] = PMTStreamInfo(StreamType::VIDEO_VC1, tsStreamIndex, descrBuffer, descriptorLen,
                                                     codecReader, lang, isSecondary);
    else if (codecName == "V_MPEG-2")
        m_pmt.pidList[tsStreamIndex] = PMTStreamInfo(StreamType::VIDEO_MPEG2, tsStreamIndex, descrBuffer, descriptorLen,
                                                     codecReader, lang, isSecondary);
    else if (codecName == "A_AAC")
        m_pmt.pidList[tsStreamIndex] = PMTStreamInfo(StreamType::AUDIO_AAC, tsStreamIndex, descrBuffer, descriptorLen,
                                                     codecReader, lang, isSecondary);
    else if (codecName == "S_SUP" || codecName == "S_HDMV/PGS" || codecName == "S_TEXT/UTF8")
        m_pmt.pidList[tsStreamIndex] = PMTStreamInfo(StreamType::SUB_PGS, tsStreamIndex, descrBuffer, descriptorLen,
                                                     codecReader, lang, isSecondary);
    else if (codecName == "A_AC3")
    {
        const auto ac3Reader = dynamic_cast<AC3StreamReader*>(codecReader);
        ac3Reader->setNewStyleAudioPES(m_useNewStyleAudioPES);

        StreamType streamType;

        if (ac3Reader->isTrueHD() && !ac3Reader->getDownconvertToAC3())
            streamType = StreamType::AUDIO_TRUE_HD;
        else if (ac3Reader->isEAC3() && !ac3Reader->getDownconvertToAC3())
        {
            if (m_bluRayMode || m_m2tsMode)
            {
                if (ac3Reader->isSecondary())
                    streamType = StreamType::AUDIO_EAC3_SECONDARY;
                else
                    streamType = StreamType::AUDIO_EAC3;
            }
            else
            {
                streamType = StreamType::AUDIO_EAC3_ATSC;
            }
        }
        else
            streamType = StreamType::AUDIO_AC3;

        m_pmt.pidList[tsStreamIndex] =
            PMTStreamInfo(streamType, tsStreamIndex, descrBuffer, descriptorLen, codecReader, lang, isSecondary);
    }
    else if (codecName == "A_MP3")
    {
        const auto mp3Reader = dynamic_cast<MpegAudioStreamReader*>(codecReader);
        if (mp3Reader->getLayer() >= 2)
            m_pmt.pidList[tsStreamIndex] = PMTStreamInfo(StreamType::AUDIO_MPEG2, tsStreamIndex, descrBuffer,
                                                         descriptorLen, codecReader, lang, isSecondary);
        else
            m_pmt.pidList[tsStreamIndex] = PMTStreamInfo(StreamType::AUDIO_MPEG1, tsStreamIndex, descrBuffer,
                                                         descriptorLen, codecReader, lang, isSecondary);
    }
    else if (codecName == "A_LPCM")
    {
        m_pmt.pidList[tsStreamIndex] = PMTStreamInfo(StreamType::AUDIO_LPCM, tsStreamIndex, descrBuffer, descriptorLen,
                                                     codecReader, lang, isSecondary);
    }
    else if (codecName == "A_MLP")
    {
        m_pmt.pidList[tsStreamIndex] = PMTStreamInfo(StreamType::AUDIO_TRUE_HD, tsStreamIndex, descrBuffer,
                                                     descriptorLen, codecReader, lang, isSecondary);
        if (const auto mergeReader = dynamic_cast<TrueHDAC3MergeReader*>(codecReader))
            mergeReader->setNewStyleAudioPES(m_useNewStyleAudioPES);
    }
    else if (codecName == "A_DTS")
    {
        const auto dtsReader = dynamic_cast<DTSStreamReader*>(codecReader);
        dtsReader->setNewStyleAudioPES(m_useNewStyleAudioPES);
        StreamType audioType;
        if (dtsReader->getDTSHDMode() != DTSStreamReader::DTSHD_SUBTYPE::DTS_SUBTYPE_UNINITIALIZED &&
            !dtsReader->getDownconvertToDTS())
        {
            // if (dtsReader->getDTSHDMode() == DTSStreamReader::DTS_SUBTYPE_EXPRESS)
            if (dtsReader->isSecondary())
                audioType = StreamType::AUDIO_DTS_HD_SECONDARY;
            else if (dtsReader->getDTSHDMode() == DTSStreamReader::DTSHD_SUBTYPE::DTS_SUBTYPE_MASTER_AUDIO)
                audioType = StreamType::AUDIO_DTS_HD_MA;
            else
                audioType = StreamType::AUDIO_DTS_HD;
        }
        else
        {
            audioType = StreamType::AUDIO_DTS;
        }
        m_pmt.pidList[tsStreamIndex] =
            PMTStreamInfo(audioType, tsStreamIndex, descrBuffer, descriptorLen, codecReader, lang, isSecondary);
    }
    else if (codecName == "A_OPUS")
    {
        // ETSI TS 101 154: Opus uses stream_type 0x06 (PRIVATE_DATA) with
        // registration_descriptor 'Opus' and opus_audio_descriptor in the PMT.
        m_pmt.pidList[tsStreamIndex] = PMTStreamInfo(StreamType::PRIVATE_DATA, tsStreamIndex, descrBuffer,
                                                     descriptorLen, codecReader, lang, isSecondary);
    }
    else
        THROW(ERR_UNKNOWN_CODEC, "Unsupported codec " << codecName << " for TS/M2TS muxing.")

    // cache the downcasts once per stream; muxPacket and writePESPacket run per
    // AVPacket/PES and previously paid a dynamic_cast each time
    m_streamInfo[tsStreamIndex].m_mpegReader = dynamic_cast<MPEGStreamReader*>(codecReader);
    m_streamInfo[tsStreamIndex].m_definesDuration = isPrimaryVideo;
    m_streamInfo[tsStreamIndex].m_steadyPace = codecName[0] == 'V' || codecName[0] == 'A';
    const auto itPid = m_pmt.pidList.find(tsStreamIndex);
    if (itPid != m_pmt.pidList.end())
    {
        itPid->second.m_mpegReader = dynamic_cast<MPEGStreamReader*>(codecReader);
        itPid->second.m_audioReader = dynamic_cast<SimplePacketizerReader*>(codecReader);
        // Carried to the playlist and the clip info, which used to decide this for themselves from the
        // presence of Dolby Vision and so described a single layer stream as an enhancement layer: no
        // primary video entry at all, and a subpath for a layer that is not there.
        itPid->second.isDvEnhancement = isDvEnhancement;

        // A single layer Dolby Vision stream may have no picture for a player that cannot read it.
        // Asked here because this is where such a stream is known to BE single layer, and because the
        // descriptor path that computes the profile is never reached by a Blu-ray mux at all.
        if (!isDvEnhancement && codecReader != nullptr && codecReader->getStreamHDR() == 4)
        {
            if (const auto hevcReader = dynamic_cast<HEVCStreamReader*>(codecReader); hevcReader != nullptr)
                hevcReader->warnIfDvHasNoFallback();
        }
    }

    m_streamInfo[DEFAULT_PCR_PID].m_tsCnt = 0;
    buildNULL();
    buildPAT();
    buildPMT();
}

bool TSMuxer::doFlush()
{
    if (!m_lastPts.empty())
        m_lastPts[m_lastPts.size() - 1] += m_additionCLPISize;

    int64_t newPCR = 0;
    if (m_m2tsMode)
    {
        newPCR = (m_endStreamDTS - m_minDts) / INT_FREQ_TO_TS_FREQ + m_fixed_pcr_offset;
        if (m_cbrBitrate != -1 && m_lastPCR != -1)
        {
            const auto cbrPCR = llround(static_cast<double>(m_lastPCR + m_pcrBits) * 90000.0 / m_cbrBitrate);
            newPCR = FFMAX(newPCR, cbrPCR);
        }
    }
    return doFlush(newPCR, 0);
}

bool TSMuxer::doFlush(const int64_t newPCR, const int64_t pcrGAP)
{
    flushTSFrame();

    if (m_sectorSize > 0)
    {
        // round file to sector size. It's also can be applied to any file in a m2ts mode
        int nullCnt = 0;
        if (m_processedBlockSize % m_sectorSize)
            nullCnt = m_sectorSize - static_cast<int>(m_processedBlockSize % m_sectorSize);
        writeNullPackets(nullCnt / m_frameSize);
    }

    if (m_m2tsMode)
        processM2TSPCR(pcrForMaxRate(newPCR, 0, pcrGAP), pcrGAP);

    const int lastBlockSize = m_outBufLen & (MuxerManager::PHYSICAL_SECTOR_SIZE - 1);  // last 64K of data
    const int roundBufLen = m_outBufLen & ~(MuxerManager::PHYSICAL_SECTOR_SIZE - 1);
    if (m_owner->isAsyncMode())
    {
        if (lastBlockSize > 0)
        {
            assert(m_sectorSize == 0);  // we should not be here in interleaved mode!
            const auto newBuff = new uint8_t[lastBlockSize];
            memcpy(newBuff, m_outBuf + roundBufLen, lastBlockSize);
            m_owner->asyncWriteBuffer(this, m_outBuf, roundBufLen, m_muxFile);
            m_outBuf = newBuff;
            m_outBufCapacity = lastBlockSize;
        }
        else
        {
            m_owner->asyncWriteBuffer(this, m_outBuf, roundBufLen, m_muxFile);
            m_outBuf = nullptr;
            m_outBufCapacity = 0;
        }
    }
    else
    {
        m_owner->syncWriteBuffer(this, m_outBuf, roundBufLen, m_muxFile);
        memmove(m_outBuf, m_outBuf + roundBufLen, lastBlockSize);
    }
    m_outBufLen = lastBlockSize;
    assert(!m_sectorSize || m_outBufLen == 0);
    return true;
}

int TSMuxer::writeOutFile(const uint8_t* buffer, const int len) const
{
    const int rez = m_muxFile->write(buffer, len);
    return rez;
}

// ** NOTHING EVER COMPARED THE PACE OF THE STREAM AGAINST THE READ RATE OF THE DISC. **
//
// The muxer paces the transport stream to whatever the source asks for, instant by instant. When
// the source asks for more than a drive is required to supply, the disc is still written, still
// reported as complete, and still plays perfectly on a computer, where the file comes off a hard
// disk that has no such limit. On a standalone player it can stutter or refuse to start.
//
// Measured on three streams of the same 12 seconds, muxed to Blu-ray:
//
//   18.8 Mbit/s average, peaks held down    no packet under the floor
//   29.2 Mbit/s average, peaks left free    12.3 per cent under it, fastest 66.8 Mbit/s
//   83.3 Mbit/s average                     99.1 per cent under it, fastest 139.2 Mbit/s
//
// The middle one is the case worth naming: its average sits well inside the limit and it still
// asks for half as much again as the drive has to give, in bursts. That is what an ordinary
// encode looks like when its peak bitrate was never capped.
//
// This says so, and it does not change a byte of the output.
//
// ** IT IS NOT ALWAYS THE SOURCE. ** An earlier version of this warning said the rate came from the
// source and not from muxing, and sent people off to re-encode. Measured on a pressed HD Blu-ray
// whose own packets never go above the limit, 846 ticks and 0 of 2,399,999 over: remuxing two
// minutes of it from its own playlist asks for 92.5 Mbit/s, with 28.2 per cent over. The muxer
// computes arrival times from its own model rather than preserving the pace the disc was authored
// at, so a perfectly conformant source can come out over the limit.
//
// --maxbitrate holds it: the same mux capped at 48,000 comes out at exactly 846 ticks with nothing
// over, and the file is 0.01 per cent smaller. Only when the video's own peak exceeds the limit is
// a re-encode the answer, and the warning now says both.
void TSMuxer::reportReadRate() const
{
    if (!m_bluRayMode || m_atsPackets == 0 || m_minAtsGap <= 0 || m_atsOverLimit == 0)
        return;

    const double limit = isV3() ? BD_READ_RATE_UHD : BD_READ_RATE_HD;
    const double fastest = atsGapToRate(m_minAtsGap) / 1e6;
    const double share = 100.0 * static_cast<double>(m_atsOverLimit) / static_cast<double>(m_atsPackets);

    // A share that rounds to 0.0 is still above nothing: "0.0 per cent" read as "none" in a warning.
    const std::string shareText = share < 0.05 ? std::string("less than 0.1") : doubleToStr(share, 1);

    std::ostringstream msg;
    msg << "Warning: this disc asks to be read at " << doubleToStr(fastest, 1) << " Mbit/s at its fastest, and "
        << shareText << " per cent of its packets are above the limit. A " << (isV3() ? "UHD" : "Blu-ray")
        << " drive only has to supply " << doubleToStr(limit / 1e6, 0)
        << " Mbit/s. It will play from a hard disk, where the rate does not matter, and may stutter or refuse "
           "to start on a standalone player. Add --maxbitrate="
        << doubleToStr(limit / 1e3, 0)
        << " to hold it at the limit: the muxer then paces the disc to that rate, which is what a pressed "
           "disc does. If the video's own peak is already above the limit the muxer cannot invent room for "
           "it, and only a re-encode with the peak bitrate capped will help.";
    LTRACE(LT_WARN, 2, msg.str());
}

static bool isVideoStreamType(const StreamType streamType)
{
    switch (streamType)
    {
    case StreamType::VIDEO_MPEG1:
    case StreamType::VIDEO_MPEG2:
    case StreamType::VIDEO_MPEG4:
    case StreamType::VIDEO_H264:
    case StreamType::VIDEO_MVC:
    case StreamType::VIDEO_VC1:
    case StreamType::VIDEO_H265:
    case StreamType::VIDEO_H266:
        return true;
    default:
        return false;
    }
}

// With --maxbitrate the data is held to the cap and never sent ahead, so where the video's own peaks are above the cap
// its pictures reach the player after they are due, and a player stutters or freezes there. Nothing said so: on a
// 120 Mbit/s HEVC video, with --maxbitrate=150000 141 of 900 pictures come up to 0.3 s late, and with 109000 862 of
// 900 up to 5.9 s. A picture counts as late when its last packet arrives, at the cap, after its decoding time.
void TSMuxer::noteLateArrival()
{
    if (m_maxBitrate == -1 || m_lastPCR == -1 || m_lastPESDTS == -1)
        return;
    const auto it = m_pmt.pidList.find(m_pesPID);
    if (it == m_pmt.pidList.end() || !isVideoStreamType(it->second.m_streamType))
        return;
    m_cappedPictures++;
    const int64_t arrival =
        m_lastPCR + static_cast<int64_t>(std::ceil(static_cast<double>(m_pcrBits) * 90000.0 / m_maxBitrate));
    if (arrival > m_lastPESDTS)
    {
        m_latePictures++;
        m_worstLateness = (std::max)(m_worstLateness, arrival - m_lastPESDTS);
    }
}

void TSMuxer::reportLateArrival() const
{
    if (m_latePictures == 0)
        return;
    LTRACE(LT_WARN, 2,
           "Warning: with --maxbitrate=" << m_maxBitrate / 1000 << ", " << m_latePictures << " of " << m_cappedPictures
                                         << " video pictures arrive after they are due, up to "
                                         << doubleToStr(static_cast<double>(m_worstLateness) / 90000.0, 1)
                                         << " s late. The video's peaks are above the cap, so a player may stutter or "
                                            "freeze there. Re-encode the video with its peak bitrate capped below the "
                                            "cap, or raise the cap.");
}

bool TSMuxer::close()
{
    reportReadRate();
    reportLateArrival();

    if (m_isExternalFile)
        return true;

    if (m_sectorSize)
    {
        assert(m_outBufLen == 0 && m_muxFile->size() % m_sectorSize == 0);
    }

    if (!m_muxFile->close())
        return false;

    if (m_sectorSize)
        return true;

    if (m_outBufLen > 0)
    {
        if (!m_muxFile->open(m_outFileName.c_str(), AbstractOutputStream::ofWrite + AbstractOutputStream::ofAppend))
            return false;
        if (!writeOutFile(m_outBuf, m_outBufLen))
            return false;
    }
    const bool bRes = m_muxFile->close();
    return bRes;
}

int TSMuxer::calcM2tsFrameCnt() const
{
    int32_t byteCnt = 0;
    for (const auto& i : m_m2tsDelayBlocks) byteCnt += i.second;
    byteCnt -= m_prevM2TSPCROffset;
    byteCnt += m_outBufLen;
    assert(byteCnt % 192 == 0);
    return byteCnt / 192;
}

// The arrival timestamp pass spreads every packet gathered since the previous pass evenly across
// the span from that pass to this PCR, so the gap each packet is given is that span divided by
// that count. A ceiling on the read rate is a floor under that gap, and this is the only place
// where both numbers are known: the count is not final until the tables, any padding and the PCR
// packet itself have been written. An earlier attempt counted bits since the last PCR instead and
// was short by the two to four packets those add, every interval.
//
// extraFrames is what the caller is about to add and has not added yet.
int64_t TSMuxer::pcrForMaxRate(const int64_t newPCR, const int64_t extraFrames, const int64_t pcrGAP) const
{
    if (m_lastPCR == -1)
        return newPCR;
    if (m_maxBitrate == -1)
    {
        // Without --maxbitrate nothing holds the rate, and the muxer's own tables could come out as a burst: when
        // they are written right after a PCR, PAT, PMT, SIT and the next PCR packet share a span of a few
        // microseconds, four packets 148 ticks apart, 274 Mbit/s, every few seconds on every Blu-ray. A pressed
        // disc never goes below the read-rate floor even there. Such a short pass is now held at the floor the
        // way --maxbitrate holds every pass, which moves its PCR later by at most a few hundred ticks of the
        // 27 MHz clock. A pass that carries the streams is left exactly as it was.
        if (!m_m2tsMode || !m_bluRayMode)
            return newPCR;
        const int64_t frames = calcM2tsFrameCnt() + extraFrames;
        if (frames <= 0 || frames > TABLE_BURST_PACKETS)
            return newPCR;
        const double floorGap = atsGapFloor(isV3() ? BD_READ_RATE_UHD : BD_READ_RATE_HD);
        const int64_t needed = m_prevM2TSPCR + pcrGAP + static_cast<int64_t>(std::ceil(frames * floorGap));
        return FFMAX(newPCR, (needed + 299) / 300);  // back to the 90 kHz clock, never downwards
    }
    // A plain transport stream carries no arrival timestamps, so there is no per packet gap to
    // hold. What the ceiling means there is that the clock may not run ahead of the data: the
    // bits written since the last PCR take a known time to arrive at the rate asked for.
    if (!m_m2tsMode)
        return FFMAX(newPCR, m_lastPCR + static_cast<int64_t>(std::ceil(m_pcrBits * 90000.0 / m_maxBitrate)));
    const int64_t frames = calcM2tsFrameCnt() + extraFrames;
    if (frames <= 0)
        return newPCR;
    const int64_t needed = m_prevM2TSPCR + pcrGAP + static_cast<int64_t>(std::ceil(frames * atsGapFloor(m_maxBitrate)));
    const int64_t minPCR = (needed + 299) / 300;  // back to the 90 kHz clock, never downwards
    return FFMAX(newPCR, minPCR);
}

void TSMuxer::processM2TSPCR(const int64_t pcrVal, const int64_t pcrGAP)
{
    const int m2tsFrameCnt = calcM2tsFrameCnt();
    const int64_t hiResPCR = pcrVal * 300 - pcrGAP;
    const int64_t pcrValDif = hiResPCR - m_prevM2TSPCR;  // m2ts pcr clock based on full 27Mhz counter
    const double pcrIncPerFrame = static_cast<double>(pcrValDif) / m2tsFrameCnt;

    // Every packet written in this pass is given the same gap, so one comparison covers all of
    // them. Nothing here changes what is written; it only records the rate the disc is asking to
    // be read at, which nothing measured before.
    //
    // A pass of a handful of packets is the muxer's own tables written together with a PCR: PAT,
    // PMT, SIT and the PCR packet, four packets 148 ticks apart, which works out at 274 Mbit/s.
    // No source causes that and no option changes it, and a drive's buffer takes four packets
    // without effort. Yet it was what the warning quoted as the fastest rate, and on a 3D disc
    // whose video never went over the limit it alone made a warning that said "0.0 per cent".
    // Such passes no longer count; a pass that carries the streams themselves still does.
    if (m2tsFrameCnt > TABLE_BURST_PACKETS && pcrIncPerFrame > 0)
    {
        if (m_minAtsGap == 0 || pcrIncPerFrame < m_minAtsGap)
            m_minAtsGap = pcrIncPerFrame;
        m_atsPackets += m2tsFrameCnt;
        if (pcrIncPerFrame < atsGapFloor(isV3() ? BD_READ_RATE_UHD : BD_READ_RATE_HD))
            m_atsOverLimit += m2tsFrameCnt;
    }

    auto curM2TSPCR = static_cast<double>(m_prevM2TSPCR);
    uint8_t* curPos;
    if (!m_m2tsDelayBlocks.empty())
    {
        int offset = m_prevM2TSPCROffset;
        for (const auto& i : m_m2tsDelayBlocks)
        {
            curPos = i.first + offset;
            int j = offset;
            for (; j < i.second; j += 192)
            {
                curM2TSPCR += pcrIncPerFrame;
                writeM2TSHeader(curPos, llround(curM2TSPCR));
                curPos += 192;
            }
            if (m_owner->isAsyncMode())
                m_owner->asyncWriteBuffer(this, i.first, i.second, m_muxFile);
            else
            {
                m_owner->syncWriteBuffer(this, i.first, i.second, m_muxFile);
                delete[] i.first;
            }
            offset = j - i.second;
        }
        m_prevM2TSPCROffset = offset;
        m_m2tsDelayBlocks.clear();
    }
    curPos = m_outBuf + m_prevM2TSPCROffset;
    const uint8_t* end = m_outBuf + m_outBufLen;
    for (; curPos < end; curPos += 192)
    {
        curM2TSPCR += pcrIncPerFrame;
        writeM2TSHeader(curPos, llround(curM2TSPCR));
    }
    assert(curPos == end);
    m_prevM2TSPCROffset = m_outBufLen;
    // assert((int64_t) curM2TSPCR == hiResPCR);
    m_prevM2TSPCR = hiResPCR;
}

// The continuity counter of a packet that carries only a PCR. A packet without payload repeats the counter of the last
// packet of its PID (ISO/IEC 13818-1, 2.4.3.3). m_tsCnt holds the counter of the NEXT packet, so a PCR packet on the
// video PID took the counter of the payload packet after it, and every PCR cost two continuity errors there. Without
// video and without --no-pcr-on-video-pid the PCR goes out on PID 0, whose packets count with m_patCnt. A PCR PID of
// its own carries no payload at all and keeps the counter it always had.
int TSMuxer::pcrOnlyPacketCounter()
{
    const int pid = m_pmt.pcr_pid;
    if (pid == DEFAULT_PCR_PID)
        return m_streamInfo[pid].m_tsCnt & 15;
    if (pid == 0)
        return (m_patCnt - 1) & 15;
    return (m_streamInfo[pid].m_tsCnt - 1) & 15;
}

void TSMuxer::writeEmptyPacketWithPCRTest(const int64_t pcrVal)
{
    if (m_m2tsMode)
    {
        m_outBufLen += 4;
        m_processedBlockSize += 4;
        m_pcrBits += 4 * 8;
    }
    uint8_t* curBuf = m_outBuf + m_outBufLen;
    const auto tsPacket = reinterpret_cast<TSPacket*>(curBuf);
    const auto initTS = UnalignedPtr<uint32_t>(curBuf);
    *initTS = TSPacket::TS_FRAME_SYNC_BYTE;
    initTS[1] = 0x07 + TSPacket::PCR_BIT_VAL;
    tsPacket->setPID(m_pmt.pcr_pid);
    tsPacket->afExists = 1;
    tsPacket->adaptiveField.setPCR33(pcrVal);
    tsPacket->counter = pcrOnlyPacketCounter();

    memset(curBuf + tsPacket->getHeaderSize(), 0xff, TS_FRAME_SIZE - tsPacket->getHeaderSize());
    tsPacket->adaptiveField.length += TS_FRAME_SIZE - tsPacket->getHeaderSize();
    m_outBufLen += TS_FRAME_SIZE;
    m_processedBlockSize += TS_FRAME_SIZE;
    m_pcrBits += TS_FRAME_SIZE * 8;
    m_muxedPacketCnt[m_muxedPacketCnt.size() - 1]++;
}

void TSMuxer::writeEmptyPacketWithPCR(const int64_t pcrVal)
{
    if (m_m2tsMode)
    {
        m_outBufLen += 4;
        m_processedBlockSize += 4;
        m_pcrBits += 4 * 8;
    }
    uint8_t* curBuf = m_outBuf + m_outBufLen;
    const auto tsPacket = reinterpret_cast<TSPacket*>(curBuf);
    const auto initTS = UnalignedPtr<uint32_t>(curBuf);
    *initTS = TSPacket::TS_FRAME_SYNC_BYTE;
    initTS[1] = 0x07 + TSPacket::PCR_BIT_VAL;
    tsPacket->setPID(m_pmt.pcr_pid);
    tsPacket->afExists = 1;
    tsPacket->adaptiveField.setPCR33(pcrVal);
    tsPacket->counter = pcrOnlyPacketCounter();

    memset(curBuf + tsPacket->getHeaderSize(), 0xff, TS_FRAME_SIZE - tsPacket->getHeaderSize());
    tsPacket->adaptiveField.length += TS_FRAME_SIZE - tsPacket->getHeaderSize();
    m_outBufLen += TS_FRAME_SIZE;
    m_processedBlockSize += TS_FRAME_SIZE;
    m_pcrBits += TS_FRAME_SIZE * 8;
    m_muxedPacketCnt[m_muxedPacketCnt.size() - 1]++;
    if (m_m2tsMode)
        processM2TSPCR(pcrVal, 0);
    writeOutBuffer();
}

void TSMuxer::buildPesHeader(const uint8_t pesStreamID, AVPacket& avPacket, int pid)
{
    const int64_t curDts = internalClockToPts(avPacket.dts) + m_timeOffset;
    const int64_t curPts = internalClockToPts(avPacket.pts) + m_timeOffset;
    // no zero-init: serialize/writePESExtension/writeAdditionData write every byte that
    // is later copied (bufLen), and this runs once per PES (1200/s for TrueHD)
    uint8_t tmpBuffer[2048];
    const auto pesPacket = reinterpret_cast<PESPacket*>(tmpBuffer);
    if (curDts != curPts)
        pesPacket->serialize(curPts, curDts, pesStreamID);
    else
        pesPacket->serialize(curPts, pesStreamID);
    m_lastPESDTS = curDts;
    m_fullPesDTS = avPacket.dts;
    m_fullPesPTS = avPacket.pts;
    pesPacket->flagsHi |= PES_DATA_ALIGNMENT;
    // int additionDataSize = avPacket.codec->writePESExtension(pesPacket);
    // AbstractStreamReader is the only BaseAbstractStreamReader subclass, so the
    // per-PES dynamic_cast here was avoidable
    const auto ast = static_cast<AbstractStreamReader*>(avPacket.codec);
    if (ast)
        ast->writePESExtension(pesPacket, avPacket);
    if (avPacket.flags & AVPacket::IS_COMPLETE_FRAME)
    {
        // The PES packet length is 16 bits, and only video may leave it open. A longer packet was given a length
        // that had wrapped round, and the stream after it was read from the wrong place. It is refused instead: a
        // subtitle segment of nearly 64 KB is the one way to get here.
        const int pesLength = avPacket.size + pesPacket->getHeaderLength() - 6;
        if (pesLength > 0xFFFF)
            THROW(ERR_COMMON, "A packet of " << avPacket.size << " bytes on track " << avPacket.stream_index
                                             << " does not fit in one PES packet of a transport stream (at most "
                                             << 0xFFFF - (pesPacket->getHeaderLength() - 6)
                                             << " bytes), so this track cannot be muxed to a transport stream or a "
                                                "disc. It can be muxed to Matroska.")
        pesPacket->setPacketLength(avPacket.size + pesPacket->getHeaderLength());
    }

    PriorityDataInfo tmpPriorityData;
    const int additionDataSize = avPacket.codec->writeAdditionData(
        tmpBuffer + pesPacket->getHeaderLength(), tmpBuffer + sizeof(tmpBuffer), avPacket, &tmpPriorityData);
    const int bufLen = pesPacket->getHeaderLength() + additionDataSize;
    m_pesData.resize(bufLen);
    memcpy(m_pesData.data(), tmpBuffer, bufLen);
    for (auto& i : tmpPriorityData) m_priorityData.emplace_back(i.first + pesPacket->getHeaderLength(), i.second);
}

// The length of a PES packet is 16 bits, and only video may leave it open. Packets that arrive whole are checked in
// buildPesHeader; a PGS segment arrives in pieces and reached the stream with an open length, which a player reads
// wrongly. It is refused with the same message. A subtitle segment of nearly 64 KB is the one way to get here. Only
// audio and subtitle streams are refused: a video stream may leave the length open, and that includes streams that
// are not typed as video, such as the enhancement layer of a Dolby Vision video.
static bool isAudioOrSubtitleStreamType(const StreamType streamType)
{
    switch (streamType)
    {
    case StreamType::AUDIO_LPCM:
    case StreamType::AUDIO_AC3:
    case StreamType::AUDIO_DTS:
    case StreamType::AUDIO_TRUE_HD:
    case StreamType::AUDIO_EAC3:
    case StreamType::AUDIO_DTS_HD:
    case StreamType::AUDIO_DTS_HD_MA:
    case StreamType::AUDIO_DRA:
    case StreamType::AUDIO_DRA_EXT:
    case StreamType::AUDIO_EAC3_SECONDARY:
    case StreamType::AUDIO_DTS_HD_SECONDARY:
    case StreamType::AUDIO_MPEG1:
    case StreamType::AUDIO_MPEG2:
    case StreamType::AUDIO_AAC:
    case StreamType::AUDIO_AAC_RAW:
    case StreamType::AUDIO_MHAS_MAIN:
    case StreamType::AUDIO_MHAS_AUX:
    case StreamType::SUB_PGS:
    case StreamType::SUB_IGS:
    case StreamType::SUB_TGS:
        return true;
    default:
        return false;
    }
}

void TSMuxer::refuseLongNonVideoPes() const
{
    const auto it = m_pmt.pidList.find(m_pesPID);
    if (it == m_pmt.pidList.end() || !isAudioOrSubtitleStreamType(it->second.m_streamType))
        return;
    const int64_t pesLength = static_cast<int64_t>(m_pesData.size()) - 6;
    THROW(ERR_COMMON, "A packet of " << pesLength << " bytes on the stream with PID " << m_pesPID
                                     << " does not fit in one PES packet of a transport stream (at most 65535 bytes), "
                                        "so this track cannot be muxed to a transport stream or a disc. It can be "
                                        "muxed to Matroska.")
}

void TSMuxer::addData(const uint8_t pesStreamID, const int pid, AVPacket& avPacket)
{
    if (m_inplacePending)
    {
        // the PES already streams straight into m_outBuf
        emitInplacePayload(avPacket.data, avPacket.size, (avPacket.flags & AVPacket::PRIORITY_DATA) != 0);
        return;
    }
    int beforePesLen = static_cast<int>(m_pesData.size());
    if (m_pesData.size() == 0)
    {
        buildPesHeader(pesStreamID, avPacket, pid);
        m_pesPID = pid;
        m_pesIFrame = avPacket.flags & AVPacket::IS_IFRAME;
        m_pesSpsPps = avPacket.flags & AVPacket::IS_SPS_PPS_IN_GOP;
    }
    const int oldLen = static_cast<int>(m_pesData.size());
    const int pesHeaderLen = oldLen - beforePesLen;
    if (oldLen > 100000000)
        THROW(ERR_COMMON, "Pes packet len too large ( >100Mb). Bad stream or invalid codec speciffed.")
    m_pesData.grow(avPacket.size /*+ additionDataSize*/);
    uint8_t* dst = m_pesData.data() + oldLen;
    // memcpy(dst, tmpBuffer, additionDataSize);
    if (avPacket.size > 0)
        memcpy(dst /*+ additionDataSize*/, avPacket.data, avPacket.size);
    if (avPacket.flags & AVPacket::PRIORITY_DATA)
    {
        if (!m_priorityData.empty() && m_priorityData.rbegin()->first + m_priorityData.rbegin()->second == beforePesLen)
            m_priorityData.rbegin()->second += avPacket.size + pesHeaderLen;
        else
            m_priorityData.emplace_back(beforePesLen, avPacket.size + pesHeaderLen);
    }

    // 0x10000 + 6 is the largest accumulation whose PES length field still gets patched
    // at flush time; anything bigger keeps length 0 and can leave the staging buffer now
    static constexpr int64_t INPLACE_SWITCH_MIN = 0x10000 + 6 + 1;
    if (static_cast<int64_t>(m_pesData.size()) - 6 > 0xFFFF)
        refuseLongNonVideoPes();
    if (static_cast<int64_t>(m_pesData.size()) >= INPLACE_SWITCH_MIN && inplaceEligible())
        switchToInplace();
}

void TSMuxer::flushTSFrame() { writePESPacket(); }

void TSMuxer::writePATPMT(const int64_t pcr, const bool force)
{
    if (pcr == -1 || pcr - m_lastPMTPCR >= m_patPmtDelta || force)
    {
        m_lastPMTPCR = pcr != -1 ? pcr : m_fixed_pcr_offset;
        writePAT();
        writePMT();
        if (force || pcr - m_lastSITPCR > SIT_INTERVAL)
        {
            m_lastSITPCR = pcr;
            writeSIT();
        }
    }
}

bool TSMuxer::isSplitPoint(const AVPacket& avPacket) const
{
    if (avPacket.stream_index != m_mainStreamIndex || !(avPacket.flags & AVPacket::IS_IFRAME))
        return false;

    if (m_splitSize > 0)
    {
        // 64 bit on purpose: the packet count is a uint32_t and the frame size an int, so the
        // product was worked out in 32 bit arithmetic and wrapped once a part passed 4 GB. Widening
        // m_splitSize alone would not have been enough, because any split size above 4 GB is
        // compared against a size that starts counting from zero again at that point.
        return static_cast<int64_t>(m_muxedPacketCnt[m_muxedPacketCnt.size() - 1]) * m_frameSize > m_splitSize;
    }
    if (m_splitDuration > 0)
    {
        return (avPacket.pts - m_curFileStartPts) > m_splitDuration;
    }
    if (m_splitByChapters && m_nextChapter < m_chapterPts.size())
    {
        // Measured from the first packet of the mux, not from the start of the current part: a
        // chapter is a position in the film, and the parts before it have already been written.
        // m_firstPts[0] is that first packet, and it is not zero, because a transport stream is
        // given an offset before anything is muxed.
        if (m_firstPts.empty() || m_firstPts[0] == -1)
            return false;
        return avPacket.pts - m_firstPts[0] >= m_chapterPts[m_nextChapter];
    }

    return false;
}

// The chapter list arrives in seconds from the start of the film, the same list the Matroska muxer
// writes and the same one a Blu-ray playlist's marks produce. Kept here as internal clock ticks so
// that isSplitPoint can compare them against a packet's pts without dividing on every packet.
//
// A mark at zero is dropped: every file already starts at its own beginning, and splitting there
// would write an empty first part. Duplicates go the same way, because two marks on one frame can
// only produce one split.
void TSMuxer::setChapters(const std::vector<double>& chapters)
{
    m_chapterPts.clear();
    m_nextChapter = 0;
    for (const double seconds : chapters)
    {
        if (seconds <= 0.0)
            continue;
        m_chapterPts.push_back(static_cast<int64_t>(seconds * INTERNAL_PTS_FREQ));
    }
    std::sort(m_chapterPts.begin(), m_chapterPts.end());
    m_chapterPts.erase(std::unique(m_chapterPts.begin(), m_chapterPts.end()), m_chapterPts.end());
}

int TSMuxer::getFirstFileNum() const
{
    const string fileName = extractFileName(m_origFileName);
    return strToInt32(fileName.c_str());
}

std::string TSMuxer::getNextName(const std::string curName)
{
    std::string result = curName;
    if (m_bluRayMode)
    {
        const string fileExt = extractFileExt(m_origFileName);
        const string fileName = extractFileName(m_origFileName);
        const string filePath = extractFilePath(m_origFileName);
        int origNum = strToInt32(fileName.c_str());
        origNum += m_curFileNum;
        m_curFileNum++;
        result = closeDirPath(filePath) + strPadLeft(int32ToStr(origNum), 5, '0') + "." + fileExt;
    }
    else if (isSplitting())
    {
        m_curFileNum++;
        const string fileExt = extractFileExt(m_origFileName);
        result = m_origFileName.substr(0, m_origFileName.size() - fileExt.size() - 1) + ".split." +
                 int32ToStr(m_curFileNum) + "." + fileExt;
    }
    return toNativeSeparators(result);
}

void TSMuxer::gotoNextFile(const int64_t newPts)
{
    // Step past every chapter this part has already reached. More than one can fall inside a
    // single part when two marks sit closer together than the next key frame, and stepping past
    // only one would split again immediately on the next frame.
    if (m_splitByChapters && !m_firstPts.empty() && m_firstPts[0] != -1)
        while (m_nextChapter < m_chapterPts.size() && newPts - m_firstPts[0] >= m_chapterPts[m_nextChapter])
            m_nextChapter++;

    // 2. CloseCurrentFile
    if (m_owner->isAsyncMode())
        m_owner->waitForWriting();

    m_muxFile->close();
    assert(m_outBufLen == 0);

    // 3. open new file
    m_outFileName = getNextName(m_outFileName);
    if (m_sublingMuxer)
        m_outFileName = getNextName(m_outFileName);  // inc by 2
    m_fileNames.push_back(m_outFileName);
    openDstFile();

    for (auto& [pid, si] : m_pmt.pidList)
    {
        PMTStreamInfo& pmtInfo = si;
        pmtInfo.m_codecReader->onSplitEvent();
        if (pmtInfo.m_index.empty())
            continue;
        pmtInfo.m_index.emplace_back();
    }

    m_muxedPacketCnt.push_back(0);
    m_interleaveInfo.emplace_back();
    m_interleaveInfo.rbegin()->push_back(0);

    m_lastPts[m_lastPts.size() - 1] = newPts;  //(curPts-FIXED_PTS_OFFSET)*INT_FREQ_TO_TS_FREQ;
    m_firstPts.push_back(newPts);              //((curPts-FIXED_PTS_OFFSET)*INT_FREQ_TO_TS_FREQ);
    m_lastPts.push_back(newPts);               //((curPts-FIXED_PTS_OFFSET)*INT_FREQ_TO_TS_FREQ);
    m_curFileStartPts = newPts;
}

void TSMuxer::ensureOutBufSpace(const int needed)
{
    if (m_outBufLen + needed <= m_outBufCapacity)
        return;
    // Grow in int64 and refuse rather than wrap. m_outBufLen and the capacity are int, so a
    // runaway PES (a raw file muxed with the wrong codec, say) would otherwise double past
    // INT_MAX, wrap negative, then to zero, and this loop would never terminate.
    int64_t newCap = m_outBufCapacity > 0 ? m_outBufCapacity : m_writeBlockSize + 1024;
    const int64_t want = static_cast<int64_t>(m_outBufLen) + needed;
    while (newCap < want) newCap *= 2;
    if (newCap > INT32_MAX - 1024)
        THROW(ERR_COMMON, "Pes packet len too large (>2Gb) . Bad stream or invalid codec speciffed")
    const auto newBuf = new uint8_t[newCap];
    memcpy(newBuf, m_outBuf, m_outBufLen);
    delete[] m_outBuf;
    m_outBuf = newBuf;
    m_outBufCapacity = newCap;
}

// One TS packet of the pending PES, replicating the writeTSFrames packet body exactly.
// payloadLen < 184 produces the segment-final stuffed packet.
void TSMuxer::emitInplacePacket(const uint8_t* payload, const int payloadLen, const bool priority)
{
    // addData's ">100Mb" sanity check is skipped once a PES is pending (it returns early), so
    // apply the same limit here or a broken stream grows m_outBuf without bound instead of
    // getting the diagnosis the check exists to give.
    if (m_outBufLen - m_pendingStartOffset > 100 * 1024 * 1024)
        THROW(ERR_COMMON, "Pes packet len too large ( >100Mb). Bad stream or invalid codec speciffed.")
    insertHeldPCRIfDue();
    ensureOutBufSpace(m_frameSize);
    if (m_m2tsMode)
    {
        m_outBufLen += 4;
        m_processedBlockSize += 4;
        m_pcrBits += 4 * 8;
    }
    const auto initTS = UnalignedPtr<uint32_t>(m_outBuf + m_outBufLen);
    *initTS = TSPacket::TS_FRAME_SYNC_BYTE + TSPacket::DATA_EXIST_BIT_VAL;
    const auto tsPacket = reinterpret_cast<TSPacket*>(m_outBuf + m_outBufLen);
    int64_t capacity = TS_FRAME_SIZE - tsPacket->getHeaderSize();
    tsPacket->setPID(m_pesPID);
    tsPacket->counter = m_streamInfo[m_pesPID].m_tsCnt++;
    tsPacket->payloadStart = m_pendingPayloadStart;
    m_pendingPayloadStart = false;
    tsPacket->priority = priority;
    if (payloadLen < capacity)
    {
        if (!tsPacket->afExists)
        {
            tsPacket->afExists = 1;
            if (capacity - payloadLen == 1)
            {
                tsPacket->adaptiveField.length = 0;
                capacity--;
            }
            else
            {
                initTS[1] = 0x01;  // zero all af flags, set af len to 1.
                capacity -= 2;
            }
        }
        memset(reinterpret_cast<uint8_t*>(tsPacket) + tsPacket->getHeaderSize(), 0xff, capacity - payloadLen);
        tsPacket->adaptiveField.length += static_cast<unsigned>(capacity - payloadLen);
        capacity = payloadLen;
    }
    memcpy(m_outBuf + m_outBufLen + tsPacket->getHeaderSize(), payload, capacity);
    m_outBufLen += TS_FRAME_SIZE;
    m_processedBlockSize += TS_FRAME_SIZE;
    m_pcrBits += TS_FRAME_SIZE * 8;
    m_muxedPacketCnt[m_muxedPacketCnt.size() - 1]++;
    m_pendingTsPackets++;
    // no writeOutBuffer here: flushing is barred while the PES is pending
}

void TSMuxer::inplaceCloseSegment()
{
    if (m_pendingTailLen > 0)
    {
        emitInplacePacket(m_pendingTail, m_pendingTailLen, m_pendingTailPriority);
        m_pendingTailLen = 0;
    }
}

// Stream payload bytes of the pending PES: full packets are emitted straight from the
// source, the sub-packet remainder waits in m_pendingTail for the next chunk.
void TSMuxer::emitInplacePayload(const uint8_t* data, int64_t len, const bool priority)
{
    if (priority != m_pendingTailPriority)
    {
        // a segment boundary: legacy emits each priority range with its own
        // writeTSFrames call, whose final partial packet is stuffed
        inplaceCloseSegment();
        m_pendingTailPriority = priority;
    }
    constexpr int PAYLOAD = TS_FRAME_SIZE - TSPacket::TS_HEADER_SIZE;  // 184
    if (m_pendingTailLen > 0)
    {
        const int fill = static_cast<int>((std::min)(static_cast<int64_t>(PAYLOAD - m_pendingTailLen), len));
        memcpy(m_pendingTail + m_pendingTailLen, data, fill);
        m_pendingTailLen += fill;
        data += fill;
        len -= fill;
        if (m_pendingTailLen < PAYLOAD)
            return;
        emitInplacePacket(m_pendingTail, PAYLOAD, priority);
        m_pendingTailLen = 0;
    }
    while (len >= PAYLOAD)
    {
        emitInplacePacket(data, PAYLOAD, priority);
        data += PAYLOAD;
        len -= PAYLOAD;
    }
    if (len > 0)
    {
        memcpy(m_pendingTail, data, len);
        m_pendingTailLen = static_cast<int>(len);
    }
}

// The staged accumulation is provably larger than a patchable PES (> 0xFFFF + 6), so its
// bytes can leave the staging buffer now; the rest of the PES streams straight through.
void TSMuxer::switchToInplace()
{
    // sanity: the segment model needs sorted, non-overlapping priority ranges. Legacy
    // can produce overlapping ones in exotic cases; those PES simply stay on the old path.
    int64_t prevEnd = 0;
    for (const auto& i : m_priorityData)
    {
        if (i.first < prevEnd || i.first + i.second > static_cast<int64_t>(m_pesData.size()))
            return;
        prevEnd = i.first + i.second;
    }

    m_inplacePending = true;
    m_midPesPCR = false;
    m_pendingStartOffset = m_outBufLen;
    m_pendingStartPktCnt = m_muxedPacketCnt[m_muxedPacketCnt.size() - 1];
    m_pendingTsPackets = 0;
    m_pendingPayloadStart = true;
    m_pendingTailLen = 0;
    m_pendingTailPriority = false;
    const auto pesPacket = reinterpret_cast<PESPacket*>(m_pesData.data());
    m_pendingPesHasPts = (pesPacket->flagsLo & 0x80) == 0x80;
    m_pendingPesPts = m_pendingPesHasPts ? pesPacket->getPts() : 0;

    const uint8_t* base = m_pesData.data();
    const int64_t total = static_cast<int64_t>(m_pesData.size());
    int64_t cur = 0;
    for (const auto& i : m_priorityData)
    {
        if (i.first > cur)
        {
            // non-priority run before the range: a closed segment
            emitInplacePayload(base + cur, i.first - cur, false);
            inplaceCloseSegment();
        }
        const bool openEnd = i.first + i.second == total;
        emitInplacePayload(base + i.first, i.second, true);
        if (!openEnd)
            inplaceCloseSegment();  // range followed by more data: it cannot grow anymore
        cur = i.first + i.second;
    }
    if (cur < total)
        emitInplacePayload(base + cur, total - cur, false);  // the open tail segment

    m_pesData.resize(0);
    m_priorityData.clear();
}

void TSMuxer::finalizeInplacePes()
{
    inplaceCloseSegment();

    // seek index bookkeeping, replicated from the staged path with the captured values
    PMTStreamInfo& streamInfo = m_pmt.pidList[m_pesPID];
    if (m_computeMuxStats && m_pendingPesHasPts)
    {
        const uint64_t curPts = m_pendingPesPts;
        if (streamInfo.m_index.empty())
            streamInfo.m_index.emplace_back();
        const auto vCodec = streamInfo.m_mpegReader;
        const bool SPSRequired = streamInfo.m_codecReader->needSPSForSplit();
        const auto aCodec = streamInfo.m_audioReader;
        bool updateIdx = false;
        if (vCodec && m_pesIFrame)
        {
            if (m_pesSpsPps || !SPSRequired)
            {
                PMTIndex& curIndex = *streamInfo.m_index.rbegin();
                if (curIndex.empty() || curPts > curIndex.rbegin()->first)
                {
                    curIndex.insert(std::make_pair(curPts, PMTIndexData(m_pendingStartPktCnt, 0)));
                    updateIdx = true;
                }
            }
            m_lastGopNullCnt = m_nullCnt;
        }
        else if (aCodec)
        {
            if (m_videoTrackCnt + m_videoSecondTrackCnt == 0)
                m_lastGopNullCnt = m_nullCnt;
            PMTIndex& curIndex = *streamInfo.m_index.rbegin();
            if (curIndex.empty() || curPts - curIndex.rbegin()->first >= 90000)
            {
                curIndex.insert(std::make_pair(curPts, PMTIndexData(m_pendingStartPktCnt, 0)));
                updateIdx = true;
            }
        }
        if (updateIdx)
        {
            PMTIndex& curIndex = *streamInfo.m_index.rbegin();
            assert(curIndex.rbegin()->second.m_frameLen == 0);
            curIndex.rbegin()->second.m_frameLen = (m_pendingTsPackets + 1) * m_frameSize;
        }
    }

    noteLateArrival();
    m_inplacePending = false;
    m_midPesPCR = false;
    m_priorityData.clear();
    while (m_outBufLen >= m_writeBlockSize) writeOutBuffer();  // catch up the flusher
}

void TSMuxer::writePESPacket()
{
    if (m_inplacePending)
    {
        finalizeInplacePes();
        return;
    }
    if (m_pesData.size() > 0)
    {
        uint32_t tsPackets = 0;

        const size_t size = m_pesData.size() - 6;
        if (size <= 0xffff)
        {
            m_pesData.data()[4] = static_cast<uint8_t>(size / 256);
            m_pesData.data()[5] = static_cast<uint8_t>(size % 256);
        }
        else
            refuseLongNonVideoPes();

        PMTStreamInfo& streamInfo = m_pmt.pidList[m_pesPID];
        const auto pesPacket = reinterpret_cast<PESPacket*>(m_pesData.data());
        bool updateIdx = false;
        if (m_computeMuxStats && (pesPacket->flagsLo & 0x80) == 0x80)
        {
            uint64_t curPts = pesPacket->getPts();

            size_t idxSize = streamInfo.m_index.size();
            if (idxSize == 0)
                streamInfo.m_index.emplace_back();
            const auto vCodec = streamInfo.m_mpegReader;  // cached at intAddStream
            // bool isH264 = dynamic_cast <H264StreamReader*> (streamInfo.m_codecReader);
            const bool SPSRequired = streamInfo.m_codecReader->needSPSForSplit();
            const auto aCodec = streamInfo.m_audioReader;  // cached at intAddStream
            if (vCodec && m_pesIFrame)
            {
                // skip some I-frames for H.264 if no SPS/PPS in a gop
                if (m_pesSpsPps || !SPSRequired)
                {
                    PMTIndex& curIndex = *streamInfo.m_index.rbegin();
                    if (curIndex.empty() || curPts > curIndex.rbegin()->first)
                    {
                        curIndex.insert(
                            std::make_pair(curPts, PMTIndexData(m_muxedPacketCnt[m_muxedPacketCnt.size() - 1], 0)));
                        updateIdx = true;
                    }
                }

                m_lastGopNullCnt = m_nullCnt;
            }
            else if (aCodec)
            {
                if (m_videoTrackCnt + m_videoSecondTrackCnt == 0)
                {
                    m_lastGopNullCnt = m_nullCnt;
                }
                PMTIndex& curIndex = *streamInfo.m_index.rbegin();
                idxSize = curIndex.size();
                if (idxSize == 0 || curPts - curIndex.rbegin()->first >= 90000)
                {
                    curIndex.insert(
                        std::make_pair(curPts, PMTIndexData(m_muxedPacketCnt[m_muxedPacketCnt.size() - 1], 0)));
                    updateIdx = true;
                }
            }
        }

        const uint8_t* curPtr = m_pesData.data();
        const uint8_t* dataEnd = curPtr + m_pesData.size();
        bool payloadStart = true;
        for (const auto& i : m_priorityData)
        {
            const uint8_t* blockPtr = m_pesData.data() + i.first;
            if (blockPtr > curPtr)
            {
                tsPackets += writeTSFrames(m_pesPID, curPtr, blockPtr - curPtr, false, payloadStart);
                payloadStart = false;
            }
            tsPackets += writeTSFrames(m_pesPID, blockPtr, i.second, true, payloadStart);
            curPtr = blockPtr + i.second;
        }
        tsPackets += writeTSFrames(m_pesPID, curPtr, dataEnd - curPtr, false, payloadStart);

        noteLateArrival();
        m_pesData.resize(0);
        m_priorityData.clear();
        if (updateIdx)
        {
            PMTIndex& curIndex = *streamInfo.m_index.rbegin();
            assert(curIndex.rbegin()->second.m_frameLen == 0);
            curIndex.rbegin()->second.m_frameLen = (tsPackets + 1) * m_frameSize;
        }
    }
}

void TSMuxer::writePCR(const int64_t newPCR)
{
    int bitsRest = 0;
    if (m_cbrBitrate != -1 && m_minBitrate != -1 && m_lastPCR != -1)
    {
        int expectedBits = lroundl(static_cast<double>(newPCR - m_lastPCR) / 90000.0 * m_minBitrate);
        expectedBits -= m_pcrBits;
        if (expectedBits > 0)
        {
            int tsFrames = expectedBits / 8 / m_frameSize;
            if (tsFrames * m_frameSize * 8 < expectedBits)
                tsFrames++;
            writeNullPackets(tsFrames);
            if (m_cbrBitrate == m_minBitrate)
                bitsRest = (tsFrames * m_frameSize * 8) - expectedBits;
        }
    }
    // One more packet than the buffer holds, because writeEmptyPacketWithPCR is about to add the
    // PCR packet and then run the arrival pass over the lot. Computed before m_pcrBits is cleared,
    // because the transport stream branch of it reads that count.
    const int64_t pcr = pcrForMaxRate(newPCR, 1, 0);
    m_pcrBits = bitsRest;
    // assert(m_pcrBits % 8 == 0);
    writeEmptyPacketWithPCR(pcr);
    m_lastPCR = pcr;
}

void TSMuxer::flushTSBuffer()
{
    if (m_owner->isAsyncMode())
        m_owner->waitForWriting();

    const int64_t fileSize = m_muxFile->size();
    if (fileSize == -1)
        THROW(ERR_FILE_COMMON, "Can't determine size for file " << m_outFileName)
    m_muxFile->close();

    if (!m_muxFile->open(m_outFileName.c_str(), File::ofWrite + File::ofAppend))
        THROW(ERR_FILE_COMMON, "Can't reopen file " << m_outFileName)

    if (writeOutFile(m_outBuf, m_outBufLen) != m_outBufLen)
        THROW(ERR_FILE_COMMON, "Can't write last data block to file " << m_outFileName)
    delete[] m_outBuf;
    m_outBufLen = 0;
}

void TSMuxer::finishFileBlock(const int64_t newPts, const int64_t newPCR, const bool doChangeFile, const bool recursive)
{
    if (m_processedBlockSize > 0)
    {
        constexpr auto gapForPATPMT = static_cast<int64_t>((192 * 4.0) / 35000000.0 * 27000000.0);
        doFlush(newPCR, gapForPATPMT);
        if (!doChangeFile)
            m_interleaveInfo.rbegin()->push_back(static_cast<int32_t>(m_processedBlockSize / 192));
        m_processedBlockSize = 0;
        m_owner->muxBlockFinished(this);
        if (m_m2tsMode)
            assert(m_outBuf == nullptr && m_outBufLen == 0);
        else
            flushTSBuffer();
        m_outBuf = new uint8_t[m_writeBlockSize + 1024];
        m_outBufCapacity = m_writeBlockSize + 1024;
        m_prevM2TSPCROffset = 0;
    }

    // In a 3D image the base view's block waits until the second view has finished its own, because the second
    // view's block comes first on the disc (MuxerManager::asyncWriteBuffer). When the base view began a new part, it
    // closed its file before the second view had finished, and its last block of the part was then written to a
    // closed file and lost: the two views of the part no longer matched and the image was refused ("5 pieces against
    // 6"). The second view now finishes its block first, so the waiting block lands while the part is still open. When
    // the second view begins the new part, this was already the order.
    const bool otherViewFirst = doChangeFile && recursive && m_masterMode && m_sublingMuxer;
    if (otherViewFirst)
        m_sublingMuxer->finishFileBlock(newPts, newPCR, doChangeFile, false);

    if (doChangeFile)
        gotoNextFile(newPts);

    writePATPMT(newPCR, true);
    writePCR(newPCR);

    if (m_sublingMuxer && recursive && !otherViewFirst)
        m_sublingMuxer->finishFileBlock(newPts, newPCR, doChangeFile, false);
}

bool TSMuxer::blockFull() const { return m_processedBlockSize >= m_interliaveBlockSize; }

bool TSMuxer::muxPacket(AVPacket& avPacket)
{
    if (avPacket.data == nullptr || avPacket.size == 0)
    {
        return true;
    }

#ifdef _DEBUG
    // assert (avPacket.dts >= m_lastProcessedDts);
    m_lastProcessedDts = avPacket.dts;
    m_lastStreamIndex = avPacket.stream_index;
#endif

    const int tsIndex = m_extIndexToTSIndex[avPacket.stream_index];
    if (tsIndex == 0)
        THROW(ERR_TS_COMMON, "Unknown track number " << avPacket.stream_index)
    StreamInfo& streamInfo = m_streamInfo[tsIndex];  // one map lookup per packet; downcast cached at intAddStream

    if (avPacket.stream_index == m_mainStreamIndex)
    {
        if (streamInfo.m_mpegReader)
            m_additionCLPISize = static_cast<int64_t>(INTERNAL_PTS_FREQ / streamInfo.m_mpegReader->getFPS());
    }

    // A title runs as long as its primary video does, whichever stream that turns out to be and
    // whatever order the meta listed them in. Measuring only the main stream truncated the playlist
    // to the shorter of two video tracks; measuring the longest of every stream would let a trailing
    // audio or subtitle block stretch a title past its last picture. The main stream is still read
    // here so an audio only mux, which has no primary video at all, keeps the length it always had.
    // With a single video track, which is every ordinary mux, this is the same stream as before.
    if (streamInfo.m_definesDuration || avPacket.stream_index == m_mainStreamIndex)
    {
        if (avPacket.duration > 0)
            *m_lastPts.rbegin() = FFMAX(*m_lastPts.rbegin(), avPacket.pts + avPacket.duration);
        else
            *m_lastPts.rbegin() = FFMAX(*m_lastPts.rbegin(), avPacket.pts);
    }
    if (m_firstPts[m_firstPts.size() - 1] == -1 /*|| avPacket.pts < m_firstPts[m_firstPts.size()-1]*/)
    {
        if (m_firstPts[m_firstPts.size() - 1] == -1)
        {
            m_curFileStartPts = avPacket.pts;
            const int64_t firstPtsShift = internalClockToPts(avPacket.pts);
            m_fixed_pcr_offset = m_timeOffset - m_vbvLen + firstPtsShift;
            if (m_fixed_pcr_offset < 0)
                m_fixed_pcr_offset = 0;
            m_prevM2TSPCR = (m_fixed_pcr_offset - 100) *
                            300;  // add a little extra delta (100) to prevent 0 bitrate for first several packets
        }
        m_firstPts[m_firstPts.size() - 1] = avPacket.pts;
    }

    if (m_pcr_delta == -1)
    {
        if (m_cbrBitrate != -1)
            m_pcr_delta = M_CBR_PCR_DELTA;
        else
            m_pcr_delta = M_PCR_DELTA;
        // In an M2TS the PAT and the PMT go out with every PCR. That was always so, because a PCR came at
        // least M_PCR_DELTA after the one before; a PCR that has to come sooner (see nextPCRChanceTooLate)
        // takes them along as well, so they too stay within 0.1 s of each other.
        m_patPmtDelta = m_m2tsMode ? 0 : PCR_FREQUENCY / 4;
    }

    if (m_minDts == -1)
        m_minDts = avPacket.dts;

    auto newPCR = (avPacket.dts - m_minDts) / INT_FREQ_TO_TS_FREQ + m_fixed_pcr_offset;
    if (m_lastPCR == -1)
    {
        writePATPMT(newPCR, true);
        writePCR(newPCR);
    }

    bool newPES = false;
    if (avPacket.dts != streamInfo.m_dts || avPacket.pts != streamInfo.m_pts || tsIndex != m_lastTSIndex ||
        avPacket.flags & AVPacket::FORCE_NEW_FRAME)
    {
        writePESPacket();
        newPES = true;
        if (streamInfo.m_dts != static_cast<int64_t>(ULLONG_MAX) && avPacket.dts > streamInfo.m_dts)
            streamInfo.m_dtsStep = avPacket.dts - streamInfo.m_dts;
    }

    m_lastTSIndex = tsIndex;

    if (m_cbrBitrate != -1 && m_lastPCR != -1)
    {
        const auto cbrPCR = llround(static_cast<double>(m_lastPCR + m_pcrBits) * 90000.0 / m_cbrBitrate);
        newPCR = FFMAX(newPCR, cbrPCR);
    }

    if (newPES && m_canSwithBlock && isSplitPoint(avPacket))
    {
        finishFileBlock(avPacket.pts, newPCR, true);  // goto next file
    }
    else if (newPES && m_interliaveBlockSize > 0 && m_canSwithBlock &&
             (blockFull() || (m_sublingMuxer && m_sublingMuxer->blockFull())))
    {
        finishFileBlock(avPacket.pts, newPCR, false);  // interleave SSIF here
    }
    else if (!(m_inplacePending && m_midPesPCR) &&
             (newPCR - m_lastPCR >= m_pcr_delta || (newPES && (m_cbrBitrate == -1 || m_maxBitrate != -1) &&
                                                    newPCR > m_lastPCR && nextPCRChanceTooLate(tsIndex, avPacket.dts))))
    {
        if (m_interliaveBlockSize == 0 || avPacket.stream_index == m_mainStreamIndex)
        {
            if (m_inplacePending)
            {
                // On the staged path these tables land in m_outBuf while the PES still
                // sits in m_pesData, so they belong BEFORE it in the output. Set the
                // pending bytes aside so the writers (and the m2ts timestamp pass) see
                // exactly the staged-path buffer state, then put the PES back behind
                // the new tables.
                const int pendingLen = m_outBufLen - m_pendingStartOffset;
                m_pendingSpill.resize(pendingLen);
                memcpy(m_pendingSpill.data(), m_outBuf + m_pendingStartOffset, pendingLen);
                m_outBufLen = m_pendingStartOffset;
                const uint32_t pktsBefore = m_muxedPacketCnt[m_muxedPacketCnt.size() - 1];
                writePATPMT(newPCR);
                writePCR(newPCR);
                m_pendingStartPktCnt += m_muxedPacketCnt[m_muxedPacketCnt.size() - 1] - pktsBefore;
                m_pendingStartOffset = m_outBufLen;
                ensureOutBufSpace(pendingLen);
                memcpy(m_outBuf + m_outBufLen, m_pendingSpill.data(), pendingLen);
                m_outBufLen += pendingLen;
            }
            else
            {
                writePATPMT(newPCR);
                writePCR(newPCR);
            }
        }
    }

    streamInfo.m_pts = avPacket.pts;
    streamInfo.m_dts = avPacket.dts;
    uint8_t pesStreamID = m_pesType[tsIndex];
    if (pesStreamID <= SYSTEM_START_CODE)
    {
        if (m_useNewStyleAudioPES)
            pesStreamID = PES_VC1_ID;
        else
            pesStreamID = PES_PRIVATE_DATA1;
    }

    addData(pesStreamID, tsIndex, avPacket);

    if (avPacket.duration > 0)
        m_endStreamDTS = avPacket.dts + avPacket.duration;
    else if (streamInfo.m_mpegReader)
        m_endStreamDTS = avPacket.dts + static_cast<int64_t>(INTERNAL_PTS_FREQ / streamInfo.m_mpegReader->getFPS());
    else
        m_endStreamDTS = avPacket.dts;

    return true;
}

// A variable rate mux writes a PCR only where a PES begins, once M_PCR_DELTA has passed since the last one. Where the
// PES come far apart, the first one past that point can be too late: video alone at 29.97 fps gave a PCR every three
// frames, 100.1 ms, and the next PCR after a 3D interleaving block that began on an audio frame could take 113 ms.
// So at each PES that may carry a PCR, this asks where the next such PES is due: for each stream with a steady pace,
// its last PES plus the step it has been taking, and in an interleaved 3D mux only the main stream, the only one that
// writes PCRs there. If even the earliest of them would put the next PCR more than 0.1 s after the last one, the PCR
// is written now. Its value is the one it always had, so nothing is clamped and no lag builds up; it only comes at an
// earlier PES.
bool TSMuxer::nextPCRChanceTooLate(const int tsIndex, const int64_t dts) const
{
    int mainTSIndex = 0;  // 0 is no PID: every stream counts
    if (m_interliaveBlockSize > 0)
    {
        const auto main = m_extIndexToTSIndex.find(m_mainStreamIndex);
        if (main == m_extIndexToTSIndex.end())
            return false;
        mainTSIndex = main->second;
    }
    int64_t nextDts = -1;
    for (const auto& [pid, info] : m_streamInfo)
    {
        if (!info.m_steadyPace || info.m_dtsStep <= 0 || (mainTSIndex != 0 && pid != mainTSIndex))
            continue;
        // The stream of the packet being muxed has not recorded its DTS yet. A PES due at this very DTS is the
        // next chance, right after this packet; one due earlier has stopped or paused, and says nothing.
        const int64_t due = (pid == tsIndex ? dts : info.m_dts) + info.m_dtsStep;
        if (due >= dts && (nextDts == -1 || due < nextDts))
            nextDts = due;
    }
    if (nextDts == -1)
        return false;
    const int64_t nextPCR = (nextDts - m_minDts) / INT_FREQ_TO_TS_FREQ + m_fixed_pcr_offset;
    return nextPCR - m_lastPCR > PCR_MAX_INTERVAL - PCR_INTERVAL_MARGIN;
}

// With --maxbitrate the PCR is held back until the data written since the last one could have arrived at that rate
// (pcrForMaxRate), so whenever the streams ask for more than the cap the clock falls behind the picture times. A PCR
// was only ever written when the picture times had moved on by m_pcr_delta from the last PCR, and a held PCR is
// later than them, so none came until they caught up: measured on a 90 Mbit/s video, the PCRs were 0.8 s apart with
// --maxbitrate=150000, 5 s with 115000 and 20 s with 48000, where the rules allow 0.1 s. A PCR is now also due once
// the data since the last one takes m_pcr_delta to arrive at the cap, so the held clock gets a PCR as often as the
// free one. It is checked before every packet of a stream, because one picture alone can take longer than 0.1 s to
// arrive at the cap. A mux that stays under the cap never meets this condition first, and is written exactly as
// before. 3D, whose two views are interleaved in blocks, is left as it was.
bool TSMuxer::maxRateClockDue() const
{
    if (m_maxBitrate == -1 || m_lastPCR == -1 || m_interliaveBlockSize > 0)
        return false;
    return static_cast<int64_t>(std::ceil(static_cast<double>(m_pcrBits) * 90000.0 / m_maxBitrate)) >= m_pcr_delta;
}

// The tables and the PCR go in between two packets of a picture. Inside a picture written in place, the PCR that the
// picture times would ask for is then left out for the rest of that picture (writeFrame), because its tables would
// be moved in front of the picture's packets that already carry arrival times from this one.
void TSMuxer::insertHeldPCRIfDue()
{
    if (!maxRateClockDue())
        return;
    ensureOutBufSpace(5 * m_frameSize);
    // Asked for at the last PCR, so pcrForMaxRate gives exactly the time the data takes at the cap, no later.
    writePATPMT(m_lastPCR + m_pcr_delta);
    writePCR(m_lastPCR);
    if (m_inplacePending)
        m_midPesPCR = true;
}

int TSMuxer::writeTSFrames(const int pid, const uint8_t* buffer, const int64_t len, const bool priorityData,
                           bool payloadStart)
{
    int result = 0;

    const uint8_t* curPos = buffer;
    const uint8_t* end = buffer + len;

    const bool tsPriority = priorityData;
    StreamInfo& streamInfo = m_streamInfo[pid];

    while (curPos < end)
    {
        insertHeldPCRIfDue();
        if (m_cbrBitrate != -1 && m_lastPCR != -1)
        {
            // The pacing value is monotone in m_pcrBits (90000/m_cbrBitrate ticks per
            // bit), so after a miss the earliest possible crossing is a computable number
            // of bits away; skip the floating point evaluation until then. The original
            // expression still decides every crossing, so the output is unchanged.
            if (m_lastPCR != m_cbrEvalPCR || m_pcrBits >= m_cbrNextEvalBits)
            {
                const auto newPCR = llround(static_cast<double>(m_lastPCR + m_pcrBits) * 90000.0 / m_cbrBitrate);
                if (newPCR - m_lastPCR >= m_pcr_delta && m_lastPCR != -1)
                {
                    m_pcrBits = 0;
                    writePATPMT(newPCR);
                    writePCR(newPCR);
                    if (m_lastPESDTS != -1 && m_lastPCR > m_lastPESDTS)
                    {
                        LTRACE(LT_ERROR, 2,
                               "VBV buffer overflow at position " << (double)(m_lastPCR - m_fixed_pcr_offset) / 90000.0
                                                                  << " sec");
                    }
                    m_cbrEvalPCR = m_lastPCR;
                    m_cbrNextEvalBits = 0;  // fresh interval: evaluate on the next packet
                }
                else
                {
                    const int64_t missingTicks = m_pcr_delta - (newPCR - m_lastPCR);
                    // conservative one tick guard against the llround boundary
                    const int64_t skipBits = missingTicks > 1 ? (missingTicks - 1) * m_cbrBitrate / 90000 : 0;
                    m_cbrEvalPCR = m_lastPCR;
                    m_cbrNextEvalBits = m_pcrBits + skipBits;
                }
            }
        }

        if (m_m2tsMode)
        {
            m_outBufLen += 4;
            m_processedBlockSize += 4;
            m_pcrBits += 4 * 8;
        }
        const int64_t tmpBufferLen = end - curPos;
        const auto initTS = UnalignedPtr<uint32_t>(m_outBuf + m_outBufLen);
        *initTS = TSPacket::TS_FRAME_SYNC_BYTE + TSPacket::DATA_EXIST_BIT_VAL;
        const auto tsPacket = reinterpret_cast<TSPacket*>(m_outBuf + m_outBufLen);
        int64_t payloadLen = TS_FRAME_SIZE - tsPacket->getHeaderSize();
        tsPacket->setPID(pid);
        tsPacket->counter = streamInfo.m_tsCnt++;
        tsPacket->payloadStart = payloadStart;  // curPos == buffer;
        payloadStart = false;
        tsPacket->priority = tsPriority;
        if (tmpBufferLen < payloadLen)
        {
            // insert padding bytes
            if (!tsPacket->afExists)
            {
                tsPacket->afExists = 1;
                if (payloadLen - tmpBufferLen == 1)
                {
                    tsPacket->adaptiveField.length = 0;
                    payloadLen--;
                }
                else
                {
                    initTS[1] = 0x01;  // zero all af flags, set af len to 1.
                    payloadLen -= 2;
                }
            }
            memset(reinterpret_cast<uint8_t*>(tsPacket) + tsPacket->getHeaderSize(), 0xff, payloadLen - tmpBufferLen);
            tsPacket->adaptiveField.length += static_cast<unsigned>(payloadLen - tmpBufferLen);
            payloadLen = tmpBufferLen;
        }
        const int tsHeaderSize = tsPacket->getHeaderSize();
        memcpy(m_outBuf + m_outBufLen + tsHeaderSize, curPos, payloadLen);

        curPos += payloadLen;
        m_outBufLen += TS_FRAME_SIZE;
        m_processedBlockSize += TS_FRAME_SIZE;
        m_pcrBits += TS_FRAME_SIZE * 8;
        m_muxedPacketCnt[m_muxedPacketCnt.size() - 1]++;
        writeOutBuffer();
        result++;
    }
    return result;
}

void TSMuxer::buildNULL()
{
    storeUnaligned<uint32_t>(m_nullBuffer, TSPacket::TS_FRAME_SYNC_BYTE);
    const auto tsPacket = reinterpret_cast<TSPacket*>(m_nullBuffer);
    tsPacket->setPID(NULL_PID);
    tsPacket->dataExists = 1;
    memset(m_nullBuffer + TSPacket::TS_HEADER_SIZE, 0xff, TS_FRAME_SIZE - TSPacket::TS_HEADER_SIZE);
}

void TSMuxer::buildPAT()
{
    storeUnaligned<uint32_t>(m_patBuffer, TSPacket::TS_FRAME_SYNC_BYTE + TSPacket::DATA_EXIST_BIT_VAL);
    const auto tsPacket = reinterpret_cast<TSPacket*>(m_patBuffer);
    tsPacket->setPID(PAT_PID);
    tsPacket->dataExists = 1;
    tsPacket->payloadStart = 1;
    m_pat.pmtPids[SIT_PID] = 0;          // add NIT. program number = 0
    m_pat.pmtPids[DEFAULT_PMT_PID] = 1;  // add PMT. program number = 1
    m_pat.transport_stream_id = 0;       // version of transport stream
    const uint32_t size =
        m_pat.serialize(m_patBuffer + TSPacket::TS_HEADER_SIZE, TS_FRAME_SIZE - TSPacket::TS_HEADER_SIZE);
    memset(m_patBuffer + TSPacket::TS_HEADER_SIZE + size, 0xff, TS_FRAME_SIZE - TSPacket::TS_HEADER_SIZE - size);
}

void TSMuxer::buildPMT()
{
    storeUnaligned<uint32_t>(m_pmtBuffer, TSPacket::TS_FRAME_SYNC_BYTE + TSPacket::DATA_EXIST_BIT_VAL);
    const auto tsPacket = reinterpret_cast<TSPacket*>(m_pmtBuffer);
    tsPacket->setPID(DEFAULT_PMT_PID);
    tsPacket->dataExists = 1;
    tsPacket->payloadStart = 1;
    const uint32_t size =
        m_pmt.serialize(m_pmtBuffer + TSPacket::TS_HEADER_SIZE, 3864, m_bluRayMode, m_hdmvDescriptors);
    uint8_t* pmtEnd = m_pmtBuffer + TSPacket::TS_HEADER_SIZE + size;
    uint8_t* curPos = m_pmtBuffer + TS_FRAME_SIZE;
    for (; curPos < pmtEnd; curPos += TS_FRAME_SIZE)
    {
        memmove(curPos + 4, curPos, pmtEnd - curPos);
        pmtEnd += 4;
        storeUnaligned<uint32_t>(curPos, TSPacket::TS_FRAME_SYNC_BYTE + TSPacket::DATA_EXIST_BIT_VAL);
        const auto tsPacket2 = reinterpret_cast<TSPacket*>(curPos);
        tsPacket2->setPID(DEFAULT_PMT_PID);
        tsPacket2->dataExists = 1;
        tsPacket2->payloadStart = 0;
    }
    const int64_t oldPmtSize = pmtEnd - m_pmtBuffer;
    int64_t roundSize = pmtEnd - m_pmtBuffer;
    if (roundSize % TS_FRAME_SIZE != 0)
        roundSize = (roundSize / TS_FRAME_SIZE + 1) * TS_FRAME_SIZE;
    memset(pmtEnd, 0xff, roundSize - oldPmtSize);
    m_pmtFrames = roundSize / TS_FRAME_SIZE;
}

void TSMuxer::buildSIT() {}

void TSMuxer::writeNullPackets(const int cnt)
{
    for (int i = 0; i < cnt; i++)
    {
        if (m_m2tsMode)
        {
            m_outBufLen += 4;
            m_processedBlockSize += 4;
            m_pcrBits += 4 * 8;
        }
        memcpy(m_outBuf + m_outBufLen, m_nullBuffer, TS_FRAME_SIZE);
        const auto tsPacket = reinterpret_cast<TSPacket*>(m_outBuf + m_outBufLen);
        tsPacket->counter = m_nullCnt++;
        m_outBufLen += TS_FRAME_SIZE;
        m_processedBlockSize += TS_FRAME_SIZE;
        m_pcrBits += TS_FRAME_SIZE * 8;
        m_muxedPacketCnt[m_muxedPacketCnt.size() - 1]++;
        writeOutBuffer();
    }
}

bool TSMuxer::appendM2TSNullPacketToFile(const int64_t curFileSize, int counter, int* packetsWrited) const
{
    *packetsWrited = 0;
    while (counter < 0) counter += 16;
    uint8_t tmpBuff[192];

    File* file = dynamic_cast<File*>(m_muxFile);

    file->seek(curFileSize - 192, File::SeekMethod::smBegin);
    const int readCnt = file->read(tmpBuff, 192);
    if (readCnt != 192)
        return false;
    memcpy(tmpBuff + 4, m_nullBuffer, TS_FRAME_SIZE);
    const auto tsPacket = reinterpret_cast<TSPacket*>(tmpBuff);
    for (int64_t newFileSize = curFileSize; newFileSize % (2048LL * 3) != 0; newFileSize += 192)
    {
        tsPacket->counter = counter++;
        if (file->write(tmpBuff, 192) != 192)
            return false;
        // m_muxedPacketCnt[m_muxedPacketCnt.size()-1]++;
        (*packetsWrited)++;
    }
    return true;
}

void TSMuxer::writePAT()
{
    if (m_m2tsMode)
    {
        m_outBufLen += 4;
        m_processedBlockSize += 4;
        m_pcrBits += 4 * 8;
    }
    memcpy(m_outBuf + m_outBufLen, m_patBuffer, TS_FRAME_SIZE);
    const auto tsPacket = reinterpret_cast<TSPacket*>(m_outBuf + m_outBufLen);
    tsPacket->counter = m_patCnt++;
    m_outBufLen += TS_FRAME_SIZE;
    m_processedBlockSize += TS_FRAME_SIZE;
    m_pcrBits += TS_FRAME_SIZE * 8;
    m_muxedPacketCnt[m_muxedPacketCnt.size() - 1]++;
    writeOutBuffer();
}

void TSMuxer::writePMT()
{
    const uint8_t* curPos = m_pmtBuffer;
    for (int i = 0; i < m_pmtFrames; i++)
    {
        if (m_m2tsMode)
        {
            m_outBufLen += 4;
            m_processedBlockSize += 4;
            m_pcrBits += 4 * 8;
        }
        memcpy(m_outBuf + m_outBufLen, curPos, TS_FRAME_SIZE);
        const auto tsPacket = reinterpret_cast<TSPacket*>(m_outBuf + m_outBufLen);
        tsPacket->counter = m_pmtCnt++;
        m_outBufLen += TS_FRAME_SIZE;
        m_processedBlockSize += TS_FRAME_SIZE;
        m_pcrBits += TS_FRAME_SIZE * 8;
        m_muxedPacketCnt[m_muxedPacketCnt.size() - 1]++;
        writeOutBuffer();
        curPos += TS_FRAME_SIZE;
    }
}

void TSMuxer::writeSIT()
{
    if (m_m2tsMode)
    {
        m_outBufLen += 4;
        m_processedBlockSize += 4;
        m_pcrBits += 4 * 8;
    }
    memcpy(m_outBuf + m_outBufLen, DefaultSitTableOne, TS_FRAME_SIZE);
    const auto tsPacket = reinterpret_cast<TSPacket*>(m_outBuf + m_outBufLen);
    tsPacket->counter = m_sitCnt++;
    m_outBufLen += TS_FRAME_SIZE;
    m_processedBlockSize += TS_FRAME_SIZE;
    m_pcrBits += TS_FRAME_SIZE * 8;
    m_muxedPacketCnt[m_muxedPacketCnt.size() - 1]++;
    writeOutBuffer();
}

void TSMuxer::writeOutBuffer()
{
    if (m_inplacePending)
        return;  // never flush while a PES is written in place: its bytes must stay movable
    if (m_outBufLen >= m_writeBlockSize)
    {
        int toFileLen = m_writeBlockSize & ~(MuxerManager::PHYSICAL_SECTOR_SIZE - 1);
        if (m_owner->isAsyncMode())
        {
            // the remainder can exceed one block right after an in-place PES finalized
            const int newCap = FFMAX(m_writeBlockSize, m_outBufLen - toFileLen) + 1024;
            const auto newBuf = new uint8_t[newCap];
            memcpy(newBuf, m_outBuf + toFileLen, m_outBufLen - toFileLen);
            m_outBufCapacity = newCap;
            if (m_m2tsMode)
            {
                if (m_prevM2TSPCROffset >= toFileLen && m_m2tsDelayBlocks.empty())
                {
                    m_owner->asyncWriteBuffer(this, m_outBuf, toFileLen, m_muxFile);
                    m_prevM2TSPCROffset -= toFileLen;
                }
                else
                    m_m2tsDelayBlocks.emplace_back(m_outBuf, toFileLen);
            }
            else
                m_owner->asyncWriteBuffer(this, m_outBuf, toFileLen, m_muxFile);
            m_outBuf = newBuf;
        }
        else
        {
            if (!m_m2tsMode)
                m_owner->syncWriteBuffer(this, m_outBuf, toFileLen, m_muxFile);
            else
            {
                if (m_prevM2TSPCROffset >= toFileLen && m_m2tsDelayBlocks.empty())
                {
                    m_owner->syncWriteBuffer(this, m_outBuf, toFileLen, m_muxFile);
                    m_prevM2TSPCROffset -= toFileLen;
                }
                else
                {
                    auto newBuf = new uint8_t[toFileLen];
                    memcpy(newBuf, m_outBuf, toFileLen);
                    m_m2tsDelayBlocks.emplace_back(newBuf, toFileLen);
                }
            }
            memmove(m_outBuf, m_outBuf + toFileLen, m_outBufLen - toFileLen);
        }
        m_outBufLen -= toFileLen;
    }
}

// Both --bitrate and --maxbitrate end up in m_cbrBitrate, so both have to be held above the rate
// the pacing can carry. --minbitrate is deliberately left alone: on its own it never reaches the
// pacing term, and zero is what the GUI writes when no lower bound is wanted.
int checkedMuxRate(const std::string& option, const std::string& value);  // defined below

// The size a --split-size value works out at, or a refusal. A size of zero or less can only be a
// typo, and accepting it would turn splitting off without a word, which is what asking for 4 GiB
// used to do before the option was widened to 64 bit.
int64_t checkedSplitSize(const std::string& value)
{
    int64_t coeff = 1;
    string postfix;
    for (const auto& j : value)
        if (!((j >= '0' && j <= '9') || j == '.'))
            postfix += j;
    postfix = strToUpperCase(postfix);
    if (postfix == "GB")
        coeff = 1000LL * 1000 * 1000;
    else if (postfix == "GIB")
        coeff = 1024LL * 1024 * 1024;
    else if (postfix == "MB")
        coeff = 1000LL * 1000;
    else if (postfix == "MIB")
        coeff = 1024LL * 1024;
    else if (postfix == "KB")
        coeff = 1000;
    else if (postfix == "KIB")
        coeff = 1024;
    const string prefix = value.substr(0, value.size() - postfix.size());
    const int64_t splitBytes = static_cast<int64_t>(strToDouble(prefix.c_str()) * static_cast<double>(coeff));
    if (splitBytes <= 0)
        THROW(ERR_COMMON, "Invalid --split-size value \"" << value << "\": it must be above zero.")
    return splitBytes;
}

// ** A REFUSAL AFTER THE DESTINATION IS OPENED DESTROYS THE USER'S FILE, AND THE USER ASKED THE
// PROGRAM TO STOP. **
//
// main() creates the output image before the mux runs, so every refusal raised later replaced an
// existing .iso with a stub: 2,490,368 bytes of a good disc image became 1,769,472, three runs out
// of three, while the message said the mux had been refused. Measured on four separate refusals
// from three different commits, so it is a class and not one slip.
//
// This runs while the meta file is still being read, before any destination exists. It sets
// nothing and returns nothing: it exists only to raise the same refusal earlier. The checks below
// STAY where they are as the backstop, which is the discipline cc0eb53 used, so a route that does
// not pass through here is still refused, only later.
//
// ** IT CALLS THE SAME PREDICATES THE REAL PARSE CALLS, DELIBERATELY. ** An early check that asks a
// DIFFERENT question is the trap cc0eb53 fell into: its early gate only counted HEVC video tracks,
// so it closed one route of at least three. One predicate in one place cannot drift from itself.
//
// --minbitrate is not here, exactly as it is not in the parse below: on its own it never reaches
// the pacing term, and zero is what the interface writes when no lower bound is wanted.
void validateMuxOptsEarly(const std::string& opts)
{
    const vector<string> params = splitQuotedStr(opts.c_str(), ' ');
    for (auto& i : params)
    {
        vector<string> paramPair = splitStr(trimStr(i).c_str(), '=');
        if (paramPair.size() < 2)
            continue;
        if (paramPair[0] == "--bitrate" || paramPair[0] == "--maxbitrate")
            checkedMuxRate(paramPair[0], paramPair[1]);
        else if (paramPair[0] == "--split-size")
            checkedSplitSize(paramPair[1]);
    }
}

int checkedMuxRate(const std::string& option, const std::string& value)
{
    const auto rate = static_cast<int>(strToDouble(value.c_str()) * 1000.0);
    if (rate <= DEGENERATE_MUX_RATE)
        THROW(ERR_COMMON, "Invalid " << option << " value \"" << value << "\": the rate is in kbps and must be above "
                                     << DEGENERATE_MUX_RATE / 1000
                                     << ". At or below that the muxing clock overtakes the media and the output "
                                        "grows without bound.")
    return rate;
}

void TSMuxer::parseMuxOpt(const std::string& opts)
{
    const vector<string> params = splitStr(opts.c_str(), ' ');
    bool cbrAsked = false;
    bool vbrAsked = false;
    bool rateGiven = false;
    for (auto& i : params)
    {
        vector<string> paramPair = splitStr(trimStr(i).c_str(), '=');
        if (paramPair.empty())
            continue;
        if (paramPair[0] == "--no-pcr-on-video-pid")
            setPCROnVideoPID(false);
        else if (paramPair[0] == "--new-audio-pes")
            setNewStyleAudioPES(true);
        else if (paramPair[0] == "--no-hdmv-descriptors")
            m_hdmvDescriptors = false;
        else if (paramPair[0] == "--cbr")
            cbrAsked = true;
        else if (paramPair[0] == "--vbr")
            vbrAsked = true;
        else if (paramPair[0] == "--bitrate" && paramPair.size() > 1)
        {
            const int rate = checkedMuxRate(paramPair[0], paramPair[1]);
            setMuxRate(rate);
            setMinBitrate(rate);
            rateGiven = true;
        }
        else if (paramPair[0] == "--maxbitrate" && paramPair.size() > 1)
            setMaxBitrate(checkedMuxRate(paramPair[0], paramPair[1]));
        else if (paramPair[0] == "--minbitrate" && paramPair.size() > 1)
            setMinBitrate(static_cast<int>(strToDouble(paramPair[1].c_str()) * 1000.0));
        else if (paramPair[0] == "--vbv-len" && paramPair.size() > 1)
            setVBVBufferLen(strToInt32(paramPair[1].c_str()));
        // Written with a space instead of an equals sign this read past the end of the vector. See
        // the same guard in main.cpp: undefined behaviour, so it crashed on some options and read
        // rubbish on others. Silently ignoring it is not an option here, because turning splitting
        // off without a word is the defect the size check above was written to stop.
        else if (paramPair[0] == "--split-duration")
        {
            if (paramPair.size() < 2)
                THROW(ERR_COMMON, "Missing value for " << paramPair[0])
            setSplitDuration(strToInt64(paramPair[1].c_str()) * INTERNAL_PTS_FREQ);
            m_computeMuxStats = true;
        }
        else if (paramPair[0] == "--split-size")
        {
            if (paramPair.size() < 2)
                THROW(ERR_COMMON, "Missing value for " << paramPair[0])
            setSplitSize(checkedSplitSize(paramPair[1]));
            m_computeMuxStats = true;
        }
        else if (paramPair[0] == "--split-chapters")
        {
            m_splitByChapters = true;
            m_computeMuxStats = true;
        }
        else if (paramPair[0] == "--blu-ray" || paramPair[0] == "--blu-ray-v3" || paramPair[0] == "--avchd")
        {
            m_bluRayMode = true;
            m_computeMuxStats = true;
        }
        else if (paramPair[0] == "--cut-start" || paramPair[0] == "--cut-end")
        {
            m_computeMuxStats = true;
        }
    }
    // Neither --cbr nor --vbr is a switch. Nothing anywhere reads them, the rate comes from
    // --bitrate alone, and until now both words fell out of the bottom of this loop without a
    // sound. So a meta that asked for a fixed bitrate and forgot to give one got a variable
    // bitrate mux and no hint that its request had been dropped.
    if (cbrAsked && vbrAsked)
        LTRACE(LT_WARN, 2,
               "Warning: --cbr and --vbr were both given, and they ask for opposite things. Neither one is a "
               "switch: the rate comes from --bitrate, so the mux is constant bitrate with it and variable "
               "bitrate without it.");
    else if (cbrAsked && !rateGiven)
        LTRACE(LT_WARN, 2,
               "Warning: --cbr was given without --bitrate, so there is no rate to hold to and the mux is "
               "variable bitrate.");
    else if (vbrAsked && rateGiven)
        LTRACE(LT_WARN, 2,
               "Warning: --vbr was given together with --bitrate. --bitrate fixes the rate and it wins, so the "
               "mux is constant bitrate.");
}

void TSMuxer::setSubMode(AbstractMuxer* mainMuxer, const bool flushInterleavedBlock)
{
    m_sublingMuxer = dynamic_cast<TSMuxer*>(mainMuxer);
    m_subMode = true;
    m_canSwithBlock = flushInterleavedBlock;
    m_interleaveInfo.rbegin()->push_back(0);
}

void TSMuxer::joinToMasterFile()
{
    m_isExternalFile = true;
    m_muxFile = m_sublingMuxer->getDstFile();
}

void TSMuxer::setMasterMode(AbstractMuxer* subMuxer, const bool flushInterleavedBlock)
{
    m_sublingMuxer = dynamic_cast<TSMuxer*>(subMuxer);
    m_masterMode = true;
    m_canSwithBlock = flushInterleavedBlock;
    m_interleaveInfo.rbegin()->push_back(0);
}

void TSMuxer::openDstFile()
{
    m_muxFile = m_fileFactory ? m_fileFactory->createFile() : new File();

    int systemFlags = 0;
#ifdef _WIN32
    // An unbuffered write has to be a whole number of the volume's sectors, and this writer works in units of
    // PHYSICAL_SECTOR_SIZE (2,048 bytes). On a volume with larger sectors, 4,096 bytes on a drive with native 4 KB
    // sectors, Windows refused the first block of a .ts ("The parameter is incorrect", error 87), and the last block
    // of some .m2ts files and the blocks of a 3D disc. There the file is written through the system cache instead,
    // as --no-asyncio does; the bytes written are the same.
    if (m_owner->isAsyncMode())
    {
        const uint32_t sector =
            m_fileFactory && m_fileFactory->isVirtualFS() ? 0 : File::volumeSectorSize(m_outFileName.c_str());
        if (sector == 0 || MuxerManager::PHYSICAL_SECTOR_SIZE % sector == 0)
            systemFlags += FILE_FLAG_NO_BUFFERING;
    }
#endif
    if (!m_muxFile->open(m_outFileName.c_str(), File::ofWrite, systemFlags))
        THROW(ERR_CANT_CREATE_FILE, "Can't create file " << m_outFileName)
}

vector<int64_t> TSMuxer::getFirstPts() const
{
    std::vector<int64_t> rez;
    rez.reserve(m_firstPts.size());
    for (const auto& i : m_firstPts) rez.push_back(internalClockToPts(i) + m_timeOffset);
    return rez;
}

void TSMuxer::alignPTS(TSMuxer* otherMuxer)
{
    if (!m_lastPts.empty() && !otherMuxer->m_lastPts.empty())
    {
        const int64_t minPTS = FFMIN(*m_lastPts.rbegin(), *otherMuxer->m_lastPts.rbegin());
        *m_lastPts.rbegin() = minPTS;
        *otherMuxer->m_lastPts.rbegin() = minPTS;
    }
}

vector<int64_t> TSMuxer::getLastPts() const
{
    std::vector<int64_t> rez;
    rez.reserve(m_lastPts.size());
    for (const auto& i : m_lastPts) rez.push_back(internalClockToPts(i) + m_timeOffset);
    return rez;
}

void TSMuxer::setFileName(const std::string& fileName, FileFactory* fileFactory)
{
    m_curFileNum = 0;
    AbstractMuxer::setFileName(fileName, fileFactory);
    m_outFileName = getNextName(fileName);
    const string ext = strToUpperCase(extractFileExt(m_outFileName));
    setMuxFormat(ext);

    m_fileNames.clear();
    m_fileNames.push_back(m_outFileName);
}

std::string TSMuxer::getFileNameByIdx(const size_t idx)
{
    if (idx < m_fileNames.size())
        return m_fileNames[idx];
    assert(1);
    return {};
}

void TSMuxer::setMuxFormat(const std::string& format)
{
    m_m2tsMode = format == "M2TS" || format == "M2T" || format == "MTS" || format == "SSIF";
    m_writeBlockSize = m_m2tsMode ? DEFAULT_FILE_BLOCK_SIZE : TS188_ROUND_BLOCK_SIZE;
    m_outBuf = new uint8_t[m_writeBlockSize + 1024];
    m_outBufCapacity = m_writeBlockSize + 1024;
    m_frameSize = m_m2tsMode ? 192 : 188;
    if (m_m2tsMode)
        m_sectorSize = 1024 * 6;
}

bool TSMuxer::isInterleaveMode() const { return m_masterMode || m_subMode; }

std::vector<int32_t> TSMuxer::getInterleaveInfo(const size_t idx) const { return m_interleaveInfo[idx]; }
