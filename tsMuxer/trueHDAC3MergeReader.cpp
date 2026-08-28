#include "trueHDAC3MergeReader.h"

#include <fs/systemlog.h>

#include <iomanip>
#include <sstream>

#include "abstractStreamReader.h"
#include "avCodecs.h"
#include "pesPacket.h"
#include "vodCoreException.h"
#include "vod_common.h"

TrueHDAC3MergeReader::TrueHDAC3MergeReader(const std::map<std::string, std::string>& addParams)
    : m_mergeAc3Pid(0),
      m_useNewStyleAudioPES(false),
      m_thdDemuxWaitAc3(true),
      m_demuxedTHDSamplesForAc3(0),
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
                pos = keepFrom;
            }
            break;
        }
        m_lostBytes += frame - start;  // the garbage before the sync is data that could not be used
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
        const int total = flen + skipBytes;
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
        }
        m_ac3FrameQueue.push_back(std::move(q));
        pos += total;
    }
    if (pos > 0)
        m_ac3Accum.erase(m_ac3Accum.begin(), m_ac3Accum.begin() + pos);
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
        if (m_thdDemuxWaitAc3 && m_ac3FrameQueue.empty())
        {
            if (m_curPos < m_bufEnd)
            {
                m_tmpBufferLen = static_cast<uint32_t>(m_bufEnd - m_curPos);
                memmove(m_tmpBuffer.data(), m_curPos, m_tmpBufferLen);
                m_curPos = m_bufEnd;
            }
            return AbstractStreamReader::NEED_MORE_DATA;
        }

        // Priority 4: Pre-fill delayed buffer for next AC3 emission when not waiting
        if (!m_thdDemuxWaitAc3 && m_delayedAc3Buffer.isEmpty() && !m_ac3FrameQueue.empty())
            fillDelayedFromQueue();

        // Read next TrueHD packet
        const int rez = SimplePacketizerReader::readPacket(avPacket);
        if (rez != 0)
            return rez;

        if (m_samplerate)
            avPacket.dts = avPacket.pts = m_totalTHDSamples * INTERNAL_PTS_FREQ / m_samplerate;

        m_totalTHDSamples += m_samples;
        m_demuxedTHDSamplesForAc3 += m_samples;
        // Trigger AC3 wait when we have enough TrueHD samples and AC3 frames available.
        // A frame held in the delayed buffer counts as available. Priority 4 above keeps exactly
        // one frame back OUT of the queue, so at the end of the stream the queue is empty
        // precisely BECAUSE its last element is the frame now sitting in that buffer. Testing the
        // queue alone therefore never re-arms, Priority 1 is never reached again, and that frame
        // dies with the reader: N frames in and N-1 out, on every length, for the same reason
        // every time.
        if (m_ac3SamplesPerSyncFrame > 0 && m_demuxedTHDSamplesForAc3 >= m_ac3SamplesPerSyncFrame &&
            (!m_ac3FrameQueue.empty() || !m_delayedAc3Buffer.isEmpty()))
        {
            m_demuxedTHDSamplesForAc3 -= m_ac3SamplesPerSyncFrame;
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
                avPacket.pts = avPacket.dts = m_totalTHDSamples * INTERNAL_PTS_FREQ / m_samplerate;
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
// fraction of a frame short; more than a whole frame is a real gap. A pressed disc has no shortfall
// at all: 938 core frames for 938 over a measured cut, the core being constant bitrate and therefore
// a clock. At one second the warning stayed silent on a core a full second short of its track.
void TrueHDAC3MergeReader::reportCoreCoverage()
{
    if (m_coverageReported)
        return;
    m_coverageReported = true;
    if (m_samplerate <= 0 || m_ac3FramesEmitted == 0)
        return;

    const double losslessSec = static_cast<double>(m_totalTHDSamples) / m_samplerate;
    const double coreSec = static_cast<double>(m_nextAc3Time) / INTERNAL_PTS_FREQ;
    const double coreFrameSec = coreSec / m_ac3FramesEmitted;
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
