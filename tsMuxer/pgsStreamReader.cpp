// Changed by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#include "pgsStreamReader.h"

#include <fs/systemlog.h>
#include <cmath>
#include <sstream>
#include <string>

#include "avCodecs.h"
#include "bitmapSubtitles.h"
#include "ioContextDemuxer.h"
#include "tsMuxer.h"
#include "vodCoreException.h"
#include "vod_common.h"

using namespace text_subtitles;
using namespace std;

#define fabs(a) ((a) >= 0 ? (a) : -(a))

double pgs_frame_rates[16] = {
    0, 23.97602397602397, 24, 25, 29.97002997002997, 30, 50, 59.94005994005994, 60, 0, 0, 0, 0, 0, 0, 0};

PGSStreamReader::PGSStreamReader()
    : m_avFragmentEnd(nullptr),
      m_firstRenderedPacket(true),
      m_palleteID(0),
      m_paletteVersion(0),
      m_objectWindowHeight(0),
      m_objectWindowTop(0),
      m_renderedFromText(false)
{
    m_curPos = m_buffer = nullptr;
    m_tmpBufferLen = 0;
    m_state = State::stParsePES;
    composition_state = CompositionState::csEpochStart;
    m_processedSize = 0;
    m_demuxMode = false;
    m_afterPesByte = 0;
    // m_supWritedPts = m_supWritedDts = -1;
    m_maxPTS = m_lastDTS = m_lastPTS = -1;
    m_video_width = 0;
    m_video_height = 0;
    m_frame_rate = 0;
    m_newFps = 0;
    m_scale = 1.0;
    m_isNewFrame = false;
    m_needRescale = false;
    m_imgBuffer = nullptr;
    m_rgbBuffer = nullptr;
    m_scaledRgbBuffer = nullptr;
    m_scaled_width = 0;
    m_scaled_height = 0;
    m_render = new TextToPGSConverter(false);
    m_renderedData = nullptr;
    m_fontBorder = 0;
    m_offsetId = 0xff;
    m_forced_on_flag = false;

    // SS PG data
    isSSPG = false;
    leftEyeSubStreamIdx = -1;
    rightEyeSubStreamIdx = -1;
    ssPGOffset = 0xff;
}

void PGSStreamReader::video_descriptor(BitStreamReader& bitReader)
{
    // ** A PGS STREAM HAS ONE PICTURE SIZE. ** The pictures of a track that is drawn again at another size are drawn
    // into buffers of the size its first composition gives. A later composition that gives another size, as a damaged
    // one does, was taken at its word: a larger size was drawn past the end of those buffers. The size of the first
    // composition stays once the buffers are made, and the other one is said once.
    const auto width = bitReader.getBits<uint16_t>(16);
    const auto height = bitReader.getBits<uint16_t>(16);
    if (m_imgBuffer == nullptr)
    {
        m_video_width = width;
        m_video_height = height;
    }
    else if ((width != m_video_width || height != m_video_height) && !m_sizeChangeSaid)
    {
        m_sizeChangeSaid = true;
        LTRACE(LT_WARN, 2,
               "Warning: PGS stream (track " << m_streamIndex << "): a composition gives a picture of " << width << ':'
                                             << height << " where the stream began with " << m_video_width << ':'
                                             << m_video_height << ". The size it began with is kept.");
    }
    const auto frame_rate_index = bitReader.getBits<uint8_t>(4);
    m_frame_rate = pgs_frame_rates[frame_rate_index];
    bitReader.skipBits(4);
    m_badDescriptor = frame_rate_index > 8 || m_video_width == 0 || m_video_width > 4096 || m_video_height > 4096;
}

void PGSStreamReader::composition_descriptor(BitStreamReader& bitReader)
{
    bitReader.skipBits(16);  // composition_number
    composition_state = static_cast<CompositionState>(bitReader.getBits(2));
    bitReader.skipBits(6);
}

void PGSStreamReader::composition_object(BitStreamReader& bitReader)
{
    PgsPlacedObject placed;
    placed.id = bitReader.getBits<uint16_t>(16);
    bitReader.skipBits(8);  // window_id_ref
    placed.cropped = bitReader.getBit();
    m_forced_on_flag = bitReader.getBit();
    placed.forced = m_forced_on_flag;
    bitReader.skipBits(6);
    placed.x = bitReader.getBits<uint16_t>(16);
    placed.y = bitReader.getBits<uint16_t>(16);
    if (placed.cropped)
    {
        placed.cropX = bitReader.getBits<uint16_t>(16);
        placed.cropY = bitReader.getBits<uint16_t>(16);
        placed.cropWidth = bitReader.getBits<uint16_t>(16);
        placed.cropHeight = bitReader.getBits<uint16_t>(16);
    }
    m_dsObjects.push_back(placed);
}

void PGSStreamReader::pgs_window(BitStreamReader& bitReader)
{
    bitReader.skipBits(8);   // window_id
    bitReader.skipBits(32);  // window_horizontal_position, window_vertical_position
    bitReader.skipBits(32);  // window_width, window_height
}

int PGSStreamReader::calcFpsIndex(const double fps)
{
    for (int i = 0; i < 16; i++)
        if (fabs(pgs_frame_rates[i] - fps) < 1e-4)
            return i;
    THROW(ERR_SOURCE, "Non standard fps value are not supported for PGS streams")
}

void PGSStreamReader::readPalette(const uint8_t* pos, const uint8_t* end)
{
    m_palette.clear();
    // its id and version, then entries of 5 bytes: a segment cut short was read past its end
    if (end - pos < 2)
        return;
    m_palleteID = *pos++;
    m_paletteVersion = *pos++;
    while (end - pos >= 5)
    {
        YUVQuad color;
        int index = *pos++;
        color.Y = *pos++;
        color.Cr = *pos++;
        color.Cb = *pos++;
        color.alpha = *pos++;
        if (color.alpha != 0)
            m_palette.insert(make_pair(index, color));
    }
}

void PGSStreamReader::yuvToRgb(const int minY) const
{
    const uint8_t* src = m_imgBuffer;
    const int size = m_video_width * m_video_height;
    const uint8_t* end = src + size;
    auto dst = reinterpret_cast<RGBQUAD*>(m_rgbBuffer);

    RGBQUAD rgbPal[256]{};
    YUVQuad yuvPal[256];
    memset(&rgbPal[0], 0, sizeof(rgbPal));
    memset(&rgbPal[0], 0, sizeof(yuvPal));

    for (auto itr : m_palette)
    {
        rgbPal[itr.first] = TextToPGSConverter::YUVAToRGBA(itr.second, m_video_height > 576);  // as it was written
        yuvPal[itr.first] = itr.second;
    }
    constexpr RGBQUAD zeroRgb = {};
    for (; src < end; ++src)
    {
        if (yuvPal[*src].Y >= minY)
            *dst++ = rgbPal[*src];
        else
            *dst++ = zeroRgb;
    }
}

// ** EVERY PIXEL GOES INTO THE PICTURE ONLY WHERE IT LIES INSIDE IT. **
// The pixels were written one after the other from the object's place, and the end of each line moved on by the rest of
// the picture's width, so an object placed to reach past the right or the bottom edge of the picture, or one whose
// lines hold more pixels than its stated width, was written past the end of the picture buffer. Each pixel now has its
// own column and line, and what lies outside the picture is left out. The lengths in the data are checked against its
// end the same way. An object inside the picture is drawn as before.
void PGSStreamReader::decodeRleData(const std::vector<uint8_t>& rle, uint8_t* dst, const int dstWidth,
                                    const int dstHeight, const int xOffset, const int yOffset) const
{
    if (rle.empty())
        return;
    const uint8_t* src = rle.data();
    const uint8_t* srcEnd = src + rle.size();

    int x = 0;
    int y = 0;
    // ** A RUN IS WRITTEN AS FAR AS IT LIES IN THE PICTURE, AND THE COLUMN STOPS COUNTING. ** Each pixel of a run was
    // stepped on its own, inside the picture or not, and the column was an int that only an end of line set back: data
    // without an end of line, as damaged or made-up data has, counted it past the range of an int, where it turned
    // negative and the pixel was written far before the buffer; and one segment of such runs cost hundreds of
    // millions of steps. A line is not longer than the widest picture there is.
    static constexpr int WIDEST = 1 << 16;
    const auto put = [&](const uint8_t color, const int count)
    {
        const int px = xOffset + x;
        const int py = yOffset + y;
        if (px < dstWidth && py < dstHeight)
            memset(dst + static_cast<size_t>(py) * dstWidth + px, color,
                   static_cast<size_t>((std::min)(count, dstWidth - px)));
        x = (std::min)(x + count, WIDEST);
    };
    while (src < srcEnd)
    {
        if (yOffset + y >= dstHeight)
            break;  // below the picture: nothing more of the object is drawn
        if (*src != 0)
        {
            put(*src++, 1);
            continue;
        }
        if (++src >= srcEnd)
            break;
        if (*src == 0)
        {  // end of line
            src++;
            x = 0;
            y++;
            continue;
        }
        const bool hasColor = *src & 0x80;
        int runLength;
        if (*src & 0x40)
        {
            if (srcEnd - src < 2)
                break;
            runLength = ((*src & 0x3f) << 8) + src[1];
            src += 2;
        }
        else
            runLength = *src++ & 0x3f;
        uint8_t color = 0;
        if (hasColor)
        {
            if (src >= srcEnd)
                break;
            color = *src++;
        }
        put(color, runLength);
    }
}

static constexpr int Y_THRESHOLD = 33;

// ** AN OBJECT IS KEPT UNTIL THE NEXT EPOCH, AND MAY COME IN SEVERAL SEGMENTS. ** Each object was drawn as its segment
// was read, at the place of its id in the PCS, and then forgotten: a display set that shows the objects of its epoch
// again without their data (a new palette, or the same picture at an acquisition point) found nothing to draw and
// cleared the picture. An object of more than one segment (over 64 KB) was looked for in the bytes after its first
// segment, where the next segment's own header stood, and was lost. The segments of an object are now joined by their
// sequence flags (first 0x80, last 0x40), and the object is kept by its id until an epoch start; the display set is
// drawn from the objects its PCS shows (composeDisplaySet).
void PGSStreamReader::readObjectDef(const uint8_t* pos, const uint8_t* end)
{
    if (end - pos < 4)
        return;
    const uint16_t objectId = AV_RB16(pos);
    const uint8_t sequence = pos[3];
    pos += 4;  // object ID, version number and sequence flags
    PgsObject& object = m_objects[objectId];
    // a segment not marked as the first one begins an object as well where none of that id is open: a single segment
    // written without its flags is drawn as before
    if ((sequence & 0x80) || !object.open)
    {
        if (end - pos < 7)
            return;
        object = PgsObject();
        object.width = AV_RB16(pos + 3);
        object.height = AV_RB16(pos + 5);
        // not the 16 MB an object can claim, for each of 65536 ids: the data makes its own room as it comes
        object.rle.reserve((std::min)(AV_RB24(pos), 1u << 20));
        pos += 7;  // object data length, width and height
    }
    object.rle.insert(object.rle.end(), pos, end);
    object.open = !(sequence & 0x40);
}

// The picture of the display set, of the objects its PCS shows: each at its place, and of a cropped object only the
// part the PCS gives (the cropping was not read, and the whole object was drawn). False when it shows none.
bool PGSStreamReader::composeDisplaySet(const int which)
{
    bool drawn = false;
    std::vector<uint8_t> whole;
    for (const PgsPlacedObject& placed : m_dsObjects)
    {
        if ((which == 1 && !placed.forced) || (which == 2 && placed.forced))
            continue;
        const auto found = m_objects.find(placed.id);
        if (found == m_objects.end() || found->second.rle.empty())
            continue;
        const PgsObject& object = found->second;
        drawn = true;
        if (!placed.cropped)
        {
            decodeRleData(object.rle, m_imgBuffer, m_video_width, m_video_height, placed.x, placed.y);
            continue;
        }
        // The object is drawn whole first, at the size its segment gives. A damaged one can give 65535x65535, 4 GB
        // for each place it is shown at: no more of it is drawn than the crop and the picture can show.
        const int objectWidth = (std::min)({static_cast<int>(object.width), placed.cropX + placed.cropWidth,
                                            placed.cropX + static_cast<int>(m_video_width)});
        const int objectHeight = (std::min)({static_cast<int>(object.height), placed.cropY + placed.cropHeight,
                                             placed.cropY + static_cast<int>(m_video_height)});
        if (objectWidth <= 0 || objectHeight <= 0)
            continue;
        whole.assign(static_cast<size_t>(objectWidth) * objectHeight, 0xff);
        decodeRleData(object.rle, whole.data(), objectWidth, objectHeight, 0, 0);
        const int right = (std::min)(placed.cropX + placed.cropWidth, objectWidth);
        const int bottom = (std::min)(placed.cropY + placed.cropHeight, objectHeight);
        for (int y = placed.cropY; y < bottom; ++y)
        {
            const int py = placed.y + y - placed.cropY;
            if (py >= m_video_height)
                break;
            for (int x = placed.cropX; x < right; ++x)
            {
                const int px = placed.x + x - placed.cropX;
                if (px >= m_video_width)
                    break;
                m_imgBuffer[static_cast<size_t>(py) * m_video_width + px] =
                    whole[static_cast<size_t>(y) * objectWidth + x];
            }
        }
    }
    return drawn;
}

// ** A DISPLAY SET OF A PGS STREAM DRAWN AT ANOTHER SIZE IS DRAWN ONCE, AT ITS END. ** Each object was scaled and
// written as an epoch start of its own when it was read, so of a set with two objects only the second stayed on the
// screen, and the first was lost. Every object of the set is now drawn into one picture at its own place, and that
// picture is written as one object in one window, the box around all that is drawn. A set that shows nothing is a
// clear, written only while a picture is on the screen: a stream that began with a clear got a clear of a window of 0x0
// before any picture. An epoch start without an object clears the screen as well; the picture stayed on it.
void PGSStreamReader::renderDisplaySet()
{
    m_dsOpen = false;
    const size_t size = static_cast<size_t>(m_video_width) * m_video_height;
    // ** forced-pictures ON A LINE THAT IS DRAWN AGAIN. ** The filter of the forced pictures works on the segments of
    // the source, which a stream drawn at another size does not hand on: the line gave the whole track. With it, only
    // the forced objects of a set are drawn, and a set without one clears what the set before showed.
    const int which = m_forcedOnly ? 1 : 0;
    const auto draw = [&](const int part)
    {
        memset(m_imgBuffer, 0xff, size);
        memset(m_rgbBuffer, 0x00, size * 4);
        memset(m_scaledRgbBuffer, 0x00, static_cast<size_t>(m_scaled_width) * m_scaled_height * 4);
        return composeDisplaySet(part);
    };
    m_dsDrawn = draw(which);
    if (m_dsDrawn)
    {
        // the same picture in the same colours, shown again without object data: it stays on the screen as it is.
        // So does the forced part of a set whose other object changed, where the forced pictures alone are drawn.
        if ((!m_dsHasObjectData || which == 1) && m_pictureShown && m_palette == m_shownPalette &&
            m_shownPicture.size() == size && memcmp(m_shownPicture.data(), m_imgBuffer, size) == 0)
            return;
        m_shownPicture.assign(m_imgBuffer, m_imgBuffer + size);
        m_shownPalette = m_palette;
        // ** A FORCED OBJECT AND ANOTHER IN ONE SET STAY TWO OBJECTS. ** The set was drawn as one picture and written
        // as one object, marked as forced where one of its objects was: a player that shows the forced subtitles
        // alone showed the other object with them, and the forced track a Matroska output and a demux write beside
        // the track held the other objects too. Each of the two is drawn by itself and written as an object in a
        // window of its own, with its own mark. Where that cannot be (the two touch after scaling), one object as
        // before.
        bool forcedOne = false, otherOne = false;
        for (const PgsPlacedObject& placed : m_dsObjects)
            if (const auto found = m_objects.find(placed.id); found != m_objects.end() && !found->second.rle.empty())
                (placed.forced ? forcedOne : otherOne) = true;
        if (which == 0 && forcedOne && otherOne)
        {
            if (renderTwoObjects(m_dsTime))
            {
                m_pictureShown = true;
                return;
            }
            draw(0);
        }
        scaleDrawn();
        renderTextShow(m_dsTime);
        m_pictureShown = true;
    }
    else if ((m_dsClears || m_forcedOnly) && m_pictureShown)
    {
        renderTextHide(m_dsTime);
        m_pictureShown = false;
    }
}

// The picture drawn at the size of the source (m_imgBuffer), in colours and at the size of the output
void PGSStreamReader::scaleDrawn()
{
    yuvToRgb(m_fontBorder ? Y_THRESHOLD : 0);
    BitmapInfo bmpDest;
    BitmapInfo bmpRef;

    bmpRef.buffer = reinterpret_cast<RGBQUAD*>(m_rgbBuffer);
    bmpRef.Width = m_video_width;
    bmpRef.Height = m_video_height;
    bmpDest.buffer = reinterpret_cast<RGBQUAD*>(m_scaledRgbBuffer);
    bmpDest.Width = m_scaled_width;
    bmpDest.Height = m_scaled_height;

    rescaleRGB(&bmpDest, &bmpRef);
    if (m_fontBorder)
        TextSubtitlesRender::addBorder(m_fontBorder, m_scaledRgbBuffer, m_scaled_width, m_scaled_height);
}

// The forced object of the set and the other one, each drawn by itself, packed with one palette for both, and written
// as two objects in two windows (see renderDisplaySet). False where they cannot be two: one of them draws nothing at
// the size of the output, or their boxes touch there, which two windows must not.
bool PGSStreamReader::renderTwoObjects(const int64_t inTime)
{
    using Placed = TextToPGSConverter::PlacedObject;
    const size_t size = static_cast<size_t>(m_video_width) * m_video_height;
    const size_t scaledBytes = static_cast<size_t>(m_scaled_width) * m_scaled_height * 4;
    std::vector<uint8_t> pictures[2];
    for (int part = 0; part < 2; ++part)
    {
        memset(m_imgBuffer, 0xff, size);
        memset(m_rgbBuffer, 0x00, size * 4);
        memset(m_scaledRgbBuffer, 0x00, scaledBytes);
        if (!composeDisplaySet(part + 1))
            return false;
        scaleDrawn();
        pictures[part].assign(m_scaledRgbBuffer, m_scaledRgbBuffer + scaledBytes);
    }
    // one palette for both objects: the colours of both are reduced together until they fit it
    std::vector<Placed> objects(2);
    std::vector<uint8_t> packed[2];
    uint32_t mask = 0;
    bool fits = false;
    for (int step = 0; step < 64 && !fits; ++step)
    {
        fits = true;
        for (int part = 0; part < 2 && fits; ++part)
        {
            m_render->setImageBuffer(pictures[part].data());
            fits = m_render->rlePack(mask, part == 1);
            if (!fits)
                break;
            if (m_render->m_rleLen == 0)
            {
                m_render->setImageBuffer(m_scaledRgbBuffer);
                return false;  // nothing of it is left at this size
            }
            objects[part].top = m_render->padToLeastHeight(m_render->minLine());
            objects[part].height = static_cast<uint16_t>(FFMAX(8, m_render->renderedHeight()));
            objects[part].left = m_render->m_objectLeft;
            objects[part].width = m_render->m_objectWidth;
            objects[part].forced = part == 0;
            m_render->takePacked(packed[part]);
        }
        if (!fits)
        {
            // reduce colors, as renderTextShow does
            const auto tmp = reinterpret_cast<uint8_t*>(&mask);
            const int idx = step % 4;
            tmp[idx] <<= 1;
            tmp[idx]++;
        }
    }
    m_render->setImageBuffer(m_scaledRgbBuffer);
    if (!fits)
        return false;
    const Placed& a = objects[0];
    const Placed& b = objects[1];
    const bool apart = a.left + a.width <= b.left || b.left + b.width <= a.left || a.top + a.height <= b.top ||
                       b.top + b.height <= a.top;
    const size_t room = static_cast<size_t>(m_scaled_width + 16) * m_scaled_height + 16384;
    if (!apart || packed[0].size() + packed[1].size() + 4096 > room)
        return false;

    m_renderedBlocks.clear();
    m_firstRenderedPacket = true;
    m_twoWindows = objects;
    // The times, as renderTextShow has them for one object: the plane is cleared while the first object is decoded,
    // the second is decoded after the first, and the windows are drawn one after the other.
    const double pixels[2] = {static_cast<double>(a.width) * a.height, static_cast<double>(b.width) * b.height};
    const int64_t decodeTime[2] = {TextToPGSConverter::objectDecodeTime(pixels[0]),
                                   TextToPGSConverter::objectDecodeTime(pixels[1])};
    const int64_t drawTime =
        TextToPGSConverter::windowDrawTime(pixels[0]) + TextToPGSConverter::windowDrawTime(pixels[1]);
    const int64_t decodeDuration =
        FFMAX(TextToPGSConverter::windowDrawTime(static_cast<double>(m_scaled_width) * m_scaled_height),
              decodeTime[0] + decodeTime[1]) +
        drawTime;
    int64_t pts = gridTime(inTime);
    int64_t dts = pts - decodeDuration;
    if (m_demuxMode && dts < 1)
    {
        pts += 1 - dts;
        dts = 1;
    }
    keepSetOrder(pts, dts, drawTime);
    const int64_t firstDone = dts + decodeTime[0];
    const int64_t bothDone = firstDone + decodeTime[1];
    m_setWritten = true;
    m_lastSetPts = pts;
    m_lastSetEndDts = bothDone;

    uint8_t* curPos = m_renderedData;
    int rLen = m_render->composePresentationSegment(curPos, CompositionMode::Start, pts, dts, objects, m_demuxMode);
    m_renderedBlocks.emplace_back(pts, dts, rLen, curPos);
    curPos += rLen;
    rLen = m_render->composeWindowDefinition(curPos, pts - drawTime, dts, objects, m_demuxMode);
    m_renderedBlocks.emplace_back(pts - drawTime, dts, rLen, curPos);
    curPos += rLen;
    rLen = m_render->composePaletteDefinition(m_render->m_paletteByColor, curPos, dts, dts, m_demuxMode);
    m_renderedBlocks.emplace_back(dts, dts, rLen, curPos);
    curPos += rLen;
    m_render->givePacked(packed[0], a.width);
    rLen = m_render->composeObjectDefinition(curPos, firstDone, dts, 0, a.height - 1, m_demuxMode, 0);
    m_renderedBlocks.emplace_back(firstDone, dts, rLen, curPos);
    curPos += rLen;
    m_render->givePacked(packed[1], b.width);
    rLen = m_render->composeObjectDefinition(curPos, bothDone, firstDone, 0, b.height - 1, m_demuxMode, 1);
    m_renderedBlocks.emplace_back(bothDone, firstDone, rLen, curPos);
    curPos += rLen;
    rLen = m_render->composeEnd(curPos, bothDone, bothDone, m_demuxMode);
    m_renderedBlocks.emplace_back(bothDone, bothDone, rLen, curPos);
    return true;
}

// The time of a display set on the frame grid of the video, in 90 kHz ticks: rounded to the nearest frame as
// TextToPGSConverter::alignToGrid does, at the exact rate of the PGS frame rate code (24000/1001 for fps=23.976), as
// the bitmap path has it. At 23.976 itself the grid would drift from the frames by a tick every 6.5 frames.
int64_t PGSStreamReader::gridTime(const int64_t internalTime) const
{
    const double sec = static_cast<double>(internalTime) / INTERNAL_PTS_FREQ;
    double rate = nearestPgsFrameRate(m_render->m_videoFps);
    if (rate == 0.0)
        rate = m_render->m_videoFps;
    if (rate <= 0.0)
        return llround(sec * PCR_FREQUENCY);
    return llround(round(sec * rate + 1e-6) / rate * PCR_FREQUENCY);
}

// The first time of the frame grid at or after a time in 90 kHz ticks
int64_t PGSStreamReader::gridTimeFrom(const int64_t ticks) const
{
    double rate = nearestPgsFrameRate(m_render->m_videoFps);
    if (rate == 0.0)
        rate = m_render->m_videoFps;
    if (rate <= 0.0)
        return ticks;
    auto frame = static_cast<int64_t>(ceil(static_cast<double>(ticks) * rate / PCR_FREQUENCY - 1e-6));
    while (llround(static_cast<double>(frame) / rate * PCR_FREQUENCY) < ticks) ++frame;
    return llround(static_cast<double>(frame) / rate * PCR_FREQUENCY);
}

// ** THE DISPLAY SETS ARE WRITTEN IN THEIR ORDER. ** Each set was timed on its own, back from its presentation time by
// its decoding, and in a demux moved on its own to be decoded from the first tick. Several sets at one time (a picture,
// a clear and the next picture, all at 0 s) came out with decoding times that go back, and in a demux at 5988, 157 and
// 6031. A set is now decoded after the END of the set before and shown later than that set by the drawing of its
// window, at the first frame where it can be; a set that already was is not moved.
void PGSStreamReader::keepSetOrder(int64_t& pts, int64_t& dts, const int64_t drawTime) const
{
    if (!m_setWritten)
        return;
    int64_t later = (std::max)(int64_t{0}, m_lastSetEndDts - dts);
    if (pts + later <= m_lastSetPts + drawTime)
        later = m_lastSetPts + drawTime + 1 - pts;
    if (later == 0)
        return;
    const int64_t newPts = gridTimeFrom(pts + later);
    dts += newPts - pts;
    pts = newPts;
}

void PGSStreamReader::rescaleRGB(const BitmapInfo* bmpDest, const BitmapInfo* bmpRef)
{
    const double xFactor = static_cast<double>(bmpRef->Width) / static_cast<double>(bmpDest->Width);
    const double yFactor = static_cast<double>(bmpRef->Height) / static_cast<double>(bmpDest->Height);
    RGBQUAD* ImagePixels = bmpDest->buffer;

    for (int yDest = 0; yDest < bmpDest->Height; yDest++)
    {
        for (int xDest = 0; xDest < bmpDest->Width; xDest++)
        {
            const int floor_x = static_cast<int>(floor(xDest * xFactor));
            const int floor_y = static_cast<int>(floor(yDest * yFactor));
            const int ceil_x = FFMIN(bmpRef->Width - 1, floor_x + 1);
            const int ceil_y = FFMIN(bmpRef->Height - 1, floor_y + 1);
            const double fraction_x = xDest * xFactor - floor_x;
            const double fraction_y = yDest * yFactor - floor_y;
            const double one_minus_x = 1.0 - fraction_x;
            const double one_minus_y = 1.0 - fraction_y;
            int offset = floor_y * bmpRef->Width;
            const RGBQUAD* c1 = bmpRef->buffer + offset;
            const RGBQUAD* c2 = c1;
            c1 += floor_x;
            c2 += ceil_x;
            offset = ceil_y * bmpRef->Width;
            const RGBQUAD* c3 = bmpRef->buffer + offset;
            const RGBQUAD* c4 = c3;
            c3 += floor_x;
            c4 += ceil_x;
            double b1 = one_minus_x * c1->rgbRed + fraction_x * c2->rgbRed;
            double b2 = one_minus_x * c3->rgbRed + fraction_x * c4->rgbRed;
            ImagePixels->rgbRed = static_cast<uint8_t>(one_minus_y * b1 + fraction_y * b2);
            b1 = one_minus_x * c1->rgbGreen + fraction_x * c2->rgbGreen;
            b2 = one_minus_x * c3->rgbGreen + fraction_x * c4->rgbGreen;
            ImagePixels->rgbGreen = static_cast<uint8_t>(one_minus_y * b1 + fraction_y * b2);
            b1 = one_minus_x * c1->rgbBlue + fraction_x * c2->rgbBlue;
            b2 = one_minus_x * c3->rgbBlue + fraction_x * c4->rgbBlue;
            ImagePixels->rgbBlue = static_cast<uint8_t>(one_minus_y * b1 + fraction_y * b2);
            b1 = one_minus_x * c1->rgbReserved + fraction_x * c2->rgbReserved;
            b2 = one_minus_x * c3->rgbReserved + fraction_x * c4->rgbReserved;
            ImagePixels->rgbReserved = static_cast<uint8_t>(one_minus_y * b1 + fraction_y * b2);
            ImagePixels++;
        }
    }
}

void PGSStreamReader::renderTextShow(int64_t inTime)
{
    m_renderedBlocks.clear();
    m_firstRenderedPacket = true;
    m_twoWindows.clear();

    uint32_t mask = 0;
    int step = 0;
    while (!m_render->rlePack(mask))
    {
        // reduce colors
        const auto tmp = reinterpret_cast<uint8_t*>(&mask);
        const int idx = step++ % 4;
        tmp[idx] <<= 1;
        tmp[idx]++;
    }
    // The window and the object are the box around what the scaled picture draws, at its place in that picture
    // (TextToPGSConverter::packDrawnBox): they were as wide as the picture. Each drawn display set starts an epoch,
    // which only the clear of renderTextHide continues, with the same window. Where nothing is drawn, the object is 8x8
    // transparent pixels.
    // The window and the object start at the picture's first line. They were placed at its last line, so a rescaled
    // picture lay below its place and partly or wholly outside the frame.
    if (m_render->m_rleLen == 0)
        m_render->m_objectWidth = 8;
    m_objectWindowTop = m_render->padToLeastHeight(m_render->minLine());
    m_objectWindowHeight = static_cast<uint16_t>(FFMAX(8, m_render->renderedHeight()));
    m_objectWindowLeft = m_render->m_objectLeft;
    m_objectWindowWidth = m_render->m_objectWidth;
    // ** THE TIMES ARE THOSE OF THE TEXT AND BITMAP PATHS. ** The time is on the frame grid of the video; the plane is
    // cleared while the object is decoded, then the window is drawn, each time rounded up on its own. The set was
    // decoded the clearing and the drawing before it, and the times were rounded up by adding 0.999.
    const double objectPixels = static_cast<double>(m_objectWindowHeight) * m_objectWindowWidth;
    const int64_t decodeTime = TextToPGSConverter::objectDecodeTime(objectPixels);
    const int64_t drawTime = TextToPGSConverter::windowDrawTime(objectPixels);
    const int64_t decodeDuration =
        FFMAX(TextToPGSConverter::windowDrawTime(static_cast<double>(m_scaled_width) * m_scaled_height), decodeTime) +
        drawTime;
    int64_t pts = gridTime(inTime);
    int64_t dts = pts - decodeDuration;
    // --demux: a .sup holds no time below 0, and a decoding time of 0 there stands for the presentation time. A picture
    // at the start was written with a decoding time below 0, which came out as a time near 13 hours 15 minutes. The set
    // is moved later until it is decoded from the first tick: it is shown as soon as it can be decoded, and every
    // later set keeps its time.
    if (m_demuxMode && dts < 1)
    {
        pts += 1 - dts;
        dts = 1;
    }
    keepSetOrder(pts, dts, drawTime);
    const int64_t odfPTS = dts + decodeTime;
    m_setWritten = true;
    m_lastSetPts = pts;
    m_lastSetEndDts = odfPTS;

    uint8_t* curPos = m_renderedData;
    int rLen = m_render->composePresentationSegment(curPos, CompositionMode::Start, pts, dts, m_objectWindowLeft,
                                                    m_objectWindowTop, m_demuxMode, m_dsForced);
    m_renderedBlocks.emplace_back(pts, dts, rLen, curPos);
    curPos += rLen;
    rLen = m_render->composeWindowDefinition(curPos, pts - drawTime, dts, m_objectWindowLeft, m_objectWindowTop,
                                             m_objectWindowWidth, m_objectWindowHeight, m_demuxMode);
    m_renderedBlocks.emplace_back(pts - drawTime, dts, rLen, curPos);
    curPos += rLen;
    rLen = m_render->composePaletteDefinition(m_render->m_paletteByColor, curPos, dts, dts, m_demuxMode);
    m_renderedBlocks.emplace_back(dts, dts, rLen, curPos);
    curPos += rLen;
    rLen = m_render->composeObjectDefinition(curPos, odfPTS, dts, 0, m_objectWindowHeight - 1, m_demuxMode);
    m_renderedBlocks.emplace_back(odfPTS, dts, rLen, curPos);
    curPos += rLen;
    rLen = m_render->composeEnd(curPos, odfPTS, odfPTS, m_demuxMode);
    m_renderedBlocks.emplace_back(odfPTS, odfPTS, rLen, curPos);
}

void PGSStreamReader::renderTextHide(int64_t outTime)
{
    // The clear takes the drawing of the window and one tick, and ends at its decoding time, as the clear of a drawn
    // text does. It was decoded 90 ticks before that. A set of two objects is cleared with its two windows.
    int64_t drawTime =
        TextToPGSConverter::windowDrawTime(static_cast<double>(m_objectWindowHeight) * m_objectWindowWidth);
    if (!m_twoWindows.empty())
    {
        drawTime = 0;
        for (const auto& window : m_twoWindows)
            drawTime += TextToPGSConverter::windowDrawTime(static_cast<double>(window.width) * window.height);
    }
    int64_t pts = gridTime(outTime);
    int64_t dts = pts - drawTime - 1;
    if (m_demuxMode && dts < 1)
    {
        pts += 1 - dts;
        dts = 1;
    }
    keepSetOrder(pts, dts, drawTime);
    m_setWritten = true;
    m_lastSetPts = pts;
    m_lastSetEndDts = dts;

    m_firstRenderedPacket = true;
    m_renderedBlocks.clear();
    uint8_t* curPos = m_renderedData;
    int rLen = m_twoWindows.empty()
                   ? m_render->composePresentationSegment(curPos, CompositionMode::Finish, pts, dts, m_objectWindowLeft,
                                                          m_objectWindowTop, m_demuxMode, false)
                   : m_render->composePresentationSegment(curPos, CompositionMode::Finish, pts, dts, m_twoWindows,
                                                          m_demuxMode);
    m_renderedBlocks.emplace_back(pts, dts, rLen, curPos);
    curPos += rLen;
    rLen = m_twoWindows.empty()
               ? m_render->composeWindowDefinition(curPos, pts - drawTime, dts, m_objectWindowLeft, m_objectWindowTop,
                                                   m_objectWindowWidth, m_objectWindowHeight, m_demuxMode)
               : m_render->composeWindowDefinition(curPos, pts - drawTime, dts, m_twoWindows, m_demuxMode);
    m_renderedBlocks.emplace_back(pts - drawTime, dts, rLen, curPos);
    curPos += rLen;
    rLen = m_render->composeEnd(curPos, dts, dts, m_demuxMode);
    m_renderedBlocks.emplace_back(dts, dts, rLen, curPos);
}

int64_t getTimeValueNano(uint8_t* pos)
{
    const auto pts = static_cast<int64_t>(AV_RB32(pos));
    if (pts > 0xff000000)
        return ptsToInternalClock(pts - 0x100000000);
    return ptsToInternalClock(pts);
}

int64_t getTimeValueNano(const int64_t pts)
{
    if (pts > 0x1ff000000)
        return ptsToInternalClock(pts - 0x200000000);
    return ptsToInternalClock(pts);
}

// The next "PG" marker at or after `from` that begins a plausible segment: a known segment type, and a length that
// ends at another "PG" or where the data ends. Nothing when the data holds none.
static uint8_t* supMarkerIn(uint8_t* from, const uint8_t* end)
{
    for (uint8_t* p = from; p + 13 <= end; ++p)
    {
        if (p[0] != 'P' || p[1] != 'G')
            continue;
        const uint8_t type = p[10];
        if (type != PALETTE_DEF_SEGMENT && type != OBJECT_DEF_SEGMENT && type != PCS_DEF_SEGMENT &&
            type != WINDOWS_DEF_SEGMENT && type != 0x80)
            continue;
        const uint8_t* next = p + 13 + AV_RB16(p + 11);
        if (next + 2 > end || (next[0] == 'P' && next[1] == 'G'))
            return p;
    }
    return nullptr;
}

uint8_t* PGSStreamReader::findSupMarker(uint8_t* from) const { return supMarkerIn(from, m_bufEnd); }

void PGSStreamReader::warnSupDamage()
{
    if (m_supDamagedSegments++ == 0)
        LTRACE(LT_WARN, 2,
               "Warning: PGS stream (track " << m_streamIndex
                                             << "): a damaged segment in the .sup file was skipped. Reading goes on "
                                                "at the next segment.");
}

// ** forced-pictures: THE FORCED PICTURES ALONE. ** The packets of the source carry its segments, one to a packet, a
// long one in pieces. Each segment is gathered whole and given to the filter with its times, and the segments the
// filter writes are handed out in their place, each as a packet (in pieces where it is long), with the times the
// filter gave them. A stream drawn at another size is not filtered here: its packets are the drawn ones, and only its
// forced objects are drawn (renderDisplaySet).
int PGSStreamReader::readPacket(AVPacket& avPacket)
{
    if (!m_forcedOnly || m_needRescale)
    {
        m_forcedPacket = false;
        return readSourcePacket(avPacket);
    }
    for (;;)
    {
        if (!m_forcedOut.empty())
        {
            PgsForcedFilter::Segment& seg = m_forcedOut.front();
            if (m_forcedOutPos >= seg.data.size())
            {
                m_forcedOut.pop_front();
                m_forcedOutPos = 0;
                continue;
            }
            avPacket.codec = this;
            avPacket.codecID = CODEC_S_PGS;
            avPacket.stream_index = m_streamIndex;
            avPacket.duration = 0;
            avPacket.flags = 0;
            // each segment of a demux begins with its own header; in a mux the composition opens the display set
            if (m_forcedOutPos == 0 && (m_demuxMode || seg.startsSet))
                avPacket.flags += AVPacket::FORCE_NEW_FRAME;
            const size_t left = seg.data.size() - m_forcedOutPos;
            avPacket.data = seg.data.data() + m_forcedOutPos;
            avPacket.size = static_cast<int>((std::min)(left, static_cast<size_t>(MAX_AV_PACKET_SIZE)));
            avPacket.pts = ptsToInternalClock((std::max)(int64_t{0}, seg.pts));
            avPacket.dts = ptsToInternalClock((std::max)(int64_t{0}, seg.dts));
            m_forcedOutPos += static_cast<size_t>(avPacket.size);
            m_forcedPacket = true;
            return 0;
        }
        AVPacket in;
        const int rez = readSourcePacket(in);
        // The first composition tells whether the stream is drawn at another size, and it is read here: from it on
        // the packets are the drawn ones, handed on as they are. They were taken for segments of the source and
        // given to the filter, and the line wrote an empty track.
        if (m_needRescale)
        {
            avPacket = in;
            m_forcedPacket = false;
            return rez;
        }
        if (rez != 0)
            return rez;
        if (in.size <= 0 || in.data == nullptr)
            continue;
        if (m_forcedIn.empty())
        {
            m_forcedInPts = internalClockToPts(in.pts);
            m_forcedInDts = internalClockToPts(in.dts);
        }
        m_forcedIn.insert(m_forcedIn.end(), in.data, in.data + in.size);
        if (m_forcedIn.size() >= 3 && m_forcedIn.size() >= 3u + AV_RB16(m_forcedIn.data() + 1))
        {
            m_forcedFilter.push(m_forcedIn.data(), 3u + AV_RB16(m_forcedIn.data() + 1), m_forcedInPts, m_forcedInDts,
                                m_forcedOut);
            m_forcedIn.clear();
        }
    }
}

int PGSStreamReader::readSourcePacket(AVPacket& avPacket)
{
    avPacket.size = 0;
    avPacket.codec = this;
    avPacket.codecID = CODEC_S_PGS;
    avPacket.stream_index = m_streamIndex;
    avPacket.data = nullptr;
    avPacket.size = 0;
    avPacket.flags = 0;
    avPacket.duration = 0;

    if (!m_renderedBlocks.empty())
    {  // rendered data block (rescaled PGS). send it.
        if (m_firstRenderedPacket)
            avPacket.flags += AVPacket::FORCE_NEW_FRAME;
        m_firstRenderedPacket = false;
        PGSRenderedBlock& block = m_renderedBlocks[0];
        avPacket.data = block.data;
        avPacket.size = FFMIN(MAX_AV_PACKET_SIZE, block.len);
        avPacket.pts = block.pts * INT_FREQ_TO_TS_FREQ;
        avPacket.dts = block.dts * INT_FREQ_TO_TS_FREQ;
        block.data += avPacket.size;
        block.len -= avPacket.size;
        if (block.len == 0)
        {
            m_renderedBlocks.erase(m_renderedBlocks.begin());
            m_firstRenderedPacket = true;
        }
        return 0;
    }

    if (m_curPos == nullptr)
        return NEED_MORE_DATA;

    if (m_video_height == 0)
    {
        // A .sup with bytes in front of its first marker: the size was looked for in those bytes, never found, and the
        // reader waited for it without reading anything. Bytes in front that begin with "PG" themselves are no marker
        // either: the first plausible segment is looked for in every case.
        uint8_t* from = m_curPos;
        if (m_containerType == ContainerType::ctSUP && m_bufEnd - from >= 2)
            if (uint8_t* next = findSupMarker(from))
                from = next;
        intDecodeStream(from, m_bufEnd - from);
        if (m_video_height == 0)
            return NEED_MORE_DATA;
        // Random bytes on an S_HDMV/PGS line hold a "PG" and a segment code 0x16 by chance, and their "size" of
        // 15162:26915 at frame rate 0 was scaled to 1920:1080 and muxed as subtitles, at exit 0
        if (m_badDescriptor)
            THROW(ERR_SOURCE, "Track " << m_streamIndex << " is not PGS: its first composition gives a picture of "
                                       << m_video_width << ':' << m_video_height << " at frame rate " << m_frame_rate
                                       << ", which no PGS stream has. The usual reason is that the codec on the meta "
                                          "line is not the codec the file holds. List the file on its own to see what "
                                          "tsMuxeR makes of it.")

        if (m_newFps == 0.0 && m_scaled_width && m_scaled_height)
        {
            // a line with the size and no fps=: the size is chosen at the stream's own rate, now that it is known,
            // and a picture drawn again is written at that rate (see setVideoInfo)
            TextToPGSConverter::enlargeCrop(m_scaled_width, m_scaled_height, m_frame_rate, &m_scaled_width,
                                            &m_scaled_height);
            m_render->setVideoInfo(m_scaled_width, m_scaled_height, m_frame_rate);
        }
        // A presentation graphics plane is at most 1920x1080, on a UHD disc too. A larger stream was kept as it was,
        // with or without a size on the line: it is scaled to 1920x1080 now.
        const bool oversized = m_video_width > 1920 || m_video_height > 1080;
        if (oversized && !(m_scaled_width && m_scaled_height))
        {
            const double rate = m_newFps != 0.0 ? m_newFps : m_frame_rate;
            TextToPGSConverter::enlargeCrop(m_video_width, m_video_height, rate, &m_scaled_width, &m_scaled_height);
            m_render->setVideoInfo(m_scaled_width, m_scaled_height, rate);
        }
        uint16_t tmpWidth;
        uint16_t tmpHeight;
        // the stream's own size at the frame rate its target was chosen at, so that a stream already at the size the
        // line asks for is left as it is
        TextToPGSConverter::enlargeCrop(m_video_width, m_video_height, m_newFps != 0.0 ? m_newFps : m_frame_rate,
                                        &tmpWidth, &tmpHeight);
        m_needRescale = oversized || (m_scaled_width && m_scaled_width != tmpWidth) ||
                        (m_scaled_height && m_scaled_height != tmpHeight);
        // At the line's rate a 720x480 stream counts as 720x576 when the line says 25 fps, so it was not scaled for
        // video of 576 lines, and a 720x576 stream not for video of 480. A stream whose height is neither the video's
        // nor the target's is scaled; one with the video's own height is still left as it is.
        if (m_lineHeight && m_scaled_height && m_video_height != m_lineHeight && m_video_height != m_scaled_height)
            m_needRescale = true;
        if (m_needRescale)
        {
            const unsigned size = m_video_width * m_video_height;
            const unsigned scaled_size = m_scaled_width * m_scaled_height;
            m_imgBuffer = new uint8_t[size]{0xff};
            m_rgbBuffer = new uint8_t[size << 2]{};
            m_scaledRgbBuffer = new uint8_t[scaled_size << 2]{};
            m_renderedData = new uint8_t[(m_scaled_width + 16) * m_scaled_height + 16384];
            m_render->setImageBuffer(m_scaledRgbBuffer);
        }
        LTRACE(LT_INFO, 2,
               (m_renderedFromText ? "Drawing text subtitles into pictures (track " : "Decoding PGS stream (track ")
                   << m_streamIndex << "): " << " Resolution: " << m_video_width << ':' << m_video_height
                   << "  Frame rate: " << m_frame_rate);
        // ** SUBTITLES OF THE VIDEO'S OWN FILE KEEP THEIR TIMES. ** fps= on a PGS line converts the track: its times
        // are scaled from the frame rate its display sets carry to the one given, which is right for subtitles made
        // for another version of the film (a track of 25 fps beside a video of 23.976). The window gives every
        // subtitle track the frame rate of the video ("Bind to video FPS"), and so a track that came in the SAME file
        // as the video was converted as well, where its display sets say another rate than the video has: marked
        // 59.94 beside a video of 60 fps, every time was scaled by 59.94/60, and the subtitles of a film of 45 minutes
        // ran 2.7 s out of step by its end, sound and picture staying together (#60). The times of such a track are
        // those of its video already. They are scaled only by what the video itself is changed by (its stream's rate
        // against the rate it is muxed at, as fps= on the video line sets it), and the rate of the display sets is
        // written as the one given. A track of another file is converted as before.
        if (m_newFps != 0.0 && m_videoOfItsFile && !m_rateGuessed)
        {
            double streamRate = 0, muxRate = 0;
            m_videoOfItsFile(streamRate, muxRate);
            const double change = streamRate > 0 && muxRate > 0 ? streamRate / muxRate : 1.0;
            // A track read from a transport stream comes as PES, whose times the TS demuxer has put on the clock of the
            // video's first picture: they are scaled as those of a .sup file are (m_scalePes). They were not, while
            // the log said they followed the video, and its subtitles moved against the picture by the change.
            if (fabs(change - 1.0) > 1e-6)
            {
                LTRACE(LT_INFO, 2,
                       "The video of its file is changed from " << streamRate << " to " << muxRate
                                                                << " fps: the times of PGS stream #" << m_streamIndex
                                                                << " follow it.");
                m_scale = change;
                m_scalePes = true;
            }
            if (fabs(m_newFps - m_frame_rate) > 1e-4)
            {
                LTRACE(LT_INFO, 2,
                       "PGS stream #" << m_streamIndex << " is marked with frame rate " << m_newFps << " in place of "
                                      << m_frame_rate << ". Its times are those of the video of its file, and stay.");
                m_replaceRate = true;
            }
        }
        else if (m_newFps != 0.0 && fabs(m_newFps - m_frame_rate) > 1e-4 && m_rateGuessed)
        {
            LTRACE(LT_INFO, 2, "PGS stream #" << m_streamIndex << " is marked with frame rate " << m_newFps);
            m_replaceRate = true;
        }
        else if (m_newFps != 0.0 && fabs(m_newFps - m_frame_rate) > 1e-4)
        {
            LTRACE(LT_INFO, 2,
                   "Change FPS from " << m_frame_rate << " to " << m_newFps << " for PGS stream #" << m_streamIndex);
            m_scale = m_frame_rate / m_newFps;
        }
        if (m_needRescale)
            LTRACE(LT_INFO, 2,
                   "Change PGS resolution from " << m_video_width << ':' << m_video_height << " to " << m_scaled_width
                                                 << ':' << m_scaled_height
                                                 << ". Scaling method: Bilinear interpolation");
    }

    if (m_state == State::stAVPacketFragmented)
    {
        int avLen = static_cast<int>(m_avFragmentEnd - m_curPos);
        if (avLen > MAX_AV_PACKET_SIZE)
        {
            avLen = MAX_AV_PACKET_SIZE;
        }
        else
            m_state = State::stParsePES;
        avPacket.pts = m_lastPTS;
        avPacket.dts = m_lastDTS;
        avPacket.data = m_curPos;
        avPacket.size = m_needRescale ? 0 : avLen;
        m_curPos += avLen;
        m_processedSize += avLen;
        if (m_needRescale)
        {
            avPacket.dts = FFMAX(0, avPacket.dts);
            avPacket.pts = FFMAX(0, avPacket.pts);
        }
        return 0;
    }

    if (m_bufEnd - m_curPos < 3)
    {
        m_tmpBufferLen = m_bufEnd - m_curPos;
        memmove(m_tmpBuffer.data(), m_curPos, static_cast<size_t>(m_tmpBufferLen));
        return NEED_MORE_DATA;
    }
    bool pgStartCode = m_curPos[0] == 'P' && m_curPos[1] == 'G';
    // .sup input: a segment is followed by the next "PG" marker. When it is not, the bytes before the next marker are
    // skipped, with a warning. In front of the first segment, bytes that begin with "PG" ("PGPG...", "PGx") were taken
    // for its marker, and nothing of the file was muxed: there the marker has to begin a plausible segment.
    if (m_containerType == ContainerType::ctSUP && m_supExpectMarker &&
        (!pgStartCode || (!m_supStarted && findSupMarker(m_curPos) != m_curPos)))
    {
        uint8_t* next = findSupMarker(m_curPos);
        if (next == nullptr)
        {
            // keep the last bytes, a marker may begin in them
            if (m_bufEnd - m_curPos > 12)
            {
                m_processedSize += m_bufEnd - 12 - m_curPos;
                m_curPos = m_bufEnd - 12;
            }
            m_tmpBufferLen = m_bufEnd - m_curPos;
            memmove(m_tmpBuffer.data(), m_curPos, static_cast<size_t>(m_tmpBufferLen));
            return NEED_MORE_DATA;
        }
        warnSupDamage();
        m_processedSize += next - m_curPos;
        m_curPos = next;
        pgStartCode = true;
    }
    if (pgStartCode)
    {
        m_supExpectMarker = false;
        m_supStarted = true;
        if (m_bufEnd - m_curPos < 10)
        {
            m_tmpBufferLen = m_bufEnd - m_curPos;
            memmove(m_tmpBuffer.data(), m_curPos, static_cast<size_t>(m_tmpBufferLen));
            return NEED_MORE_DATA;
        }
        // ** A LATER .SUP OF A JOIN STARTS WHERE ITS VIDEO DOES. ** Its segments came at their own times, those of
        // the file before. The first segment read after a later file's first block arrived is that file's: a file
        // ends with a whole segment, which is read before more data is asked for.
        if (m_joinPending)
        {
            m_joinPending = false;
            ++m_joinFile;
            if (m_joinFile < m_joinStartsNs.size())
                m_joinShift = m_joinStartsNs[m_joinFile] * (INTERNAL_PTS_FREQ / 1000000) / 1000;
        }
        m_lastPTS = static_cast<int64_t>(static_cast<double>(getTimeValueNano(m_curPos + 2)) * m_scale);
        m_lastDTS = static_cast<int64_t>(static_cast<double>(getTimeValueNano(m_curPos + 6)) * m_scale);
        if (m_lastDTS == 0)
            m_lastDTS = m_lastPTS;
        m_lastPTS += m_joinShift;
        m_lastDTS += m_joinShift;
        m_maxPTS = FFMAX(m_maxPTS, m_lastPTS);
        m_curPos += 10;
        m_processedSize += 10;
        m_state = State::stParsePGS;
        avPacket.pts = m_lastPTS;
        avPacket.dts = m_lastDTS;
        m_isNewFrame = true;

        if (m_needRescale)
        {
            avPacket.dts = FFMAX(0, avPacket.dts);
            avPacket.pts = FFMAX(0, avPacket.pts);
        }
        return 0;
    }

    const bool pesStartCode = m_curPos[0] == 0 && m_curPos[1] == 0 && m_curPos[2] == 01;

    if (pesStartCode)
    {
        if (m_bufEnd - m_curPos < 8)
        {
            m_tmpBufferLen = m_bufEnd - m_curPos;
            memmove(m_tmpBuffer.data(), m_curPos, static_cast<size_t>(m_tmpBufferLen));
            return NEED_MORE_DATA;
        }
        const auto pesPacket = reinterpret_cast<PESPacket*>(m_curPos);
        const int pesHeaderLen = pesPacket->getHeaderLength();
        // if (m_bufEnd - m_curPos < pesHeaderLen+1) {
        if (m_bufEnd - m_curPos < pesHeaderLen)
        {
            m_tmpBufferLen = m_bufEnd - m_curPos;
            memmove(m_tmpBuffer.data(), m_curPos, static_cast<size_t>(m_tmpBufferLen));
            return NEED_MORE_DATA;
        }
        // scaled only where its own video's speed is changed; fps= on a track of another transport stream does not
        // convert it, as before
        const double pesScale = m_scalePes ? m_scale : 1.0;
        if (pesPacket->flagsLo & 0x80)
        {  // pts exists
            m_lastDTS = m_lastPTS =
                static_cast<int64_t>(static_cast<double>(getTimeValueNano(pesPacket->getPts())) * pesScale);
            m_maxPTS = FFMAX(m_lastPTS, m_maxPTS);
        }
        if ((pesPacket->flagsLo & 0xc0) == 0xc0)  // dts exists
            m_lastDTS = static_cast<int64_t>(static_cast<double>(getTimeValueNano(pesPacket->getDts())) * pesScale);
        m_curPos += pesHeaderLen;
        m_processedSize += pesHeaderLen;
        m_state = State::stParsePGS;

        avPacket.pts = m_lastPTS;
        avPacket.dts = m_lastDTS;
        m_isNewFrame = true;
        // m_afterPesByte = *m_curPos;
        if (m_needRescale)
        {
            avPacket.dts = FFMAX(0, avPacket.dts);
            avPacket.pts = FFMAX(0, avPacket.pts);
        }
        return 0;
    }

    avPacket.data = m_curPos;
    if (m_bufEnd - m_curPos < 3)
    {
        m_tmpBufferLen = m_bufEnd - m_curPos;
        memmove(m_tmpBuffer.data(), m_curPos, static_cast<size_t>(m_tmpBufferLen));
        return NEED_MORE_DATA;
    }
    const uint8_t segment_type = *m_curPos;
    const auto segment_len = AV_RB16(m_curPos + 1);
    if (m_bufEnd - m_curPos < 3ll + segment_len)
    {
        m_tmpBufferLen = m_bufEnd - m_curPos;
        memmove(m_tmpBuffer.data(), m_curPos, static_cast<size_t>(m_tmpBufferLen));
        return NEED_MORE_DATA;
    }
    if (m_containerType == ContainerType::ctSUP)
    {
        // A segment that lost bytes claims more than it holds: the next marker comes before the end its length gives.
        // It is dropped and reading goes on at that marker. Before, the reader went on at the claimed end, in the
        // middle of the next segment, and every subtitle after it was lost without a word.
        uint8_t* claimedEnd = m_curPos + 3 + segment_len;
        if (claimedEnd + 2 <= m_bufEnd && !(claimedEnd[0] == 'P' && claimedEnd[1] == 'G'))
        {
            uint8_t* next = findSupMarker(m_curPos + 3);
            if (next != nullptr && next < claimedEnd)
            {
                warnSupDamage();
                m_processedSize += next - m_curPos;
                m_curPos = next;
                m_supExpectMarker = false;
                return readPacket(avPacket);
            }
        }
        m_supExpectMarker = true;
    }
    m_curPos += 3;
    BitStreamReader bitReader{};
    try
    {
        uint8_t number_of_composition_objects, number_of_windows;
        switch (segment_type)
        {
        case PALETTE_DEF_SEGMENT:
            // pallete definition
            // m_palleteID = m_curPos[0];
            // m_paletteVersion = m_curPos[1];
            if (m_needRescale)
                readPalette(m_curPos, m_curPos + segment_len);
            // LTRACE(LT_INFO, 2, "PGS #" << m_streamIndex << " palette definition");
            break;
        case OBJECT_DEF_SEGMENT:
            // object definition
            // LTRACE(LT_INFO, 2, "PGS #" << m_streamIndex << " object definition size " << segment_len
            //	  << " objectID=" << (int)m_curPos[0] << " version=" << (int) m_curPos[2]);
            if (m_needRescale && m_dsOpen)
            {
                readObjectDef(m_curPos, m_curPos + segment_len);
                m_dsHasObjectData = true;
            }
            break;
        case PCS_DEF_SEGMENT:
            // Presentation Composition Segment
            // The rate was written only where the FIRST PCS differs from fps=, so a stream whose display sets give
            // more than one rate kept the others as they were. With fps= every PCS gets its rate; the times are
            // scaled as the first PCS decided.
            {
                // Its first 5 bytes are the picture size and the frame rate, read and, with fps=, written here: a
                // segment of fewer bytes, as the cut end of a file leaves, was read and written past its end.
                if (segment_len < 5)
                    break;
                ++m_compositions;
                const int ownIndex = m_curPos[4] >> 4;
                if (m_firstRateIndex < 0)
                    m_firstRateIndex = ownIndex;
                else if (ownIndex != m_firstRateIndex && !m_mixedRateSaid)
                {
                    m_mixedRateSaid = true;
                    std::ostringstream what;
                    what << "Warning: the display sets of PGS stream #" << m_streamIndex
                         << " give more than one frame rate (" << pgs_frame_rates[m_firstRateIndex] << " and "
                         << pgs_frame_rates[ownIndex] << "). ";
                    if (m_newFps != 0.0)
                        what << "Every display set is written at fps=" << m_newFps << ".";
                    else
                        what << "Give fps= with the rate of the video to write one rate.";
                    LTRACE(LT_WARN, 2, what.str());
                }
                if (fabs(m_scale - 1.0) > 1e-4 || m_replaceRate ||
                    (m_newFps != 0.0 && ownIndex != calcFpsIndex(m_newFps)))
                    m_curPos[4] = static_cast<uint8_t>(calcFpsIndex(m_newFps) << 4);
            }
            // the set before had no END: it is drawn before this PCS gives other objects and places
            if (m_needRescale && m_dsOpen)
                renderDisplaySet();
            bitReader.setBuffer(m_curPos, m_bufEnd);
            video_descriptor(bitReader);
            composition_descriptor(bitReader);

            bitReader.skipBits(16);  // palette_update_flag, palette_id_ref
            number_of_composition_objects = bitReader.getBits<uint8_t>(8);
            m_dsForced = false;
            m_dsObjects.clear();
            for (int i = 0; i < number_of_composition_objects; i++)
            {
                composition_object(bitReader);
                m_dsForced = m_dsForced || m_forced_on_flag;
            }
            // --demux: a display set whose decoding would start before 0 is written that much later (writeAdditionData)
            m_demuxDtsShift = m_demuxMode && m_lastDTS < 0 ? INT_FREQ_TO_TS_FREQ - m_lastDTS : 0;
            if (m_needRescale)
            {
                if (composition_state == CompositionState::csEpochStart)
                    m_objects.clear();
                for (auto& object : m_objects) object.second.open = false;  // no object goes on into another set
                m_dsOpen = true;
                m_dsDrawn = false;
                m_dsHasObjectData = false;
                m_dsTime = m_maxPTS;
                m_dsClears = composition_state != CompositionState::csEpochStart || number_of_composition_objects == 0;
            }
            break;
        case WINDOWS_DEF_SEGMENT:
            // Window Definition Segment
            bitReader.setBuffer(m_curPos, m_bufEnd);
            number_of_windows = bitReader.getBits<uint8_t>(8);
            for (int i = 0; i < number_of_windows; i++) pgs_window(bitReader);
            // LTRACE(LT_INFO, 2, "PGS #" << m_streamIndex << " Window Definition Segment");
            break;
        case END_DEF_SEGMENT:
            if (m_needRescale && m_dsOpen)
                renderDisplaySet();
            break;
        case 0x18:
            // Interactive Composition Segment
            // LTRACE(LT_INFO, 2, "PGS #" << m_streamIndex << " Interactive Composition Segment");
        case 0x81:
        case 0x82:
            // Used by HDMV Text subtitle streams
            // LTRACE(LT_INFO, 2, "PGS #" << m_streamIndex << " TEXT DATA");
        default:
            // LTRACE(LT_INFO, 2, "PGS #" << m_streamIndex << " unknown type " << (int)segment_type);
            break;
        }
    }
    catch (BitStreamException&)
    {
        return NEED_MORE_DATA;
    }
    avPacket.pts = m_lastPTS;
    avPacket.dts = m_lastDTS;
    if (m_isNewFrame)
    {
        if (!m_needRescale)
            avPacket.flags += AVPacket::FORCE_NEW_FRAME;
        m_isNewFrame = false;
    }

    int avLen = segment_len;
    if (avLen > MAX_AV_PACKET_SIZE - 3)
    {
        avLen = MAX_AV_PACKET_SIZE - 3;
        m_state = State::stAVPacketFragmented;
        m_avFragmentEnd = m_curPos + segment_len;
    }
    m_curPos += avLen;
    m_processedSize += 3 + avLen;
    avPacket.size = 3 + avLen;
    if (m_needRescale)
    {
        if (!m_renderedBlocks.empty())
        {
            avPacket.pts = m_renderedBlocks.begin()->pts * INT_FREQ_TO_TS_FREQ;
            avPacket.dts = m_renderedBlocks.begin()->dts * INT_FREQ_TO_TS_FREQ;
        }
        else
        {
            avPacket.dts = FFMAX(0, avPacket.dts);
            avPacket.pts = FFMAX(0, avPacket.pts);
        }
        avPacket.size = 0;  // skip source palette object
    }
    return 0;
}

int PGSStreamReader::flushPacket(AVPacket& avPacket) { return 0; }

void PGSStreamReader::setBuffer(uint8_t* data, const uint32_t dataLen, bool lastBlock)
{
    if (m_tmpBufferLen + dataLen > static_cast<int64_t>(m_tmpBuffer.size()))
        m_tmpBuffer.resize(static_cast<size_t>(m_tmpBufferLen + dataLen));

    if (!m_tmpBuffer.empty())
        memmove(m_tmpBuffer.data() + m_tmpBufferLen, data + MAX_AV_PACKET_SIZE, dataLen);
    m_tmpBufferLen += dataLen;

    if (!m_tmpBuffer.empty())
        m_curPos = m_buffer = m_tmpBuffer.data();
    else
        m_curPos = m_buffer = nullptr;
    m_bufEnd = m_buffer + m_tmpBufferLen;
    m_tmpBufferLen = 0;
}

int64_t PGSStreamReader::getProcessedSize() { return m_processedSize; }

CheckStreamRez PGSStreamReader::checkStream(uint8_t* buffer, int len, ContainerType containerType,
                                            int containerDataType, int containerStreamIndex)
{
    CheckStreamRez rez;
    if ((containerType == ContainerType::ctM2TS || containerType == ContainerType::ctTS) &&
        containerDataType == TRACKTYPE_PGS)
    {
        rez.codecInfo = pgsCodecInfo;
        rez.streamDescr = "Presentation Graphic Stream";
        if (containerStreamIndex >= 0x1200)
            rez.streamDescr +=
                std::string(" #") + int32ToStr(containerStreamIndex - (V3_flags & 0x1e ? 0x12A0 : 0x1200));
    }
    else if (containerType == ContainerType::ctMKV && containerDataType == TRACKTYPE_PGS)
    {
        rez.codecInfo = pgsCodecInfo;
        rez.streamDescr = "Presentation Graphic Stream";
        rez.streamDescr += std::string(" #") + int32ToStr(containerStreamIndex);
    }
    // The sub-pictures of a VOB, drawn as PGS (ProgramStreamDemuxer::addSubPicture): told by their stream id, as their
    // first picture can come long after the start
    else if ((containerType == ContainerType::ctVOB || containerType == ContainerType::ctEVOB) &&
             containerDataType == TRACKTYPE_PGS && containerStreamIndex >= 0x20 && containerStreamIndex <= 0x3f)
    {
        rez.codecInfo = pgsCodecInfo;
        rez.streamDescr = "DVD subtitles (sub-picture stream " + int32ToStr(containerStreamIndex - 0x20) +
                          "), drawn as Presentation Graphic Stream";
    }
    else if (containerType == ContainerType::ctSUP && len > 2)
    {
        // A few bytes in front of the first "PG" marker: the file was not taken for a .sup at all, and nothing was
        // muxed. The first plausible segment counts, as the reader finds it: within 16 bytes only, a file with 16 or
        // more bytes in front, or one that begins inside a segment, was not listed, though the reader muxes it.
        if (uint8_t* first = supMarkerIn(buffer, buffer + len))
        {
            rez.codecInfo = pgsCodecInfo;
            rez.streamDescr = std::string("Presentation Graphic Stream");
            len -= static_cast<int>(first - buffer);
            buffer = first;
        }
    }
    intDecodeStream(buffer, len);
    if (m_video_height != 0)
    {
        std::ostringstream ostr;
        ostr << " Resolution: " << m_video_width << ':' << m_video_height << " Frame rate: " << m_frame_rate;
        rez.streamDescr += ostr.str();
    }
    return rez;
}

void PGSStreamReader::intDecodeStream(uint8_t* buffer, const size_t len)
{
    const uint8_t* bufEnd = buffer + len;
    uint8_t* curPos = buffer;
    while (curPos < bufEnd)
    {
        if (bufEnd - curPos < 10)
            return;
        if (curPos[0] == 'P' && curPos[1] == 'G')
            curPos += 10;
        else if (curPos[0] == 0 && curPos[1] == 0 && curPos[2] == 1)
        {
            const auto pesPacket = reinterpret_cast<PESPacket*>(curPos);
            curPos += pesPacket->getHeaderLength();
            if (curPos >= bufEnd)
                return;
        }

        if (bufEnd - curPos < 3)
            return;
        const uint8_t segment_type = *curPos;
        const auto segment_len = AV_RB16(curPos + 1);
        if (bufEnd - curPos < 3ll + segment_len)
            return;

        curPos += 3;
        if (segment_type == 0x16)
        {
            BitStreamReader bitReader{};
            bitReader.setBuffer(curPos, bufEnd);
            video_descriptor(bitReader);
            return;
        }
        curPos += segment_len;
    }
}

int PGSStreamReader::writeAdditionData(uint8_t* dstBuffer, uint8_t* dstEnd, AVPacket& avPacket,
                                       PriorityDataInfo* priorityData)
{
    if (!m_demuxMode)
        return 0;
    if (dstEnd - dstBuffer < 10)
        THROW(ERR_INTERNAL_ERROR, "PGS stream error: Not enough buffer for write headers")
    if (!m_needRescale)
    {
        *dstBuffer++ = 'P';
        *dstBuffer++ = 'G';
        auto data = UnalignedPtr<uint32_t>(dstBuffer);
        // A display set at the start was written with a decoding time below 0, which a .sup cannot hold: it came out
        // as a time near 13 hours 15 minutes. The set is written that much later, so that it is decoded from the first
        // tick (a decoding time of 0 stands for the presentation time) and shown as soon as it can be decoded; every
        // later set keeps its time.
        // a packet of the forced pictures alone has the times the filter gave it, not those of the segment read last
        const int64_t pts = m_forcedPacket ? avPacket.pts : m_lastPTS + m_demuxDtsShift;
        const int64_t dts = m_forcedPacket ? avPacket.dts : m_lastDTS + m_demuxDtsShift;
        *data++ = my_htonl(static_cast<uint32_t>(internalClockToPts(pts)));
        *data = my_htonl(static_cast<uint32_t>(internalClockToPts(dts)));
        return 10;
    }
    return 0;
}

void PGSStreamReader::setVideoInfo(const uint16_t width, const uint16_t height, const double fps)
{
    m_scaled_width = width;
    m_scaled_height = height;
    m_lineHeight = height;
    m_newFps = fps;
    // the size at the frame rate of the line: it was chosen before the rate was known, so that a line for video of
    // 720x576 at 25 fps asked for 720x480, and a PGS stream at 720x576 was scaled down to it. Without a rate on the
    // line the size is chosen in readPacket at the stream's own rate: chosen at a rate of 0, video-width=720,
    // video-height=576 without fps= asked for 720x480 as well, and a 720x576 stream at 25 fps was scaled down to it
    // and written with frame rate code 0 in every PCS.
    if (width && height && fps != 0.0)
        TextToPGSConverter::enlargeCrop(width, height, fps, &m_scaled_width, &m_scaled_height);
    m_render->setVideoInfo(m_scaled_width, m_scaled_height, m_newFps);
}
