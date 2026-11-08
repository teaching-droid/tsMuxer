// Largely rewritten by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#include "trueHDAC3MergeReader.h"

#include <fs/systemlog.h>

#include <cstring>
#include <iomanip>
#include <sstream>

#include "abstractStreamReader.h"
#include "avCodecs.h"
#include "pesPacket.h"
#include "tsPacket.h"
#include "vodCoreException.h"
#include "vod_common.h"

TrueHDAC3MergeReader::TrueHDAC3MergeReader(const std::map<std::string, std::string>& addParams)
    : m_mergeAc3Pid(0),
      m_useNewStyleAudioPES(false),
      m_thdDemuxWaitAc3(true),
      m_demuxedTHDTimeForAc3(0),
      m_nextAc3Time(0),
      m_ac3SamplesPerSyncFrame(0),
      m_pendingEmitSamples(0),
      m_pendingEmitSampleRate(0),
      m_ac3FramesEmitted(0),
      m_coverageReported(false),
      m_ac3CoreSampleRate(0),
      m_ac3CoreChannels(0)
{
    const auto itTrack = addParams.find("merge-ac3-track");
    const auto itFile = addParams.find("merge-ac3-file");
    if ((itTrack == addParams.end() || itTrack->second.empty()) &&
        (itFile == addParams.end() || itFile->second.empty()))
        THROW(ERR_INVALID_CODEC_FORMAT, "internal: TrueHDAC3MergeReader without merge-ac3-* source")
    if (itTrack != addParams.end() && !itTrack->second.empty())
        m_mergeAc3Pid = strToInt32(itTrack->second.c_str());
}

TrueHDAC3MergeReader::~TrueHDAC3MergeReader() { reportCoreCoverage(); }

// ** THE CORE KEEPS ITS OWN START. ** The AC-3 track of merge-ac3-track was timed from 0, from the first TrueHD unit,
// whatever its start in the Matroska file: a core that starts 500 ms after its TrueHD came 500 ms early against the
// TrueHD and the picture, where the same track muxed as a track of its own keeps its start. Its frames now keep the
// distance to the TrueHD that the file gives them, and a core that starts later goes out when the TrueHD has reached
// its start, so that each core frame lies by the TrueHD units of its time.
void TrueHDAC3MergeReader::setCoreStart(const int64_t start)
{
    m_coreStart = m_nextAc3Time = start;
    if (start > 0)
    {
        m_coreDelayLeft = start;
        m_thdDemuxWaitAc3 = false;
    }
}

const CodecInfo& TrueHDAC3MergeReader::getCodecInfo() { return trueHDCodecInfo; }

void TrueHDAC3MergeReader::setAc3SideData(const uint8_t* data, const uint32_t len)
{
    if (data == nullptr || len == 0)
        return;
    // ** THE AC-3 HALF ARRIVES HERE AND NOT THROUGH setBuffer, SO IT HAS TO BE COUNTED HERE. **
    //
    // merge-ac3-track braids a separate AC-3 stream into this TrueHD track, and those bytes reach
    // the reader down their own path. The loss report divides by getReadSize(), which is summed in
    // setBuffer, so the AC-3 half was missing from it entirely: the eighth review measured the
    // report claiming 4,251,706 bytes were read for a track that had just been written out at
    // 5,302,772. ** IT SAID FEWER BYTES WERE READ THAN WERE WRITTEN, WHICH CANNOT BE TRUE OF
    // ANYTHING. ** The ninth review found it still there.
    m_readBytes += len;
    m_ac3BytesConsumed += len;
    const size_t off = m_ac3Accum.size();
    m_ac3Accum.resize(off + len);
    memcpy(m_ac3Accum.data() + off, data, len);
    extractAc3FramesFromAccum();
}

// ** THE DENOMINATOR COUNTED THIS HALF AND THE NUMERATOR HAD NO PATH TO IT. **
//
// setAc3SideData adds the AC-3 half to getReadSize(), which is what the loss report divides by, but
// every byte this function abandons was abandoned in silence, so a real loss in that half could not
// be reported at all. Measured on one AC-3 file used two ways: read as an ordinary track it reports
// "202496 bytes of the 1120000 read", and braided into a TrueHD track by merge-ac3 the same damage
// is ** SILENT **.
//
// There are three places here where a byte is given up, and each one is now charged: the garbage
// skipped to reach a sync, the single byte dropped when a frame will not parse, and the run
// discarded when a whole block holds no sync at all. Nothing else changes; the frames themselves
// are counted by the denominator already.
void TrueHDAC3MergeReader::extractAc3FramesFromAccum()
{
    // Parse with a read cursor and compact the accumulator ONCE at the end. The
    // previous version erased the vector front per AC-3 frame, which memmoves the
    // whole remaining accumulator (a 2 MB read block) for every ~2 KB frame:
    // O(chunk^2 / frameLen), hundreds of GB of memmove over a movie-length track.
    size_t pos = 0;
    while (pos < m_ac3Accum.size())
    {
        uint8_t* start = m_ac3Accum.data() + pos;
        uint8_t* end = m_ac3Accum.data() + m_ac3Accum.size();
        uint8_t* frame = m_ac3Parser.findAc3Sync(start, end);
        if (frame == nullptr)
        {
            // no sync in the rest: keep at most the last 4096 bytes of it
            if (m_ac3Accum.size() - pos > 65536)
            {
                const size_t keepFrom = m_ac3Accum.size() - 4096;
                m_lostBytes += static_cast<int64_t>(keepFrom - pos);
                countLostPadding(start, m_ac3Accum.data() + keepFrom);
                pos = keepFrom;
            }
            break;
        }
        m_lostBytes += frame - start;  // the garbage before the sync is data that could not be used
        countLostPadding(start, frame);
        pos += frame - start;
        int skipBytes = 0;
        const int flen = m_ac3Parser.parse(frame, end, skipBytes);
        if (flen == NOT_ENOUGH_BUFFER)
            break;  // partial frame stays at the front for the next call
        if (flen <= 0)
        {
            m_lostBytes++;  // bad frame: this byte is abandoned, so it is a loss
            pos++;          // resync from the next byte
            continue;
        }
        if (m_ac3Parser.isEAC3())
        {
            THROW(ERR_INVALID_CODEC_FORMAT,
                  "merge-ac3-track: E-AC-3 is not supported as the TrueHD core; use a classic AC-3 track or "
                  "transcode with ffmpeg -c:a ac3 (see tsMuxer --help).")
        }
        if (m_ac3CoreSampleRate == 0)
            checkCoreRate(m_ac3Parser.frameSampleRate());
        const int total = flen + skipBytes;
        m_frameBytes += total;  // the core arrives here, and its bytes are counted as read here (setAc3SideData)
        Ac3QueuedFrame q;
        q.data.assign(frame, frame + total);
        q.samples = m_ac3Parser.frameSamples();
        q.sample_rate = m_ac3Parser.frameSampleRate();
        if (m_ac3SamplesPerSyncFrame == 0 && q.samples > 0)
            m_ac3SamplesPerSyncFrame = q.samples;
        // The first frame that parsed is what the core IS. Nothing later overwrites it, so a
        // damaged frame in the middle of the source cannot rewrite the track header behind it.
        if (m_ac3CoreSampleRate == 0 && q.sample_rate > 0)
        {
            m_ac3CoreSampleRate = q.sample_rate;
            m_ac3CoreChannels = m_ac3Parser.frameChannels();
            if (total >= static_cast<int>(sizeof(m_coreHeader)))
                memcpy(m_coreHeader, frame, sizeof(m_coreHeader));
        }
        m_ac3FrameQueue.push_back(std::move(q));
        pos += total;
    }
    if (pos > 0)
        m_ac3Accum.erase(m_ac3Accum.begin(), m_ac3Accum.begin() + pos);
}

// ** THE CORE IS THE SAME AUDIO AT 48 KHZ. ** A player that does not decode TrueHD plays the AC-3 core in its place,
// and on a Blu-ray that core is AC-3 at 48 kHz under TrueHD at 48, 96 or 192 kHz. An AC-3 at 44.1 or 32 kHz was taken
// as the core without a word: the disc held 44.1 kHz frames in its 48 kHz TrueHD track, and its clip information a rate
// code a disc does not have. Such an AC-3 is refused, as E-AC-3 is. TrueHD at a rate that is no disc rate, as 44.1 kHz,
// is warned about as before and keeps the core it is given. Checked at the first core frame, or at the first TrueHD
// unit when the rate of the TrueHD is not known before.
void TrueHDAC3MergeReader::checkCoreRate(const int coreRate)
{
    if (m_coreRateChecked || coreRate <= 0 || m_samplerate <= 0)
        return;
    m_coreRateChecked = true;
    if (m_samplerate % 48000 == 0 && coreRate != 48000)
        THROW(ERR_INVALID_CODEC_FORMAT, (m_mergeAc3Pid ? "merge-ac3-track" : "merge-ac3-file")
                                            << ": the AC-3 has a sample rate of " << coreRate
                                            << " Hz, so it cannot be the core of TrueHD at " << m_samplerate
                                            << " Hz: the core is the same audio at 48 kHz, which a player that does "
                                               "not decode TrueHD plays. Use an AC-3 at 48 kHz (transcode with "
                                               "ffmpeg -ar 48000 -c:a ac3).")
}

void TrueHDAC3MergeReader::fillDelayedFromQueue()
{
    if (m_ac3FrameQueue.empty())
        return;
    const Ac3QueuedFrame front = std::move(m_ac3FrameQueue.front());
    m_ac3FrameQueue.pop_front();
    m_pendingEmitSamples = front.samples;
    m_pendingEmitSampleRate = front.sample_rate;
    m_delayedAc3Buffer.clear();
    m_delayedAc3Buffer.append(front.data.data(), front.data.size());
    m_delayedAc3Packet.flags = m_flags + AVPacket::IS_COMPLETE_FRAME | AVPacket::FORCE_NEW_FRAME;
    m_delayedAc3Packet.stream_index = m_streamIndex;
    m_delayedAc3Packet.codecID = getCodecInfo().codecID;
    m_delayedAc3Packet.codec = static_cast<BaseAbstractStreamReader*>(this);
    m_delayedAc3Packet.duration = 0;
    m_delayedAc3Packet.data = m_delayedAc3Buffer.data();
    m_delayedAc3Packet.size = static_cast<int>(front.data.size());
}

int TrueHDAC3MergeReader::readPacket(AVPacket& avPacket)
{
    while (true)
    {
        // Priority 1: Return pending AC3 packet if waiting for it
        if (m_thdDemuxWaitAc3 && !m_delayedAc3Buffer.isEmpty())
        {
            avPacket = m_delayedAc3Packet;
            m_delayedAc3Buffer.clear();
            m_thdDemuxWaitAc3 = false;
            avPacket.dts = avPacket.pts = m_nextAc3Time;
            avPacket.flags |= AVPacket::IS_CORE_PACKET;
            m_ac3FramesEmitted++;
            if (m_pendingEmitSampleRate > 0 && m_pendingEmitSamples > 0)
                m_nextAc3Time +=
                    static_cast<int64_t>(INTERNAL_PTS_FREQ) * m_pendingEmitSamples / m_pendingEmitSampleRate;
            return 0;
        }

        // Priority 2: Return AC3 frame if we're in AC3 wait state and have queued frames
        if (m_thdDemuxWaitAc3 && m_delayedAc3Buffer.isEmpty() && !m_ac3FrameQueue.empty())
        {
            Ac3QueuedFrame q = std::move(m_ac3FrameQueue.front());
            m_ac3FrameQueue.pop_front();
            m_immediateAc3Buffer.clear();
            m_immediateAc3Buffer.append(q.data.data(), q.data.size());
            avPacket.flags = m_flags + AVPacket::IS_COMPLETE_FRAME | AVPacket::FORCE_NEW_FRAME;
            avPacket.stream_index = m_streamIndex;
            avPacket.codecID = getCodecInfo().codecID;
            avPacket.codec = static_cast<BaseAbstractStreamReader*>(this);
            avPacket.data = m_immediateAc3Buffer.data();
            avPacket.size = static_cast<int>(q.data.size());
            avPacket.duration = 0;
            avPacket.dts = avPacket.pts = m_nextAc3Time;
            avPacket.flags |= AVPacket::IS_CORE_PACKET;
            m_ac3FramesEmitted++;
            if (q.sample_rate > 0 && q.samples > 0)
                m_nextAc3Time += static_cast<int64_t>(INTERNAL_PTS_FREQ) * q.samples / q.sample_rate;
            m_thdDemuxWaitAc3 = false;
            if (m_ac3SamplesPerSyncFrame == 0)
                m_ac3SamplesPerSyncFrame = q.samples;
            return 0;
        }

        // Priority 3: Need more AC3 data if waiting and don't have any.
        // Carry the unconsumed TrueHD tail over, exactly as SimplePacketizerReader::readPacket
        // does before every NEED_MORE_DATA. Without this, setBuffer refills the staging buffer
        // from offset 0 and whatever was still sitting between m_curPos and m_bufEnd is gone.
        // It only bites when the AC-3 source has no sync word in its first block (a wrong or
        // empty merge-ac3-file), but then it silently drops a whole 2 MiB of audio and still
        // reports "Mux successful complete".
        // A wait armed before the core's end was known can find that end here: then no core frame is
        // coming, and the lossless audio goes on alone (the coverage report at the end says so).
        if (m_thdDemuxWaitAc3 && m_ac3FrameQueue.empty() && m_ac3SideEof)
            m_thdDemuxWaitAc3 = false;
        if (m_thdDemuxWaitAc3 && m_ac3FrameQueue.empty())
        {
            if (m_curPos < m_bufEnd)
            {
                m_tmpBufferLen = static_cast<uint32_t>(m_bufEnd - m_curPos);
                memmove(m_tmpBuffer.data(), m_curPos, m_tmpBufferLen);
                m_curPos = m_bufEnd;
            }
            // Say where the stream stands, as SimplePacketizerReader does before asking for more data. The muxer
            // takes the time of the packet it gets back as this stream's position and adds the stream's timeshift
            // to it, also when no packet came. Left untouched, the packet still held the time of the one before,
            // to which the timeshift had already been added, or the time of a video packet, so a timeshift= of
            // 7 s made the position jump 7 s ahead and the audio was sent up to 7 s after it was due: dropouts on
            // playback. A merge from a Matroska file waits here often, because the AC-3 arrives in small pieces.
            avPacket.flags = m_flags + AVPacket::IS_COMPLETE_FRAME | AVPacket::FORCE_NEW_FRAME;
            avPacket.stream_index = m_streamIndex;
            avPacket.codecID = getCodecInfo().codecID;
            avPacket.codec = static_cast<BaseAbstractStreamReader*>(this);
            avPacket.data = nullptr;
            avPacket.size = 0;
            avPacket.duration = 0;
            avPacket.dts = avPacket.pts = m_samplerate ? samplesToInternal(m_totalTHDSamples, m_samplerate)
                                                       : static_cast<int64_t>(m_curPts * m_stretch) + m_timeOffset;
            return AbstractStreamReader::NEED_MORE_DATA;
        }

        // Priority 4: Pre-fill delayed buffer for next AC3 emission when not waiting
        if (!m_thdDemuxWaitAc3 && m_delayedAc3Buffer.isEmpty() && !m_ac3FrameQueue.empty())
            fillDelayedFromQueue();

        // Read next TrueHD packet
        const int rez = SimplePacketizerReader::readPacket(avPacket);
        if (rez != 0)
            return rez;
        checkCoreRate(m_ac3CoreSampleRate);

        if (m_samplerate)
            avPacket.dts = avPacket.pts = samplesToInternal(m_totalTHDSamples, m_samplerate);

        m_totalTHDSamples += m_samples;
        if (m_coreDelayLeft > 0)
        {
            if (m_samplerate > 0)
                m_coreDelayLeft -= samplesToInternal(m_samples, m_samplerate);
            if (m_coreDelayLeft <= 0)
                m_thdDemuxWaitAc3 = true;
            return 0;
        }
        // A core frame is due when the lossless audio sent since the last one has PLAYED for as long
        // as one core frame plays. Counting samples compared two clocks: a 96 or 192 kHz TrueHD unit
        // holds 80 or 160 samples, so the 1,536 of a 48 kHz core frame were reached two or four times
        // too soon, every core frame went out in the first half or quarter of the title and none
        // after. Each side is scaled by the other's rate, so with equal rates the decisions are
        // exactly the ones the sample count made.
        const int64_t thdRate = m_samplerate > 0 ? m_samplerate : 1;
        const int64_t coreRate = m_ac3CoreSampleRate > 0 ? m_ac3CoreSampleRate : thdRate;
        m_demuxedTHDTimeForAc3 += static_cast<int64_t>(m_samples) * coreRate;
        const int64_t coreFrameTime = static_cast<int64_t>(m_ac3SamplesPerSyncFrame) * thdRate;
        // Trigger AC3 wait when we have enough TrueHD samples and AC3 frames available.
        // A frame held in the delayed buffer counts as available. Priority 4 above keeps exactly
        // one frame back OUT of the queue, so at the end of the stream the queue is empty
        // precisely BECAUSE its last element is the frame now sitting in that buffer. Testing the
        // queue alone therefore never re-arms, Priority 1 is never reached again, and that frame
        // dies with the reader: N frames in and N-1 out, on every length, for the same reason
        // every time.
        // ** A DUE CORE FRAME WAITS; IT IS NOT POSTPONED. ** This also required one to be already
        // BUFFERED, so a momentarily empty queue put the frame out one unit LATE and, the surplus
        // having been banked, the next one one unit EARLY. Measured on a two hour track taken from
        // a Matroska: ten such postponements, eighteen core frames moved by one 0.833 ms unit, and
        // a combined stream that no longer matched its source byte for byte although both
        // substreams did. A gap histogram does NOT catch all ten, because a frame due exactly on a
        // unit boundary turns a gap of 38 into 39, which is an ordinary value.
        //
        // Arming the wait with nothing buffered sends readPacket to Priority 3, which asks for more
        // AC-3 and gets it, because the demuxer feeds both sides in lockstep. The only reason not to
        // wait is that none is coming, and that is the AC-3 side's own end of data: with
        // merge-ac3-file the core can end while most of the lossless track remains, so asking the
        // LOSSLESS stream instead threw part way through a short core run rather than warning.
        if (m_ac3SamplesPerSyncFrame > 0 && m_demuxedTHDTimeForAc3 >= coreFrameTime &&
            (!m_ac3FrameQueue.empty() || !m_delayedAc3Buffer.isEmpty() || !m_ac3SideEof))
        {
            m_demuxedTHDTimeForAc3 -= coreFrameTime;
            m_thdDemuxWaitAc3 = true;
        }
        return 0;
    }
}

int TrueHDAC3MergeReader::flushPacket(AVPacket& avPacket)
{
    const int rez = MLPStreamReader::flushPacket(avPacket);
    if (rez > 0)
    {
        if (!(avPacket.flags & AVPacket::PRIORITY_DATA))
            if (m_samplerate)
                avPacket.pts = avPacket.dts = samplesToInternal(m_totalTHDSamples, m_samplerate);
    }
    return rez;
}

// Say so if the AC-3 core did not last as long as the lossless track it was merged into.
//
// This used to pass in silence. A merge given a source covering only the first of two joined parts
// produced a complete lossless track, a core that stopped half way, "Mux successful complete", exit
// zero, and no warning anywhere. Half the title had TrueHD with no compatibility core under it,
// which a Blu-ray does not allow, and nothing said a word.
//
// MEASURED AT THE END RATHER THAN GUESSED FROM THE META, on purpose. The setup that usually causes
// it, a joined TrueHD line with a single merge-ac3-file, is ALSO the correct way to do it when the
// AC-3 files were joined in an earlier pass. Warning on the setup would fire on correct usage and
// become noise; warning on the outcome only fires when something is actually missing.
//
// THE TOLERANCE IS ONE AC-3 FRAME, and it used to be one second. The wider figure was never about
// what a disc needs: it was chosen to sit outside a separate defect that dropped the final core
// frame on every merge, 32 ms, so that the warning could not fire on it. That defect is fixed, so
// the allowance goes back to what the format itself asks for. The core is emitted in whole frames,
// so a lossless track whose length is not an exact multiple of one frame can legitimately come up a
// fraction of a frame short; more than a whole frame is a real gap. A disc track with its core has no shortfall
// at all: 938 core frames for 938 over a measured cut, the core being constant bitrate and therefore
// a clock. At one second the warning stayed silent on a core a full second short of its track.
void TrueHDAC3MergeReader::reportCoreCoverage()
{
    if (m_coverageReported)
        return;
    m_coverageReported = true;
    // m_readBytes is what THIS reader was actually fed. A discovery pass runs before the mux and
    // builds a reader of its own that is never given anything, so without this every merge, including
    // the ones that work, reported a missing core from that reader's destructor. Measured: the
    // discovery instance ends with readBytes 0, and the muxing instance with 5,343,300 on a merge
    // that worked and 4,230,468 on one whose AC-3 source gave nothing.
    if (m_samplerate <= 0 || m_readBytes == 0)
        return;

    // ** NO CORE AT ALL WAS THE ONE OUTCOME THIS SAID NOTHING ABOUT. **
    //
    // The zero was excluded so the average below would not divide by it, which left the WORST
    // result, a merge that produced no compatibility core whatever, as the only silent one. The loss
    // report cannot catch it either: a track that is not in the file contributes no bytes read, so
    // there is nothing to charge. Measured on a Matroska source, the same line differing only in the
    // track number: one the file holds gives 7,212,432 bytes and names the track the core came from,
    // one it does not hold gives 4,323,248 bytes, no core, no warning, exit 0.
    if (m_ac3FramesEmitted == 0)
    {
        LTRACE(LT_WARN, 2,
               "Warning: no AC-3 compatibility core was written for this TrueHD track, so the output has "
               "none at all, which a Blu-ray does not allow. Nothing usable came back from the AC-3 "
               "source. With merge-ac3-track, check that the number names a track the file really holds, "
               "because a remuxed file does not keep the track numbers of the file it was made from. "
               "With merge-ac3-file, check that the file is a classic AC-3 stream. Listing the source on "
               "its own shows the tracks and codecs tsMuxeR finds in it.");
        return;
    }

    const double losslessSec = static_cast<double>(m_totalTHDSamples) / m_samplerate;
    const double coreSec = static_cast<double>(m_nextAc3Time) / INTERNAL_PTS_FREQ;
    const double coreFrameSec =
        static_cast<double>(m_nextAc3Time - m_coreStart) / INTERNAL_PTS_FREQ / m_ac3FramesEmitted;
    if (losslessSec - coreSec <= coreFrameSec)
        return;

    // Two decimals rather than whole seconds, because a gap can now be reported that is shorter
    // than a second, and "the last 0 s of it has no compatibility core" says nothing.
    std::ostringstream msg;
    msg << std::fixed << std::setprecision(2) << "Warning: the AC-3 core covers only " << coreSec << " s of a "
        << losslessSec << " s TrueHD track, so the last " << (losslessSec - coreSec)
        << " s of it has no compatibility core, which a Blu-ray does not allow. The AC-3 source ran out "
           "first: it must cover the whole TrueHD track, including every part of a joined one. "
           "merge-ac3-track follows a join by itself; merge-ac3-file does not, so join the AC-3 files in "
           "a separate pass first and merge the single result.";
    LTRACE(LT_WARN, 2, msg.str());
}

bool TrueHDAC3MergeReader::needMPLSCorrection() const { return false; }

void TrueHDAC3MergeReader::writePESExtension(PESPacket* pesPacket, const AVPacket& avPacket)
{
    if (m_useNewStyleAudioPES)
    {
        pesPacket->flagsLo |= 1;
        uint8_t* data = reinterpret_cast<uint8_t*>(pesPacket) + pesPacket->getHeaderLength();
        *data++ = 0x01;
        *data++ = 0x81;
        if (avPacket.flags & AVPacket::IS_CORE_PACKET)
            *data = 0x76;
        else
            *data = 0x72;
        pesPacket->m_pesHeaderLen += 3;
    }
}

const std::string TrueHDAC3MergeReader::getStreamInfo()
{
    std::ostringstream str;
    str << "TRUE-HD + AC-3 core (merged from track " << m_mergeAc3Pid << "). ";
    str << MLPStreamReader::getStreamInfo();
    return str.str();
}

// ** THE DISC FORM OF TRUEHD WITH ITS CORE IS DECLARED BY ITS CORE. ** A Blu-ray declares a TrueHD stream that
// carries an AC-3 core with the AC-3 registration and the AC-3 audio descriptor of that core (ATSC A/52 annex A),
// and so does tsMuxeR for the same stream read from a disc as A_AC3. Built with merge-ac3-file or merge-ac3-track,
// the same kind of stream had the HDMV registration of a TrueHD stream without a core: two declarations for one kind
// of stream. It now has the disc's, from the first core frame; where no core frame was read yet, the HDMV one stays.
int TrueHDAC3MergeReader::getTSDescriptor(uint8_t* dstBuff, const bool blurayMode, const bool hdmvDescriptors)
{
    const int len = MLPStreamReader::getTSDescriptor(dstBuff, blurayMode, hdmvDescriptors);
    if (!hdmvDescriptors || len <= 0 || m_coreHeader[0] != 0x0B || m_coreHeader[1] != 0x77)
        return len;
    const uint8_t fscod = m_coreHeader[4] >> 6;
    const uint8_t frmsizecod = m_coreHeader[4] & 0x3F;
    const uint8_t bsid = m_coreHeader[5] >> 3;
    const uint8_t bsmod = m_coreHeader[5] & 0x07;
    const uint8_t acmod = m_coreHeader[6] >> 5;
    const uint8_t dsurmod = acmod == 2 ? (m_coreHeader[6] >> 3) & 0x03 : 0;  // 2/0: dsurmod follows acmod at once
    uint8_t* p = dstBuff;
    *p++ = static_cast<uint8_t>(TSDescriptorTag::REGISTRATION);
    *p++ = 4;
    *p++ = 'A';
    *p++ = 'C';
    *p++ = '-';
    *p++ = '3';
    *p++ = static_cast<uint8_t>(TSDescriptorTag::AC3);
    *p++ = 4;
    *p++ = static_cast<uint8_t>(fscod << 5 | bsid);                 // sample_rate_code (3), bsid (5)
    *p++ = static_cast<uint8_t>((frmsizecod >> 1) << 2 | dsurmod);  // bit_rate_code (6, exact), surround_mode (2)
    *p++ = static_cast<uint8_t>(bsmod << 5 | acmod << 1);           // bsmod (3), num_channels (4), full_svc (1)
    *p++ = 0;                                                       // langcod
    return static_cast<int>(p - dstBuff);
}
