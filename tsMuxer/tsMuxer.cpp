// Largely rewritten by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#include "tsMuxer.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <fs/systemlog.h>
#include <fs/textfile.h>

#include "ac3StreamReader.h"
#include "crc32.h"
#include "dtsStreamReader.h"
#include "h264StreamReader.h"
#include "hevcStreamReader.h"
#include "iso_writer.h"
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
// The HD figure is also measured rather than only read: three HD disc structures were
// checked and none of the 3.3 million packets between them has a gap below 846 ticks, which is
// exactly 48 Mbit/s on the 188 byte packet.
//
// The UHD figure comes from the BDA white paper for BD-ROM Version 3, where the maximum multiplex
// rate of the transport stream is given per disc capacity and per zone: 64 and 81.7 Mbit/s for a
// 50 GB disc, 81.7 and 109 for 66 and 100 GB, and 127.9 in the high transfer rate zone of the two
// larger ones. 109 is therefore not the only legal answer, it is the one that fits the capacities
// UHD titles actually come on. A single number is a simplification and this is the useful
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

static constexpr uint8_t DefaultSitTableOne[] = {
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

// The peak_rate of the partial transport stream descriptor in the SIT, in units of 400 bit/s, outside a disc: the
// 48 Mbit/s of the table above, and 127.9 Mbit/s with HEVC, the top of the UHD disc's rates
static constexpr int SIT_PEAK_RATE = 120000;
static constexpr int SIT_PEAK_RATE_HEVC = 319750;

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
    m_cbrGiven = false;
    m_cbrWinBits = 0;
    m_cbrWinTicks = 0;
    m_cbrWinStart = 0;
    m_cbrCarryBits = 0;
    m_floorPadBits = 0;
    m_floorPadRest = 0;
    m_cbrPeakRate = 0;
    m_cbrPeakAt = 0;
    m_sendAhead = false;
    m_outBufPos = 0;
    m_saHeldEnd = 0;
    m_saRecEnd = 0;
    m_saRecOldHi = 0;
    m_saDoneEnd = 0;
    m_saPrevHi = 0;
    m_saPrevPcr = 0;
    m_saPrevOldPcr = 0;
    m_saPassBits = 0;
    m_saFloorPcr = 0;
    m_saFirstPass = true;
    m_saDtsClock = 0;
    m_saMuxDts = 0;
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
    for (const auto& b : m_saHeld) delete[] b.data;  // only after a failed mux: a finished one hands all of it on
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
            // The core of audio that has one, which a player without the decoder of the rest plays, is at 48 kHz
            const int freq = audio->getFreq();
            const int coreFreq = audio->getCoreFreq();
            const bool freqBad = freq > 0 && freq != 48000 && freq != 96000 && freq != 192000;
            if (freqBad || (coreFreq > 0 && coreFreq != 48000))
                LTRACE(LT_WARN, 2,
                       "Warning: track " << streamIndex << " (" << audio->getCodecInfo().displayName << ") has "
                                         << (freqBad ? "a sample rate of " : "a core at a sample rate of ")
                                         << (freqBad ? freq : coreFreq)
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
    // An enhancement layer is never larger than its base: a Dolby Vision picture wider than the base before it is a
    // picture of its own (a 4K profile 8.1 listed after a 1080p video was put on 0x1015 as that video's layer).
    // Nor is a stream whose RPU says it is a single layer (profile 8.1: HevcDvRpuUnit) anyone's layer: a 4K profile 8.1
    // listed after a plain 4K video went to 0x1015.
    const auto* mpegReader = dynamic_cast<MPEGStreamReader*>(codecReader);
    const auto* hevcReader = dynamic_cast<HEVCStreamReader*>(codecReader);
    const bool isDvEnhancement =
        codecReader != nullptr && codecReader->getStreamHDR() == 4 && m_videoTrackCnt > 0 &&
        !(mpegReader && m_firstBaseWidth > 0 && mpegReader->getStreamWidth() > m_firstBaseWidth) &&
        !(hevcReader && hevcReader->dvRpuElPresent() == 0);
    const bool isPrimaryVideo = isVideo && !isSecondary && !isDvEnhancement;

    // A UHD disc (V3) carries its main video at 1080 lines or more, and progressive. The test was made where the
    // video format code is worked out, which is done again for every table that carries it: on the WIDTH, so 1280x720
    // passed and a 720x480 picture-in-picture warned, and the line came as often as the tables were written. Now on
    // the height of the primary video, once for the track.
    // The VC-1 reader reads its sequence header only with the first picture (getTSDescriptor reads it into a copy of
    // its own), so its height here was 0 and 1920x1080 VC-1 got the warning. A video whose size is not known yet is
    // tested at its first packet (muxPacket), when the reader has read it.
    if (isPrimaryVideo && isV3())
    {
        if (const auto video = dynamic_cast<MPEGStreamReader*>(codecReader))
        {
            if (video->getStreamHeight() > 0)
                warnV3Video(video);
            else
            {
                m_v3VideoPending = video;
                m_v3VideoPendingIndex = streamIndex;
            }
        }
    }

    // The format of the main video of any disc, at its first packet, when its rate is known as well
    if (isPrimaryVideo && m_bluRayMode && m_discVideoPending == nullptr && m_discVideoPendingIndex < 0)
        if (const auto video = dynamic_cast<MPEGStreamReader*>(codecReader))
        {
            m_discVideoPending = video;
            m_discVideoPendingIndex = streamIndex;
        }

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
                if (m_videoTrackCnt == 0 && mpegReader)
                    m_firstBaseWidth = mpegReader->getStreamWidth();
                m_videoTrackCnt++;
                V3_flags |= BL_TRACK;
            }
            if (m_subMode)
                tsStreamIndex++;
            // The Dolby Vision descriptor of a transport stream names the base layer's PID on its enhancement layer.
            // A single layer Dolby Vision stream (profile 5 or 8) is no one's base layer.
            if (!isDvEnhancement && codecReader != nullptr && codecReader->getStreamHDR() != 4)
                m_dvBasePids.push_back(tsStreamIndex);
            if (auto* hevc = dynamic_cast<HEVCStreamReader*>(codecReader))
            {
                int basePid = 0;
                if (isDvEnhancement && m_dvBasePids.empty())
                    basePid = 0x1011;
                else if (isDvEnhancement)
                    basePid = m_dvBasePids[(std::min)(static_cast<size_t>(m_DVvideoTrackCnt), m_dvBasePids.size()) - 1];
                hevc->setDoViBaseLayerPid(basePid);
            }
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
            // A disc's secondary audio is mixed over the main audio by the player, and is DTS Express or Dolby Digital
            // Plus without an AC-3 core, as the help says. Any other track marked so was declared secondary without a
            // word: DTS-HD High Resolution or Master Audio as secondary DTS-HD, a Dolby Digital Plus track with its
            // core as secondary Dolby Digital Plus, AC-3 or LPCM with the stream type of primary audio.
            const auto dts = dynamic_cast<DTSStreamReader*>(codecReader);
            const auto ac3 = dynamic_cast<AC3StreamReader*>(codecReader);
            const char* what = nullptr;
            if (dts && dts->getDTSHDMode() != DTSStreamReader::DTSHD_SUBTYPE::DTS_SUBTYPE_EXPRESS)
                what = dts->getDTSHDMode() == DTSStreamReader::DTSHD_SUBTYPE::DTS_SUBTYPE_MASTER_AUDIO
                           ? "DTS-HD Master Audio"
                       : dts->getDTSHDMode() == DTSStreamReader::DTSHD_SUBTYPE::DTS_SUBTYPE_UNINITIALIZED
                           ? "DTS"
                           : "DTS-HD High Resolution";
            else if (ac3 && ac3->isTrueHD())
                what = "TrueHD";
            else if (ac3 && ac3->isEAC3() && ac3->isAC3() && !ac3->getDownconvertToAC3())
                what = "Dolby Digital Plus with an AC-3 core";
            else if (ac3 && (!ac3->isEAC3() || ac3->getDownconvertToAC3()))
                what = "AC-3";
            else if (!dts && !ac3)
                what = codecName.c_str();
            if (what && m_bluRayMode)
                LTRACE(LT_WARN, 2,
                       "Warning: track "
                           << streamIndex << " (" << what
                           << ") is marked secondary. A Blu-ray disc's secondary audio is DTS Express or Dolby "
                              "Digital Plus without an AC-3 core, which the player mixes over the main audio, so a "
                              "player may not play this track. Leave out \"secondary\" to mux it as primary audio.");
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
    // The PCR goes on the video PID. A mux without video put it on PID 0, the PID of the program association table,
    // which the PCR_PID of the program map then named: it goes on a PID of its own, as with --no-pcr-on-video-pid,
    // until a video track takes it.
    if (m_pcrOnVideo)
    {
        if (codecName[0] == 'V')
            m_pmt.pcr_pid = tsStreamIndex;
        else if (m_pmt.pcr_pid == 0)
            m_pmt.pcr_pid = DEFAULT_PCR_PID;
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
            // stream id 0xfd as the Blu-ray format has it. A merged track writes the matching PES
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
        m_sitHevc = true;
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
            // Which audio is secondary is the author's choice ("secondary" on the line). But a Blu-ray disc has DTS
            // Express as secondary audio only: as primary audio it goes out as DTS-HD High Resolution, which needs
            // the DTS core that an Express stream does not have. This used to be written without a word.
            if (m_bluRayMode && !dtsReader->isSecondary() &&
                dtsReader->getDTSHDMode() == DTSStreamReader::DTSHD_SUBTYPE::DTS_SUBTYPE_EXPRESS)
                LTRACE(LT_WARN, 2,
                       "Warning: track "
                           << streamIndex
                           << " (DTS Express) is muxed as primary audio. A Blu-ray disc has DTS Express as secondary "
                              "audio only, so a player may not play it. Give \"secondary\" on its line to mux it as "
                              "secondary audio (a commentary track, mixed over the main audio).");
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
        if (auto* audio = itPid->second.m_audioReader)
        {
            itPid->second.m_firstChannels = audio->getChannels();
            itPid->second.m_firstFreq = audio->getFreq();
            itPid->second.m_firstCoreFreq = audio->getCoreFreq();
            itPid->second.m_firstAltFreq = audio->getAltFreq();
        }
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
        if (m_interliaveBlockSize == 0 || m_maxBitrate == -1 || m_lastPCR == -1)
            writeNullPackets(nullCnt / m_frameSize);
        else
        {
            // The padding of an interleaving block of 3D is up to 31 packets, and the block's PCR comes after it: at
            // a low --maxbitrate the padding alone takes longer than 0.1 s to arrive at the cap. The held clock gets
            // its PCRs inside the padding as well (maxRateClockDue); the tables and the PCR are part of it, and the
            // block still ends on the sector. Where no PCR comes due, the same nulls are written.
            while (m_processedBlockSize % m_sectorSize)
            {
                insertHeldPCRIfDue();
                if (m_processedBlockSize % m_sectorSize)
                    writeNullPackets(1);
            }
        }
    }

    if (m_m2tsMode)
    {
        const int64_t pcr = pcrForMaxRate(newPCR, 0, pcrGAP);
        if (m_sendAhead)
            sendAheadRecordPass(pcr, pcrForMaxRate(0, 0, pcrGAP), pcrGAP, m_prevM2TSPCR, false);
        processM2TSPCR(pcr, pcrGAP);
    }
    // The end of the output or of a block: nothing after it can be sent ahead of it, so every time is made final here
    // and the held data goes out before the rest of the buffer.
    if (m_sendAhead)
        sendAheadFinishAll();

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
    m_outBufPos += roundBufLen;
    m_saHeldEnd = m_outBufPos;
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
// source and not from muxing, and sent people off to re-encode. Measured on an HD Blu-ray structure
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
        << " to hold it at the limit: the muxer then spreads the data so that no second asks for more than that "
           "rate, as the disc format requires. If the video's own peak is already above the limit the muxer "
           "cannot invent room for it, and only a re-encode with the peak bitrate capped will help.";
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
    if (m_sendAhead)
    {
        sendAheadNotePes();  // counted when its time is final (sendAheadFinalizeFront)
        return;
    }
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

// How late the latest picture is, for the warning: whole milliseconds below one second, rounded up, so that a picture
// that is late is never said to be 0 ms late; from one second, seconds to a hundredth. In tenths or hundredths of a
// second, a delay of a few milliseconds read "0.0 s" or "0.00 s", as if there were none (#51).
static std::string latenessText(const int64_t ticks)
{
    const int64_t ms = (std::max)(int64_t{1}, (ticks + 89) / 90);
    if (ms < 1000)
        return std::to_string(ms) + " ms";
    return doubleToStr(static_cast<double>(ticks) / 90000.0, 2) + " s";
}

void TSMuxer::reportLateArrival() const
{
    if (m_latePictures == 0)
        return;
    LTRACE(LT_WARN, 2,
           "Warning: with --maxbitrate=" << m_maxBitrate / 1000 << ", " << m_latePictures << " of " << m_cappedPictures
                                         << " video pictures arrive after they are due, up to "
                                         << latenessText(m_worstLateness)
                                         << " late. The video's peaks are above the cap, so a player may stutter or "
                                            "freeze there. Re-encode the video with its peak bitrate capped below the "
                                            "cap, or raise the cap.");
    // 3D in interleaved blocks: each view is held to the cap on a clock of its own, and far below the video's rate the
    // two clocks drift apart, so the second view's clock references can jump. Measured: none from 500 kbps up, jumps
    // at 150 kbps, where no 3D video can play; a 3D disc allows far more than any such cap.
    // once per job: both views report their late pictures, the 3D cause is one
    static bool said3d = false;
    if (m_interliaveBlockSize > 0 && !said3d && (said3d = true))
        LTRACE(LT_WARN, 2,
               "Warning: this is 3D video, and the cap is below what it needs. Far below that, the two views can no "
               "longer be kept in step and the clock of the second view can jump. Give a higher --maxbitrate.");
}

// --bitrate holds the output at its rate from below only: null packets fill up to it, but where the content needs
// more, the clock follows the decoding times and the data goes out at the rate the content needs, so the output is
// not at a constant rate there, and nothing said so. The rate a constant-rate clock would hold it to (the term in
// writeTSFrames) only moves the clock while the PCR is small, which it is not with the usual start times. The bits of
// content written between two PCRs (all but the null packets of the padding) are summed into windows of at least a
// second of the clock; a window whose content is above the rate by more than four packets counts, and the highest one
// is said once at the end. A short peak that the rate could carry over the window is not counted. The output is not
// changed.
void TSMuxer::noteCbrRate(const int64_t pcr, const int64_t paddingBits)
{
    if (!m_cbrGiven || m_lastPCR == -1 || pcr <= m_lastPCR)
        return;
    if (m_cbrWinTicks == 0)
        m_cbrWinStart = m_lastPCR;
    m_cbrWinBits += m_pcrBits - m_cbrCarryBits - paddingBits;
    m_cbrWinTicks += pcr - m_lastPCR;
    if (m_cbrWinTicks < 90000)
        return;
    const int64_t allowed = static_cast<int64_t>(m_cbrBitrate) * m_cbrWinTicks / 90000 + 4 * m_frameSize * 8;
    if (m_cbrWinBits > allowed)
    {
        const int64_t rate = m_cbrWinBits * 90000 / m_cbrWinTicks;
        if (rate > m_cbrPeakRate)
        {
            m_cbrPeakRate = rate;
            m_cbrPeakAt = m_cbrWinStart + m_cbrWinTicks / 2;
        }
    }
    m_cbrWinBits = 0;
    m_cbrWinTicks = 0;
}

void TSMuxer::reportCbrExcess() const
{
    if (m_cbrPeakRate == 0)
        return;
    // once per job for 3D in interleaved blocks: both views are measured, the cause is one
    static bool said3d = false;
    if (m_interliaveBlockSize > 0 && said3d)
        return;
    if (m_interliaveBlockSize > 0)
        said3d = true;
    LTRACE(LT_WARN, 2,
           "Warning: the content needs more than --bitrate="
               << m_cbrBitrate / 1000 << " kbps (up to " << m_cbrPeakRate / 1000 << " kbps around "
               << doubleToStr(static_cast<double>(m_cbrPeakAt - m_fixed_pcr_offset) / 90000.0, 1)
               << " s), so the output is not at a constant rate there. "
               << (m_maxBitrate == -1 ? "Give a higher --bitrate, or use --maxbitrate "
                                        "to cap it."
                                      : "Give a higher --bitrate."));
}

// ** --maxbitrate SENDS DATA AHEAD IN THE QUIET PARTS. **
//
// The pacing gives each packet the time the picture times ask for, vbv-len before the decoding time, or later when
// the data since the last PCR takes longer than that to arrive at the cap. It only ever holds data back. Where the
// video's own peaks are above the cap, its pictures therefore arrived after they were due even when the average was
// well under it, because the time the quiet part before a peak leaves unused was never spent on the peak.
//
// The times are now made final only once the muxer has seen what comes next. Every pass between two PCRs is kept,
// with the output it covers, until the decoding times have moved on by four times vbv-len past the point where the
// link has stood idle for vbv-len after it (sendAheadReach), at most SEND_AHEAD_HOLD_LIMIT of output. Its end is then
// given the latest time that still lets every PES seen in that window arrive by its decoding time at the cap, if
// that is earlier than the time it was written with: the packets keep their order, their content and their count,
// only the PCR values and the arrival timestamps change. No time is ever made more than vbv-len earlier than the
// picture times ask for, so data waits in the decoder for at most twice vbv-len, and no pass is ever shorter than its
// packets take at the cap. Where even that cannot bring a picture in on time, the peak is longer than the decoder
// can buffer, and the warning at the end of the mux still counts it.
//
// A mux that never needs it gets every time exactly as before: a pass is only made earlier when one of the PES after
// it would otherwise be late. Measured on a 1080p H.264 video averaging 17 Mbit/s with peaks of 70 to 120 Mbit/s,
// --maxbitrate=40000: 125 of 719 pictures up to 1.0 s late before, 49 up to 0.5 s now. With at most 1 s in the
// decoder no order of sending these packets does better than 46 up to 0.5 s. On a cut of an HD disc that begins in
// a dense scene, --maxbitrate=40000: 43 late up to 107 ms before, none now.
//
// Not with --bitrate or --minbitrate, whose padding follows the old times, nor for the two views of a 3D disc, which
// are interleaved in blocks.
static constexpr int64_t SEND_AHEAD_WINDOW_FACTOR = 4;  // how far ahead is looked, in vbv-len
// The most output held back at once, so that a stream whose decoding times stall cannot hold the whole of it.
static constexpr int64_t SEND_AHEAD_HOLD_LIMIT = 64LL * 1024 * 1024;

void TSMuxer::writeBlockToOwner(uint8_t* data, const int len)
{
    if (m_owner->isAsyncMode())
        m_owner->asyncWriteBuffer(this, data, len, m_muxFile);
    else
    {
        m_owner->syncWriteBuffer(this, data, len, m_muxFile);
        delete[] data;
    }
}

// A block of output whose times may still change. It takes the block (allocated with new[]).
void TSMuxer::sendAheadHold(uint8_t* data, const int len)
{
    m_saHeld.push_back({data, len, m_saHeldEnd});
    m_saHeldEnd += len;
    sendAheadReleaseDone(false);
}

void TSMuxer::sendAheadReleaseDone(const bool all)
{
    while (!m_saHeld.empty() && (all || m_saHeld.front().pos + m_saHeld.front().len <= m_saDoneEnd))
    {
        writeBlockToOwner(m_saHeld.front().data, m_saHeld.front().len);
        m_saHeld.pop_front();
    }
}

// Writes len bytes at an output position that has not gone out yet: in a held block, in a block still waiting for its
// arrival timestamps, or in m_outBuf.
void TSMuxer::sendAheadPatch(int64_t pos, const uint8_t* src, int len)
{
    while (len > 0)
    {
        uint8_t* dst = nullptr;
        int64_t room = 0;
        for (const auto& b : m_saHeld)
            if (pos >= b.pos && pos < b.pos + b.len)
            {
                dst = b.data + (pos - b.pos);
                room = b.pos + b.len - pos;
                break;
            }
        if (!dst)
        {
            int64_t start = m_outBufPos;
            for (const auto& b : m_m2tsDelayBlocks) start -= b.second;
            for (const auto& b : m_m2tsDelayBlocks)
            {
                if (pos >= start && pos < start + b.second)
                {
                    dst = b.first + (pos - start);
                    room = start + b.second - pos;
                    break;
                }
                start += b.second;
            }
        }
        if (!dst)
        {
            if (pos < m_outBufPos || pos >= m_outBufPos + m_outBufLen)
                THROW(ERR_COMMON, "Internal error: output at " << pos << " to be retimed is no longer held")
            dst = m_outBuf + (pos - m_outBufPos);
            room = m_outBufPos + m_outBufLen - pos;
        }
        const int n = static_cast<int>(FFMIN(room, static_cast<int64_t>(len)));
        memcpy(dst, src, n);
        pos += n;
        src += n;
        len -= n;
    }
}

void TSMuxer::sendAheadRecordPass(const int64_t pcrVal, const int64_t floorPcr, const int64_t pcrGAP,
                                  const int64_t oldPrevHi, const bool hasPcrPacket)
{
    SendAheadPass p{};
    p.endPos = m_outBufPos + m_outBufLen;
    p.frames = (p.endPos - m_saRecEnd) / m_frameSize;
    p.oldPcr = pcrVal;
    p.oldPrevHi = oldPrevHi;
    p.pcrGap = pcrGAP;
    p.bits = m_saPassBits;
    p.dtsClock = m_saDtsClock;
    p.muxDts = m_saMuxDts;
    p.hasPcrPacket = hasPcrPacket;
    p.first = m_saFirstPass;
    p.idle = FFMAX(int64_t(0), pcrVal - floorPcr);
    m_saFirstPass = false;
    m_saRecEnd = p.endPos;
    m_saRecOldHi = pcrVal * 300 - pcrGAP;
    m_saPasses.push_back(p);
}

void TSMuxer::sendAheadNotePes()
{
    if (m_lastPESDTS == -1)
        return;
    const auto it = m_pmt.pidList.find(m_pesPID);
    const bool video = it != m_pmt.pidList.end() && isVideoStreamType(it->second.m_streamType);
    m_saPes.push_back({m_outBufPos + m_outBufLen, m_lastPESDTS, video});
}

// Where the window ahead of the pass at the front is measured from. Where the cap holds the data back, a lateness
// is carried on from pass to pass, so a picture late at the end of such a run can only be helped before the run
// began. A pass after which the link has stood idle for vbv-len in all can make up that much on its own, so the
// window starts there. False while the passes after the front one have not left that much idle yet.
bool TSMuxer::sendAheadReach(int64_t* dts) const
{
    int64_t idle = 0;
    for (size_t i = 1; i < m_saPasses.size(); i++)
    {
        idle += m_saPasses[i].idle;
        if (idle >= m_vbvLen)
        {
            *dts = m_saPasses[i].muxDts;
            return true;
        }
    }
    return false;
}

void TSMuxer::sendAheadFinalizeFront()
{
    int64_t reach = 0;
    const bool reachKnown = sendAheadReach(&reach);
    const SendAheadPass p = m_saPasses.front();
    m_saPasses.pop_front();
    const int64_t start = m_saDoneEnd;
    if (p.frames == 0)
    {
        // A block that ends right after a PCR carries no packet, only a time, which keeps its distance to the pass
        // before.
        m_saPrevHi += p.oldPcr * 300 - p.pcrGap - p.oldPrevHi;
        return;
    }
    const double floorGap = atsGapFloor(m_maxBitrate);  // 27 MHz per packet at the cap
    const auto frames = static_cast<double>(p.frames);
    int64_t prevHi = m_saPrevHi;
    if (p.first)
        prevHi =
            m_m2tsMode ? p.oldPrevHi : p.oldPcr * 300 - p.pcrGap - static_cast<int64_t>(std::ceil(frames * floorGap));

    // The latest end this pass may have so that each PES seen in the window ahead arrives by its decoding time: one
    // that ends inside the pass arrives with the packet after its last, spread evenly across the pass; one after it
    // with the packets after the pass at the cap. Every PCR after this one is rounded up to the 90 kHz clock, which
    // the packets after it are given a thousandth more than the cap's time for.
    const int64_t window = m_vbvLen * SEND_AHEAD_WINDOW_FACTOR;
    const double aheadGap = floorGap * 1.001;
    double latest = std::numeric_limits<double>::infinity();
    for (const auto& e : m_saPes)
    {
        if (e.endPos <= start || (reachKnown && e.dts > reach + window))
            continue;
        const double due = static_cast<double>(e.dts) * 300.0;
        const auto n = static_cast<double>((e.endPos - start) / m_frameSize);
        const double bound = n < frames
                                 ? static_cast<double>(prevHi) + (due - static_cast<double>(prevHi)) * frames / (n + 1)
                                 : due - (n + 1 - frames) * aheadGap;
        latest = FFMIN(latest, bound);
    }
    int64_t cand = p.oldPcr;
    if (latest < std::numeric_limits<double>::infinity())
    {
        const auto latestPcr = static_cast<int64_t>(std::floor((latest + static_cast<double>(p.pcrGap)) / 300.0));
        cand = FFMIN(cand, FFMAX(latestPcr, p.dtsClock - m_vbvLen));
    }
    // A pass that follows one made earlier may not stretch past the 0.1 s a PCR interval is allowed to take.
    if (!p.first)
        cand = FFMIN(cand, m_saPrevPcr + FFMAX(PCR_MAX_INTERVAL - PCR_INTERVAL_MARGIN, p.oldPcr - m_saPrevOldPcr));
    int64_t newPcr = p.oldPcr;
    if (cand < p.oldPcr)
    {
        // never faster than the cap after the pass before: the same floor pcrForMaxRate holds
        int64_t lo = 0;
        if (!p.first)
        {
            if (m_m2tsMode)
                lo = (prevHi + p.pcrGap + static_cast<int64_t>(std::ceil(frames * floorGap)) + 299) / 300;
            else
                lo =
                    m_saPrevPcr + static_cast<int64_t>(std::ceil(static_cast<double>(p.bits) * 90000.0 / m_maxBitrate));
        }
        newPcr = FFMAX(lo, cand);
        if (p.first)
            prevHi -= (p.oldPcr - newPcr) * 300;
    }
    const int64_t newHi = newPcr * 300 - p.pcrGap;
    const double inc = static_cast<double>(newHi - prevHi) / frames;

    if (newPcr != p.oldPcr || prevHi != p.oldPrevHi)
    {
        if (m_m2tsMode)
        {
            // the arrival timestamps, computed as processM2TSPCR does
            auto cur = static_cast<double>(prevHi);
            for (int64_t i = 0; i < p.frames; i++)
            {
                cur += inc;
                uint8_t ats[4];
                writeM2TSHeader(ats, llround(cur));
                sendAheadPatch(start + i * m_frameSize, ats, 4);
            }
        }
        if (p.hasPcrPacket && newPcr != p.oldPcr)
        {
            // the PCR field, as setPCR33 writes it
            uint8_t pcr[6];
            const auto base = static_cast<uint32_t>(newPcr >> 1);
            pcr[0] = static_cast<uint8_t>(base >> 24);
            pcr[1] = static_cast<uint8_t>(base >> 16);
            pcr[2] = static_cast<uint8_t>(base >> 8);
            pcr[3] = static_cast<uint8_t>(base);
            pcr[4] = static_cast<uint8_t>((newPcr & 1) << 7 | 0x7e);
            pcr[5] = 0;
            const int64_t pcrPos = p.endPos - m_frameSize + (m_m2tsMode ? 4 : 0) + TSPacket::TS_HEADER_SIZE +
                                   AdaptiveField::ADAPTIVE_FIELD_LEN;
            sendAheadPatch(pcrPos, pcr, 6);
        }
    }
    if (m_m2tsMode)
        noteAtsPass(static_cast<int>(p.frames), inc);

    // the pictures that end in this pass, with the times they now have
    while (!m_saPes.empty() && m_saPes.front().endPos <= p.endPos)
    {
        const SendAheadPes e = m_saPes.front();
        m_saPes.pop_front();
        if (!e.video || e.endPos <= start)
            continue;
        m_cappedPictures++;
        const auto n = static_cast<double>((e.endPos - start) / m_frameSize);
        const double arrival = static_cast<double>(prevHi) + (n + 1) * inc;
        const double late = arrival - static_cast<double>(e.dts) * 300.0;
        if (late > 0)
        {
            m_latePictures++;
            m_worstLateness = FFMAX(m_worstLateness, static_cast<int64_t>(std::ceil(late / 300.0)));
        }
    }
    m_saDoneEnd = p.endPos;
    m_saPrevHi = newHi;
    if (p.hasPcrPacket)
    {
        m_saPrevPcr = newPcr;
        m_saPrevOldPcr = p.oldPcr;
    }
}

// Called at each new PES: makes final the passes the decoding times have moved past by the window.
void TSMuxer::sendAheadAdvance(const int64_t muxDts)
{
    const int64_t window = m_vbvLen * SEND_AHEAD_WINDOW_FACTOR;
    while (!m_saPasses.empty())
    {
        int64_t reach = 0;
        if (m_saRecEnd - m_saDoneEnd <= SEND_AHEAD_HOLD_LIMIT && !(sendAheadReach(&reach) && reach + window < muxDts))
            break;
        sendAheadFinalizeFront();
    }
    sendAheadReleaseDone(false);
}

void TSMuxer::sendAheadFinishAll()
{
    while (!m_saPasses.empty()) sendAheadFinalizeFront();
    // A transport stream may end with packets after its last PCR. Their pictures arrive at the cap after it.
    const double floorGap = atsGapFloor(m_maxBitrate);
    for (const auto& e : m_saPes)
    {
        if (!e.video || e.endPos <= m_saDoneEnd)
            continue;
        m_cappedPictures++;
        const auto n = static_cast<double>((e.endPos - m_saDoneEnd) / m_frameSize);
        const double late = static_cast<double>(m_saPrevHi) + (n + 1) * floorGap - static_cast<double>(e.dts) * 300.0;
        if (late > 0)
        {
            m_latePictures++;
            m_worstLateness = FFMAX(m_worstLateness, static_cast<int64_t>(std::ceil(late / 300.0)));
        }
    }
    m_saPes.clear();
    sendAheadReleaseDone(true);
}

// A disc player starts and seeks at key pictures only, the entries of the clip's seek index. A stream made with intra
// refresh has a key picture at its start only: a disc of a minute of it had one entry, so nothing in it could be
// reached by a chapter or a seek, and nothing said so.
void TSMuxer::noteKeyGap(const int pid, const int64_t pts, const bool key)
{
    if (!m_bluRayMode)
        return;
    KeyGap& gap = m_keyGaps[pid];
    // as the seek index takes them: a key picture counts when it is later than the last one
    if (key && pts > gap.lastKey)
    {
        if (gap.lastKey != -1)
            gap.longest = (std::max)(gap.longest, pts - gap.lastKey);
        gap.lastKey = pts;
    }
    gap.lastPts = (std::max)(gap.lastPts, pts);
}

void TSMuxer::reportKeyPictureGaps()
{
    if (m_keyGapsReported)
        return;
    m_keyGapsReported = true;
    for (const auto& [pid, gap] : m_keyGaps)
    {
        const int64_t longest = (std::max)(gap.longest, gap.lastKey != -1 ? gap.lastPts - gap.lastKey : int64_t{0});
        if (longest > 10 * 90000)
            LTRACE(LT_WARN, 2,
                   "Warning: the video on PID 0x" << std::hex << pid << std::dec << " goes "
                                                  << doubleToStr(static_cast<double>(longest) / 90000.0, 1)
                                                  << " s without a key picture. A disc player starts and seeks at key "
                                                     "pictures only, so chapters and seeks there do not work. Encode "
                                                     "the video with key pictures, such as one a second (a stream made "
                                                     "with intra refresh has one only at its start).");
    }
}

bool TSMuxer::close()
{
    reportReadRate();
    reportLateArrival();
    reportCbrExcess();
    reportKeyPictureGaps();

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

    // The file is opened again only for what is left in the buffer. With nothing left over it was closed a second
    // time, which fails on Linux and macOS (a closed descriptor), so an output that ended on a whole block, as an empty
    // one or a --cut-end whose last packet filled the buffer exactly does, was reported as "Failed to write the end of
    // the output".
    if (m_outBufLen == 0)
        return true;
    if (!m_muxFile->open(m_outFileName.c_str(), AbstractOutputStream::ofWrite + AbstractOutputStream::ofAppend))
        return false;
    if (!writeOutFile(m_outBuf, m_outBufLen))
        return false;
    return m_muxFile->close();
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
        // microseconds, four packets 148 ticks apart, 274 Mbit/s, every few seconds on every Blu-ray. The disc
        // format allows nothing below the read-rate floor even there. Such a short pass is now held at the floor the
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
    // Nothing since the last pass. In 3D that is a block that ends right after a PCR, and the time the block's end is
    // given can be far behind this view's held clock, which may not go back to it.
    if (frames <= 0)
        return m_interliaveBlockSize > 0 ? FFMAX(newPCR, (m_prevM2TSPCR + pcrGAP + 299) / 300) : newPCR;
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
    if (!m_sendAhead)  // its times are not final yet: counted in sendAheadFinalizeFront
        noteAtsPass(m2tsFrameCnt, pcrIncPerFrame);

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
            if (m_sendAhead)
                sendAheadHold(i.first, i.second);
            else if (m_owner->isAsyncMode())
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

// The read rate a pass of arrival timestamps asks for, for reportReadRate. See processM2TSPCR.
void TSMuxer::noteAtsPass(const int frames, const double gap)
{
    if (frames <= TABLE_BURST_PACKETS || gap <= 0)
        return;
    if (m_minAtsGap == 0 || gap < m_minAtsGap)
        m_minAtsGap = gap;
    m_atsPackets += frames;
    if (gap < atsGapFloor(isV3() ? BD_READ_RATE_UHD : BD_READ_RATE_HD))
        m_atsOverLimit += frames;
}

// The continuity counter of a packet that carries only a PCR. A packet without payload repeats the counter of the last
// packet of its PID (ISO/IEC 13818-1, 2.4.3.3). m_tsCnt holds the counter of the NEXT packet, so a PCR packet on the
// video PID took the counter of the payload packet after it, and every PCR cost two continuity errors there. A PCR
// PID of its own, also that of a mux without video, carries no payload at all and keeps the counter it always had.
int TSMuxer::pcrOnlyPacketCounter()
{
    const int pid = m_pmt.pcr_pid;
    if (pid == DEFAULT_PCR_PID)
        return m_streamInfo[pid].m_tsCnt & 15;
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
    if (m_sendAhead)
        sendAheadRecordPass(pcrVal, m_saFloorPcr, 0, m_m2tsMode ? m_prevM2TSPCR : m_saRecOldHi, true);
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
        // A key picture that comes exactly at the duration starts the next part, as one at a chapter does below. With
        // ">" it went into the part it ends, and the part waited for the next key picture: a split every 2 s of video
        // with a key picture every second gave parts of 3 s.
        return (avPacket.pts - m_curFileStartPts) >= m_splitDuration;
    }
    if (m_splitByChapters && m_nextChapter < m_chapterPts.size())
    {
        // Measured from the first packet of the mux, not from the start of the current part: a
        // chapter is a position in the film, and the parts before it have already been written.
        // m_firstPts[0] is that first packet, and it is not zero, because a transport stream is
        // given an offset before anything is muxed. The chapters of a --cut-start are counted from the cut, which with
        // --cut-pictures=cover is after the first picture (setChapterOrigin).
        if (m_chapterOrigin >= 0)
            return avPacket.pts - m_chapterOrigin >= m_chapterPts[m_nextChapter];
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
        // A clip name has five digits, and a playlist names a clip by them. A split that went past 99999 wrote a part
        // named 100000.m2ts, and the playlist pointed at clip 10000, which does not exist.
        if (origNum > 99999)
            THROW(ERR_COMMON, "The split needs clip number " << origNum
                                                             << ", and a disc clip has five digits at most. Give a "
                                                                "lower --m2tsOffset or larger parts.")
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

// A track's seek index has a list of entries for each part of a split output, and the clip information of part n reads
// list n. A new part adds a list to every index that has begun; an index began with one list when its first entry came.
// When that was in a later part, as when a split of a few bytes came before the first picture, the lists were one part
// behind: a part's entries went into the clip information of the part before. The index now begins with a list for
// each part so far.
void TSMuxer::startIndexLists(PMTStreamInfo& streamInfo) const
{
    const size_t parts = (std::max)(m_fileNames.size(), static_cast<size_t>(1));
    if (streamInfo.m_index.size() < parts)
        streamInfo.m_index.resize(parts);
}

void TSMuxer::gotoNextFile(const int64_t newPts)
{
    // Step past every chapter this part has already reached. More than one can fall inside a
    // single part when two marks sit closer together than the next key frame, and stepping past
    // only one would split again immediately on the next frame.
    if (m_splitByChapters && !m_firstPts.empty() && m_firstPts[0] != -1)
    {
        const int64_t origin = m_chapterOrigin >= 0 ? m_chapterOrigin : m_firstPts[0];
        while (m_nextChapter < m_chapterPts.size() && newPts - origin >= m_chapterPts[m_nextChapter]) m_nextChapter++;
    }

    // 2. CloseCurrentFile
    if (m_owner->isAsyncMode())
        m_owner->waitForWriting();

    m_muxFile->close();
    assert(m_outBufLen == 0);
    // The object of the closed part was kept for the whole mux, one for each part. It is freed here, except with a
    // second view of 3D video beside this muxer: there its file object can still be in use, by a .ssif written directly
    // (the second view writes through this one's, joinToMasterFile) and by this view's blocks held back for the
    // interleave (MuxerManager::asyncWriteBuffer).
    if (!m_isExternalFile && !m_sublingMuxer)
        delete m_muxFile;
    m_muxFile = nullptr;

    // 3. open new file
    // Each part of a disc is a play item of the one playlist, and a playlist holds at most 999 play items. A split into
    // more parts wrote a playlist of 1000 or more, which no player reads, at exit 0.
    if (m_bluRayMode && m_fileNames.size() >= 999)
        THROW(ERR_COMMON,
              "The split needs more than 999 parts, and a disc playlist holds at most 999 play items. Give "
              "a larger --split-duration or --split-size.")
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
        startIndexLists(streamInfo);
        const auto vCodec = streamInfo.m_mpegReader;
        const bool SPSRequired = streamInfo.m_codecReader->needSPSForSplit();
        const auto aCodec = streamInfo.m_audioReader;
        bool updateIdx = false;
        if (vCodec)
            noteKeyGap(streamInfo.m_pid, static_cast<int64_t>(curPts), m_pesIFrame);
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

            startIndexLists(streamInfo);
            size_t idxSize = streamInfo.m_index.size();
            const auto vCodec = streamInfo.m_mpegReader;  // cached at intAddStream
            // bool isH264 = dynamic_cast <H264StreamReader*> (streamInfo.m_codecReader);
            const bool SPSRequired = streamInfo.m_codecReader->needSPSForSplit();
            const auto aCodec = streamInfo.m_audioReader;  // cached at intAddStream
            if (vCodec)
                noteKeyGap(streamInfo.m_pid, static_cast<int64_t>(curPts), m_pesIFrame);
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

// The null packets that hold --bitrate or --minbitrate from the last PCR up to newPCR. Written right before the PCR,
// and also where a block of 3D or a part of a split ends (finishFileBlock): the interval's padding came after the end,
// in the next block or part behind its PAT and PMT, and was given the arrival times of the few ticks between the two,
// 360 packets 1 or 2 ticks apart at the start of each part of a split at --bitrate=10000, and runs of equal times in
// both views of a 3D disc. A second call in the same interval finds the floor reached and writes nothing; the bits
// the tables after the padding add to it are carried as what the padding wrote beyond the floor.
void TSMuxer::writeFloorPadding(const int64_t newPCR)
{
    if (m_cbrBitrate == -1 || m_minBitrate == -1 || m_lastPCR == -1)
        return;
    int expectedBits = lroundl(static_cast<double>(newPCR - m_lastPCR) / 90000.0 * m_minBitrate);
    expectedBits -= m_pcrBits;
    if (expectedBits > 0)
    {
        int tsFrames = expectedBits / 8 / m_frameSize;
        if (tsFrames * m_frameSize * 8 < expectedBits)
            tsFrames++;
        writeNullPackets(tsFrames);
        m_floorPadBits += static_cast<int64_t>(tsFrames) * m_frameSize * 8;
        if (m_cbrBitrate == m_minBitrate)
            m_floorPadRest = (tsFrames * m_frameSize * 8) - expectedBits;
    }
    else if (m_floorPadBits > 0 && m_cbrBitrate == m_minBitrate)
        m_floorPadRest = -expectedBits;
}

void TSMuxer::writePCR(const int64_t newPCR)
{
    writeFloorPadding(newPCR);
    const int bitsRest = m_floorPadRest;
    const int64_t paddingBits = m_floorPadBits;
    m_floorPadRest = 0;
    m_floorPadBits = 0;
    // One more packet than the buffer holds, because writeEmptyPacketWithPCR is about to add the
    // PCR packet and then run the arrival pass over the lot. Computed before m_pcrBits is cleared,
    // because the transport stream branch of it reads that count.
    const int64_t pcr = pcrForMaxRate(newPCR, 1, 0);
    m_saPassBits = m_pcrBits;
    if (m_sendAhead)
        m_saFloorPcr = pcrForMaxRate(0, 1, 0);
    noteCbrRate(pcr, paddingBits);
    m_pcrBits = bitsRest;
    m_cbrCarryBits = bitsRest;
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
    m_outBufPos += m_outBufLen;
    m_saHeldEnd = m_outBufPos;
    m_outBufLen = 0;
}

void TSMuxer::finishFileBlock(const int64_t newPts, const int64_t newPCR, const bool doChangeFile, const bool recursive)
{
    if (m_processedBlockSize > 0)
    {
        constexpr auto gapForPATPMT = static_cast<int64_t>((192 * 4.0) / 35000000.0 * 27000000.0);
        writeFloorPadding(newPCR);
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

void TSMuxer::warnV3Video(MPEGStreamReader* video)
{
    const unsigned height = video->getStreamHeight();
    if (height > 0 && height < 1080)  // 0: never read, said nothing about
        LTRACE(LT_WARN, 2, "Warning: video height < 1080 is not standard for V3 Blu-ray.");
    if (video->getInterlaced())
        LTRACE(LT_WARN, 2, "Warning: interlaced video is not standard for V3 Blu-ray.");
}

// A disc carries its main video in a few sizes and rates only. 640x360, 720p at 25, 576p at 50, H.264 at 2160 lines,
// and HEVC at 120 or 240 pictures a second or at 7680x4320 went onto a disc without a word, with a frame rate code of 0
// or a size code the picture does not have. The disc is written as asked; the warning says that a player may not play
// it. Heights below 1080 and interlaced video on a UHD disc have their warnings in warnV3Video.
void TSMuxer::warnDiscVideo(MPEGStreamReader* video)
{
    const unsigned width = video->getStreamWidth();
    const unsigned height = video->getStreamHeight();
    const double fps = video->getFPS();
    if (width == 0 || height == 0 || fps <= 0)
        return;
    const auto rateIn = [fps](std::initializer_list<double> rates)
    { return std::any_of(rates.begin(), rates.end(), [fps](const double r) { return std::fabs(fps - r) < 0.01; }); };
    const bool hevc = dynamic_cast<HEVCStreamReader*>(video) != nullptr;
    std::string why;
    if (hevc && isV3())
    {
        if (width > 3840 || height > 2160)
            why = "a UHD disc carries at most 3840x2160";
        else if (!rateIn({23.976, 24, 25, 29.97, 50, 59.94, 60}))
            why = "a UHD disc carries 23.976, 24, 25, 29.97, 50 or 59.94 pictures a second";
    }
    else if (height > 1080)
        why =
            "a Blu-ray disc carries H.264, MPEG-2 and VC-1 video up to 1920x1080, and a UHD disc carries 2160 lines "
            "as HEVC only";
    else if (!((width == 1920 || width == 1440) && height == 1080) && !(width == 1280 && height == 720) &&
             !(width == 720 && (height == 576 || height == 480)))
        why = "the picture sizes of a Blu-ray disc are 1920x1080, 1440x1080, 1280x720, 720x576 and 720x480";
    else if (height == 720 && !rateIn({23.976, 24, 50, 59.94}))
        why = "720p on a Blu-ray disc is 23.976, 24, 50 or 59.94 pictures a second";
    else if (height == 576 && !rateIn({25}))
        why = "576 lines on a Blu-ray disc are 25 pictures a second (interlaced)";
    else if (height == 480 && !rateIn({23.976, 29.97}))
        why = "480 lines on a Blu-ray disc are 29.97 pictures a second (interlaced), or 23.976 with pulldown";
    else if (height == 1080 && !rateIn({23.976, 24, 25, 29.97, 50, 59.94}))
        why = "1080 lines on a Blu-ray disc are 23.976, 24, 25, 29.97, 50 or 59.94 pictures a second";
    if (why.empty())
        return;
    LTRACE(LT_WARN, 2,
           "Warning: the main video is " << width << "x" << height << " at " << doubleToStr(fps, 3)
                                         << " pictures a second, which is not a format of the disc: " << why
                                         << ". The disc is written as asked, and a disc player may not play it.");
}

bool TSMuxer::muxPacket(AVPacket& avPacket)
{
    if (avPacket.data == nullptr || avPacket.size == 0)
    {
        return true;
    }

    if (m_v3VideoPending && avPacket.stream_index == m_v3VideoPendingIndex)
    {
        warnV3Video(m_v3VideoPending);
        m_v3VideoPending = nullptr;
    }
    if (m_discVideoPending && avPacket.stream_index == m_discVideoPendingIndex)
    {
        warnDiscVideo(m_discVideoPending);
        m_discVideoPending = nullptr;
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
            // The clock starts --vbv-len before the first decoding time. It started that long before the first
            // presentation time, but video with B frames is decoded earlier than it is shown: with a small --vbv-len
            // the first decoding times came before the clock, and those pictures were late. Every PCR moves back by
            // that shift; the presentation and decoding times stay as they were.
            const int64_t firstDtsShift = internalClockToPts(avPacket.dts);
            m_fixed_pcr_offset = m_timeOffset - m_vbvLen + firstDtsShift;
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
        // see sendAheadAdvance
        m_sendAhead = m_maxBitrate != -1 && m_minBitrate <= 0 && m_vbvLen > 0 && m_interliaveBlockSize == 0 &&
                      !m_subMode && !m_masterMode;
    }

    if (m_minDts == -1)
        m_minDts = avPacket.dts;

    auto newPCR = (avPacket.dts - m_minDts) / INT_FREQ_TO_TS_FREQ + m_fixed_pcr_offset;
    m_saDtsClock = newPCR;
    m_saMuxDts = internalClockToPts(avPacket.dts) + m_timeOffset;
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
        if (m_sendAhead)
            sendAheadAdvance(m_saMuxDts);
    }

    m_lastTSIndex = tsIndex;

    // ** THE CLOCK GOES ON BETWEEN TWO PACKETS FAR APART. ** A PCR was only ever written at a packet of a stream, so
    // where two came more than 0.1 s apart, so did two PCRs: every step was 125 ms for video at 8 fps, and a stream
    // of subtitles alone went 12 s without one. Packets that carry only a PCR now fill such a gap, each at most 0.09 s
    // after the one before.
    if (newPES && m_lastPCR != -1 && !m_inplacePending &&
        (m_interliaveBlockSize == 0 || avPacket.stream_index == m_mainStreamIndex))
    {
        constexpr int64_t PCR_FILL_STEP = PCR_MAX_INTERVAL - PCR_MAX_INTERVAL / 10;
        while (newPCR - m_lastPCR > PCR_MAX_INTERVAL)
        {
            const int64_t step = m_lastPCR + PCR_FILL_STEP;
            writePATPMT(step);
            writePCR(step);
        }
    }

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
// before.
//
// At a cap below about 256 kbps that can still be too late. PAT, PMT and SIT go out with the PCR and count in its
// time, and at 150 kbps each packet takes 10 ms, so the eight packets that reach m_pcr_delta and those three tables
// put the PCRs 0.11 s apart. A PCR is therefore also due when the next packet and the tables would take the time
// past 0.1 s, counted as pcrForMaxRate counts it, on the 188 byte packet. It can only come first where the first
// condition with all the tables would pass 0.1 s. Everywhere else the output is the same as before.
// The tables are counted in the packets they are written in: the PMT of six or more audio tracks takes two packets or
// more (m_pmtFrames, four for 32 AC-3 tracks, five for 30 with a video), and counted as one, the PCRs were again up
// to 0.17 s apart (30 tracks at 91 kbps), and still 0.1053 s at 400 kbps. PAT and SIT are one packet each. They are
// counted where they are due with the next PCR (with every PCR of an M2TS, every 0.25 s in a TS, the SIT less often),
// and never as fewer than the three counted before, so where the PMT takes one packet the output is the same as
// before.
// Where the tables alone take longer than 0.1 s at the cap, eight packets at 91 kbps, no PCR before them can help:
// writePAT, writePMT and writeSIT put a PCR between their own packets then (pcrInsideTablesIfDue).
//
// 3D written in interleaved blocks was left out of this at first, and there the held clock got a PCR only at a block
// change or at a picture of the view's video: 0.12 s apart at --maxbitrate=5000 and seconds apart below. Each view is
// its own transport stream with its own clock, held at the cap by pcrForMaxRate the same way, and these PCRs follow
// that clock exactly, so they only fill in between the PCRs it was given before. Their packets count in the blocks
// like any other. A mux that stays under the cap is written exactly as before.
bool TSMuxer::maxRateClockDue() const
{
    if (m_maxBitrate == -1 || m_lastPCR == -1)
        return false;
    if (static_cast<int64_t>(std::ceil(static_cast<double>(m_pcrBits) * 90000.0 / m_maxBitrate)) >= m_pcr_delta)
        return true;
    // the next PCR comes within PCR_MAX_INTERVAL of the last one, and writePATPMT decides on it so
    const int64_t nextPCR = m_lastPCR + PCR_MAX_INTERVAL;
    const bool pmtDue = nextPCR - m_lastPMTPCR >= m_patPmtDelta;
    const bool sitDue = pmtDue && nextPCR - m_lastSITPCR > SIT_INTERVAL;
    const int64_t tables = pmtDue ? 1 + m_pmtFrames + (sitDue ? 1 : 0) : 0;
    // the next one and the tables
    const double packets =
        static_cast<double>(m_pcrBits) / (m_frameSize * 8) + static_cast<double>((std::max)(int64_t{4}, 1 + tables));
    return static_cast<int64_t>(std::ceil(packets * TS_PACKET_BITS * 90000.0 / m_maxBitrate)) > PCR_MAX_INTERVAL;
}

// Before each packet of the tables: a PCR first where this packet would take the time since the last one past 0.1 s
// at the cap. That happens only where the tables alone take that long, a PMT of many tracks at a very low
// --maxbitrate; elsewhere maxRateClockDue has put the PCR before the tables, and nothing is written here.
void TSMuxer::pcrInsideTablesIfDue()
{
    if (m_maxBitrate == -1 || m_lastPCR == -1)
        return;
    const double packets = static_cast<double>(m_pcrBits) / (m_frameSize * 8) + 1.0;  // with this one
    if (static_cast<int64_t>(std::ceil(packets * TS_PACKET_BITS * 90000.0 / m_maxBitrate)) <= PCR_MAX_INTERVAL)
        return;
    ensureOutBufSpace(static_cast<int>(5 + m_pmtFrames) * m_frameSize);  // this PCR, the tables left and the next PCR
    writePCR(m_lastPCR);
    if (m_inplacePending)
        m_midPesPCR = true;
}

// The tables and the PCR go in between two packets of a picture. Inside a picture written in place, the PCR that the
// picture times would ask for is then left out for the rest of that picture (writeFrame), because its tables would
// be moved in front of the picture's packets that already carry arrival times from this one.
void TSMuxer::insertHeldPCRIfDue()
{
    if (!maxRateClockDue())
        return;
    ensureOutBufSpace(static_cast<int>(4 + m_pmtFrames) * m_frameSize);  // PAT, the PMT, SIT and the PCR
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
                    m_cbrWinBits += m_pcrBits - m_cbrCarryBits;  // counted before the count is dropped
                    m_cbrCarryBits = 0;
                    m_pcrBits = 0;
                    writePATPMT(newPCR);
                    writePCR(newPCR);
                    // The clock has passed the decoding time of the data still being written: at the constant
                    // rate that data comes after it is due, so the decoder's buffer runs empty there (an underflow).
                    // This was reported as "VBV buffer overflow", the opposite of what happens.
                    if (m_lastPESDTS != -1 && m_lastPCR > m_lastPESDTS)
                    {
                        LTRACE(LT_ERROR, 2,
                               "At " << (double)(m_lastPCR - m_fixed_pcr_offset) / 90000.0
                                     << " s the stream needs more than --bitrate=" << m_cbrBitrate / 1000
                                     << " kbps, so its data arrives " << latenessText(m_lastPCR - m_lastPESDTS)
                                     << " after it is due and a player may stutter or freeze there. Give a "
                                        "higher --bitrate.");
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
    // H.222.0 2.4.4.9: the section of a PMT is at most 1021 bytes long. About 43 AC-3 tracks fill it, and with more the
    // stream was written with a longer section, which a reader that follows the standard rejects. The table is built
    // as each track is added, before the output is opened, so the mux stops here before anything is written.
    const uint8_t* section = m_pmtBuffer + TSPacket::TS_HEADER_SIZE + 1;  // after the pointer field
    const int sectionLength = ((section[1] & 0x0f) << 8) | section[2];
    // The table is checked as it grows, so the count is of the tracks added so far, not of all of them: "With 59
    // tracks" was said of 65.
    if (sectionLength > 1021)
        THROW(ERR_COMMON, "The tracks do not fit into one transport stream: with "
                              << m_pmt.pidList.size() << " of them the program map table already needs "
                              << sectionLength
                              << " bytes, and a transport stream holds at most 1021 in one. Leave some out.")
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
    pcrInsideTablesIfDue();
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
        pcrInsideTablesIfDue();
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
    pcrInsideTablesIfDue();
    if (m_m2tsMode)
    {
        m_outBufLen += 4;
        m_processedBlockSize += 4;
        m_pcrBits += 4 * 8;
    }
    uint8_t* sit = m_outBuf + m_outBufLen;
    memcpy(sit, DefaultSitTableOne, TS_FRAME_SIZE);
    // ** ON A DISC THE SIT GIVES THE RATE OF THE CLIP INFORMATION. ** The peak rate of the SIT and the
    // TS_recording_rate of the clip information are one value on a disc, clip by clip. The SIT was a fixed packet of
    // 48 Mbit/s, set to 127.9 for any HEVC track, so a disc with HEVC gave 127.9 here and 109 or 48 in its clip
    // information, and the second view of a 3D disc 48 here and 35 there. Both come from discRecordingRate now. Outside
    // a disc there is no clip information, and the packet stays as it was. The HEVC value was written into the shared
    // table, so it reached the outputs that came after it in the same run; it belongs to this muxer now.
    const int peakRate = m_bluRayMode ? discRecordingRate() / 400 : m_sitHevc ? SIT_PEAK_RATE_HEVC : SIT_PEAK_RATE;
    sit[17] = static_cast<uint8_t>(0xc0 | (peakRate >> 16));  // 2 reserved bits, 22 bits of peak_rate
    sit[18] = static_cast<uint8_t>(peakRate >> 8);
    sit[19] = static_cast<uint8_t>(peakRate);
    // the CRC of the section, from table_id to the end of the program loop
    const uint32_t crc = calculateCRC32(sit + 5, 24);
    sit[29] = static_cast<uint8_t>(crc >> 24);
    sit[30] = static_cast<uint8_t>(crc >> 16);
    sit[31] = static_cast<uint8_t>(crc >> 8);
    sit[32] = static_cast<uint8_t>(crc);
    const auto tsPacket = reinterpret_cast<TSPacket*>(sit);
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
        if (m_sendAhead)
        {
            // Held until its times are final (sendAheadAdvance). In an M2TS a block whose arrival timestamps are
            // not written yet waits for the next PCR first, as it always has.
            const auto block = new uint8_t[toFileLen];
            memcpy(block, m_outBuf, toFileLen);
            if (!m_m2tsMode || (m_prevM2TSPCROffset >= toFileLen && m_m2tsDelayBlocks.empty()))
            {
                sendAheadHold(block, toFileLen);
                if (m_m2tsMode)
                    m_prevM2TSPCROffset -= toFileLen;
            }
            else
                m_m2tsDelayBlocks.emplace_back(block, toFileLen);
            memmove(m_outBuf, m_outBuf + toFileLen, m_outBufLen - toFileLen);
        }
        else if (m_owner->isAsyncMode())
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
        m_outBufPos += toFileLen;
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
// Any unit it did not know was read as bytes: --split-size=4G gave parts of 4 bytes, one per key frame, and 4.5e9
// the same. The units of --disc-size are read here too (G as GB, Gi as GiB), and anything else is refused.
int64_t checkedSplitSize(const std::string& value)
{
    const string s = strToUpperCase(trimStr(value));
    size_t i = 0;
    while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.')) ++i;
    const string number = s.substr(0, i);
    const string unit = s.substr(i);
    double coeff = 0;
    if (unit.empty() || unit == "B")
        coeff = 1;
    else if (unit == "K" || unit == "KB")
        coeff = 1e3;
    else if (unit == "KI" || unit == "KIB")
        coeff = 1024.0;
    else if (unit == "M" || unit == "MB")
        coeff = 1e6;
    else if (unit == "MI" || unit == "MIB")
        coeff = 1024.0 * 1024;
    else if (unit == "G" || unit == "GB")
        coeff = 1e9;
    else if (unit == "GI" || unit == "GIB")
        coeff = 1024.0 * 1024 * 1024;
    else if (unit == "T" || unit == "TB")
        coeff = 1e12;
    else if (unit == "TI" || unit == "TIB")
        coeff = 1024.0 * 1024 * 1024 * 1024;
    if (coeff == 0 || number.find_first_of("0123456789") == string::npos ||
        std::count(number.begin(), number.end(), '.') > 1)
        THROW(ERR_COMMON, "Invalid --split-size value \""
                              << value
                              << "\": write a number and a unit, such as 4GB, 4G, 700MiB or "
                                 "500MB (KB, MB, GB, TB, and KiB, MiB, GiB, TiB).")
    // Above what 64 bits hold the conversion wrapped, and 99999999999999G was refused as not above zero
    const double bytes = strToDouble(number.c_str()) * coeff;
    if (!(bytes < 9.2e18))
        THROW(ERR_COMMON, "Invalid --split-size value \"" << value
                                                          << "\": it is larger than any output can be. Leave the "
                                                             "split out, or give a size such as 4GB.")
    const auto splitBytes = static_cast<int64_t>(bytes);
    if (splitBytes <= 0)
        THROW(ERR_COMMON, "Invalid --split-size value \"" << value << "\": it must be above zero.")
    return splitBytes;
}

// The length of each part, in seconds when no unit is given. A transport stream read it as a whole number of
// seconds and took 0, 0.5 and -10 as "no split" without a word, and "1min" as 1 s; a Matroska output refused the
// first three. Both read it here now.
int64_t checkedSplitDuration(const std::string& value)
{
    int64_t ticks = 0;
    if (!parseTimeToTicks(value, "s", &ticks) || ticks <= 0)
        THROW(ERR_COMMON, "Invalid --split-duration value \"" << value
                                                              << "\": write a length above zero, in seconds such as "
                                                                 "600, or with a unit such as 10min or 1h.")
    return ticks;
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
        vector<string> paramPair = splitOption(trimStr(i));
        if (paramPair.size() < 2)
            continue;
        if (paramPair[0] == "--bitrate" || paramPair[0] == "--maxbitrate")
            checkedMuxRate(paramPair[0], paramPair[1]);
        else if (paramPair[0] == "--split-size")
            checkedSplitSize(paramPair[1]);
        else if (paramPair[0] == "--split-duration")
            checkedSplitDuration(paramPair[1]);
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
    // Split outside quotes, as the line is split everywhere else. Split at every space, the words of a quoted value
    // were read as options: --label="no --blu-ray here" put a transport stream into disc mode, named 00000.ts and
    // with its AC-3 as stream id 0xfd.
    const vector<string> params = splitQuotedStr(opts.c_str(), ' ');
    bool cbrAsked = false;
    bool vbrAsked = false;
    bool rateGiven = false;
    bool minRateGiven = false;
    int maxRate = -1;
    for (auto& i : params)
    {
        vector<string> paramPair = splitOption(trimStr(i));
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
            maxRate = checkedMuxRate(paramPair[0], paramPair[1]);
        else if (paramPair[0] == "--minbitrate" && paramPair.size() > 1)
        {
            setMinBitrate(static_cast<int>(strToDouble(paramPair[1].c_str()) * 1000.0));
            minRateGiven = true;
        }
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
            setSplitDuration(checkedSplitDuration(paramPair[1]));
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
            // A disc carries AC-3, E-AC-3, DTS and DTS-HD as stream id 0xfd with its extension, as --help says and as
            // the window always sets it for a disc. Only --new-audio-pes did it, so a disc from a meta file without it
            // had 0xbd, and a player that takes the extension to find the audio played it without sound.
            setNewStyleAudioPES(true);
            // The clock references of a disc go on a PID of their own, 0x1001, as the format has it and as the window
            // always writes it. On the command line they went on the last video track added, for a Dolby Vision disc
            // the enhancement layer.
            setPCROnVideoPID(false);
        }
        else if (paramPair[0] == "--cut-start" || paramPair[0] == "--cut-end")
        {
            m_computeMuxStats = true;
        }
    }
    // A disc keeps its HDMV descriptors, as the help says: its clip information is written for them. With
    // --no-hdmv-descriptors, a disc lost the HDMV registration of its program map table, and its MPEG-2 video the
    // HDMV descriptor its clip information matches.
    if (m_bluRayMode && !m_hdmvDescriptors)
    {
        m_hdmvDescriptors = true;
        LTRACE(LT_INFO, 2,
               "--no-hdmv-descriptors does not apply to a Blu-ray or AVCHD disc: it keeps its HDMV "
               "descriptors.");
    }
    // --maxbitrate also sets the rate the muxing clock runs at, and it did so over --bitrate when it came after it, as
    // the window writes it for a constant bitrate disc with the read rate tick. The size stayed the rate of --bitrate,
    // which the padding follows, but the remainder that padding carries from one PCR to the next was dropped. --bitrate
    // now keeps the clock whatever order the two come in; --maxbitrate stays the ceiling.
    if (maxRate != -1)
    {
        const int cbrRate = m_cbrBitrate;
        setMaxBitrate(maxRate);
        if (rateGiven)
            setMuxRate(cbrRate);
        // A ceiling below the constant rate holds the stream to the ceiling, and nothing said so: --bitrate=40000 with
        // --maxbitrate=20000 gave 20000. The padding still filled each gap between two PCRs to the higher rate, and
        // the ceiling then held the next PCR back until that padding could have arrived: --bitrate=20000 with
        // --maxbitrate=5000 put the PCRs 0.39 s apart, where 0.1 s is the most the rules allow. The constant rate and
        // its padding are lowered to the ceiling, as if --bitrate had given it.
        if (rateGiven && maxRate < cbrRate)
        {
            LTRACE(LT_WARN, 2,
                   "Warning: --maxbitrate (" << maxRate / 1000 << " kbps) is below --bitrate (" << cbrRate / 1000
                                             << " kbps), so the constant rate and its padding are lowered to "
                                             << maxRate / 1000 << " kbps.");
            setMuxRate(maxRate);
            setMinBitrate(maxRate);
        }
        // The same for a padding floor above the ceiling, which nothing said at all: --minbitrate=20000 with
        // --maxbitrate=10000 put the PCRs 0.18 s apart
        else if (!rateGiven && minRateGiven && m_minBitrate > maxRate)
        {
            LTRACE(LT_WARN, 2,
                   "Warning: --minbitrate (" << m_minBitrate / 1000 << " kbps) is above --maxbitrate ("
                                             << maxRate / 1000 << " kbps), so the padding is lowered to "
                                             << maxRate / 1000 << " kbps.");
            setMinBitrate(maxRate);
        }
    }
    m_cbrGiven = rateGiven;
    // --bitrate gives a constant rate, and --minbitrate after it lowered that rate without a word: 20000 became 10000.
    // The constant rate is kept, whatever order the two come in.
    if (rateGiven && minRateGiven)
    {
        setMinBitrate(maxRate != -1 ? (std::min)(m_cbrBitrate, maxRate) : m_cbrBitrate);
        LTRACE(LT_WARN, 2, "Warning: --minbitrate is ignored with --bitrate, which gives a constant rate.");
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
    // In an image the pieces of the two views of a 3D mux have to stay whole (IsoWriter::beforePiece)
    if (isInterleaveMode())
        if (const auto isoFile = dynamic_cast<ISOFile*>(m_muxFile))
            isoFile->setSubMode(true);
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

int64_t TSMuxer::clipPts(const int64_t internal) const
{
    return internal < 0 ? -1 : internalClockToPts(internal) + m_timeOffset;
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
    // The muxer of the second view of a 3D disc is given its file name twice (MuxerManager::preinitMux), and the
    // buffer of the first call was left behind. Nothing has been written into it yet at this point.
    delete[] m_outBuf;
    m_outBuf = new uint8_t[m_writeBlockSize + 1024];
    m_outBufCapacity = m_writeBlockSize + 1024;
    m_frameSize = m_m2tsMode ? 192 : 188;
    if (m_m2tsMode)
        m_sectorSize = 1024 * 6;
}

bool TSMuxer::isInterleaveMode() const { return m_masterMode || m_subMode; }

int TSMuxer::discRecordingRate() const
{
    if (m_subMode)
        return MAX_SUBMUXER_RATE;
    return is4K() ? MAX_4K_MUXER_RATE : MAX_MAIN_MUXER_RATE;
}

std::vector<int32_t> TSMuxer::getInterleaveInfo(const size_t idx) const { return m_interleaveInfo[idx]; }
