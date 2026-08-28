#include "simplePacketizerReader.h"

#include <fs/systemlog.h>

#include <algorithm>
#include <cstring>
#include <iostream>

#include "avCodecs.h"
#include "vodCoreException.h"
#include "vod_common.h"

SimplePacketizerReader::SimplePacketizerReader()
{
    m_needSync = true;
    m_tmpBufferLen = 0;
    m_curPts = PTS_CONST_OFFSET;
    m_frameNum = 0;
    m_processedBytes = 0;
    m_lostBytes = 0;
    m_readBytes = 0;
    m_everSynced = false;
    m_pendingLost = 0;
    m_tagCredit = 0;
    m_tagProbePos = nullptr;
    m_containerDataType = 0;
    m_containerStreamIndex = 0;
    m_stretch = 1.0;
    m_curMplsIndex = -1;
    m_lastMplsTime = 0;
    m_mplsOffset = 0;
    m_halfFrameLen = 0;
}

// Does the BODY of an ID3v2 tag look like an ID3v2 tag, rather than merely starting with "ID3"?
//
// ** THE DECLARED LENGTH LANDING ON A FRAME SYNC IS ALMOST NO EVIDENCE AT ALL, AND THAT WAS
// MEASURED, NOT ASSUMED. ** Requiring a frame of the codec to begin exactly where the tag says it
// ends LOOKS like sixteen bits of confirmation. It is worth about two, because the two numbers are
// not independent: the length field's byte 7 moves the end in steps of 16,384, and an audio stream
// has a CONSTANT frame size, so the two grids share a factor. On a real AC-3 stream, 5 x 16,384 =
// 64 x 1,280, and 26 OF THE 128 LEGAL VALUES OF THAT ONE BYTE land exactly on an 0x0B77 sync -
// every fifth one, in an arithmetic run. Measured on the file, not reasoned about.
//
// So a SINGLE FLIPPED BYTE in a length field had about a one in five chance of being believed, and
// each time it was believed it silently ate everything between the real end of the tag and the
// false one: 1,966,080 bytes of AC-3, 1,048,576 of AAC, 835,584 of the user's own MP3, all with
// exit 0 and nothing on screen. Confirming MORE frames afterwards cannot help, because a length
// that lands on a frame boundary lands on GENUINE AUDIO, which confirms perfectly.
//
// The evidence has to come from INSIDE the tag. Two things are checked, and the second is the one
// that catches a flipped length:
//
//   A. the body must BEGIN like a tag: a frame identifier of A-Z and 0-9, or 0x00 where padding
//      starts. Random audio passes this about once in 250 tries, so 10 bytes of damage that happen
//      to read "ID3" no longer swallow the rest of the file
//   B. ** PADDING MUST BE ZERO, AND THE SPECIFICATION SAYS SO IN WORDS THAT CAN BE QUOTED. ** ID3v2
//      section 3.0: "The value of the padding bytes must be $00." A corrupted length turns real
//      audio into "padding", and audio is not zero
//
// Anything this cannot understand - a tag with the whole body unsynchronised, a compressed 2.2 tag,
// an extended header that does not fit - returns "do not skip", which is exactly the behaviour of
// the unmodified reader. NOT SKIPPING IS ALWAYS SAFE. Skipping wrongly is what destroys audio.
static bool id3v2BodyIsPlausible(const uint8_t* p, const uint8_t* end, const int64_t tagLen)
{
    const int ver = p[3];
    if (ver < 2 || ver > 4)
        return false;
    const bool unsync = (p[5] & 0x80) != 0;
    const bool hasExt = (p[5] & 0x40) != 0;
    const bool hasFooter = ver >= 4 && (p[5] & 0x10) != 0;
    // 2.2 uses bit 6 for compression, not for an extended header, and says a tag it does not
    // understand should be ignored. Nothing can be walked inside one, so nothing is claimed.
    if (ver == 2 && hasExt)
        return false;

    // ** EVERYTHING BELOW COMPARES LENGTHS, NEVER POINTERS PAST THE BUFFER. ** A declared ID3v2
    // length can be 2^28, so `p + tagLen` is a pointer up to 268 MB beyond a 2 MiB buffer. Nothing
    // ever dereferenced it - the "is the whole tag here yet" test stopped that - but merely FORMING
    // a pointer that far outside an allocation is undefined behaviour, and so is comparing it. It
    // works on every flat address space there is, which is exactly what makes it easy to leave in.
    //
    // So `at` and `bodyEnd` are OFFSETS from p, and `avail` is how much is really in the buffer.
    // p[at] is read only where at < avail has been established.
    const int64_t avail = end - p;
    const int64_t bodyEnd = tagLen - (hasFooter ? 10 : 0);  // where the frames must finish
    int64_t at = 10;

    if (ver >= 3 && hasExt)
    {
        if (avail - at < 4)
            return true;  // not here yet: judged when the rest arrives, not guessed at now
        // 2.3 gives the size of what FOLLOWS the field; 2.4 gives a syncsafe size INCLUDING it.
        const uint8_t* e = p + at;
        const int64_t extSize = ver == 3 ? (static_cast<int64_t>(e[0]) << 24) | (static_cast<int64_t>(e[1]) << 16) |
                                               (static_cast<int64_t>(e[2]) << 8) | static_cast<int64_t>(e[3])
                                         : ((static_cast<int64_t>(e[0]) << 21) | (static_cast<int64_t>(e[1]) << 14) |
                                            (static_cast<int64_t>(e[2]) << 7) | static_cast<int64_t>(e[3])) -
                                               4;
        if (extSize < 0 || extSize > bodyEnd - at - 4)
            return false;
        at += 4 + extSize;
    }

    // A. The first thing in the body. Everything after this point needs the whole tag, so a body
    //    that has not arrived yet is neither accepted nor condemned here.
    const int idLen = ver == 2 ? 3 : 4;
    const int hdrLen = ver == 2 ? 6 : 10;

    // ** AN EMPTY TAG IS STILL A TAG, AND FORGETTING THAT CONDEMNED FIVE PERFECTLY GOOD FILES. ** A
    // ten byte ID3v2 header declaring a body of nothing is legal and ordinary. Reading a "frame
    // identifier" out of it reads the first four bytes of the AUDIO, which are 0xff 0xfd for MPEG,
    // so the tag was rejected and the file fell back to the unmodified reader's answer. Measured on
    // the five zero length fixtures in the block edge set, which went from exactly right to 959
    // bytes of leak before this line existed.
    if (at >= bodyEnd)
        return at == bodyEnd;

    if (avail - at < idLen)
        return true;

    // A body too short to hold even one frame identifier can only be padding, and padding is zero.
    // bodyEnd is below avail here, because avail - at is not, so every byte read is in the buffer.
    if (bodyEnd - at < idLen)
    {
        for (int64_t z = at; z < bodyEnd; ++z)
            if (p[z] != 0)
                return false;
        return true;
    }

    if (p[at] != 0)
    {
        for (int i = 0; i < idLen; ++i)
            if (!((p[at + i] >= 'A' && p[at + i] <= 'Z') || (p[at + i] >= '0' && p[at + i] <= '9')))
                return false;
    }

    // B needs every byte of the tag, and the frame sizes are only readable when the body is not
    // unsynchronised as a whole, which 2.3 permits and 2.4 replaced with a per frame flag.
    //
    // From here on bodyEnd <= avail, so the walk below cannot read outside the buffer.
    if (avail < bodyEnd || unsync)
        return true;

    while (at < bodyEnd)
    {
        if (p[at] == 0)  // padding begins, and from here to the end must be nothing else
        {
            for (int64_t z = at; z < bodyEnd; ++z)
                if (p[z] != 0)
                    return false;
            return true;
        }
        if (bodyEnd - at < hdrLen)
            return false;
        const uint8_t* f = p + at;  // in bounds: at + hdrLen <= bodyEnd <= avail
        for (int i = 0; i < idLen; ++i)
            if (!((f[i] >= 'A' && f[i] <= 'Z') || (f[i] >= '0' && f[i] <= '9')))
                return false;
        int64_t fsize;
        if (ver == 2)
            fsize = (static_cast<int64_t>(f[3]) << 16) | (static_cast<int64_t>(f[4]) << 8) | static_cast<int64_t>(f[5]);
        else if (ver == 3)
            fsize = (static_cast<int64_t>(f[4]) << 24) | (static_cast<int64_t>(f[5]) << 16) |
                    (static_cast<int64_t>(f[6]) << 8) | static_cast<int64_t>(f[7]);
        else
        {
            // 2.4 frame sizes are syncsafe. Taggers that wrote 2.3 sizes into a 2.4 tag are common
            // enough that a size with a high bit set is treated as "cannot walk this" rather than
            // as a forgery, so those tags keep the old behaviour instead of being condemned.
            if ((f[4] | f[5] | f[6] | f[7]) >= 0x80)
                return true;
            fsize = (static_cast<int64_t>(f[4]) << 21) | (static_cast<int64_t>(f[5]) << 14) |
                    (static_cast<int64_t>(f[6]) << 7) | static_cast<int64_t>(f[7]);
        }
        if (fsize < 0 || fsize > bodyEnd - at - hdrLen)
            return false;
        at += hdrLen + fsize;
    }
    return at == bodyEnd;
}

// ** THE CHECK ID3v2 HAS AND APEv2 NEVER GOT. **
//
// The APEv2 branch below used to take its declared size on faith, and that size is a LINEAR DIAL on
// what the loss report prints. Measured: an AC-3 file with a million bytes of audio wiped reports
// "1000032 bytes of the 2000626 read" correctly, and the same file with those first 32 bytes
// replaced by a well formed APEv2 header reports ** NOTHING AT ALL **, while changing one byte of
// the same header, APETAGEX to APETAGEY, brings the correct figure straight back. A tag header is
// eleven bytes of structure over a region of any size, so a corrupted one hides a loss of any size.
//
// An APEv2 header declares an item count and a size. After the 32 byte header come that many items,
// each: 4 byte little endian value size, 4 byte flags, a key of printable ASCII terminated by 0x00,
// then the value. The declared size counts the items plus the footer and EXCLUDES the header, so
// the whole tag is 32 + size. This walks exactly that many items and requires the accumulated
// length to land EXACTLY on the declared size, with either nothing left over or one consistent
// footer.
//
// ** THE FOOTER IS DECIDED BY THE EXACT LANDING, NOT BY THE FLAG BIT ALONE. ** The flag that says
// "contains no footer" is left clear by writers that do write one, so refusing a tag on that bit
// would reject genuine cover art and leak its payload.
//
// LIKE THE ID3v2 WALK, THIS IS PERMISSIVE WHEN THE BODY HAS NOT ARRIVED: a genuine tag larger than
// one read block is still credited. And like it, everything compares OFFSETS, so no pointer past
// the buffer is ever formed.
//
// ** WHAT IT DOES NOT DO, STATED PLAINLY: it validates STRUCTURE, and an item's value bytes are
// opaque by design because they carry cover art. A deliberately crafted tag can therefore declare
// one item whose value IS the audio and pass this walk. That costs an attacker eleven bytes beyond
// the header and no check confined to this reader can prevent it. This defends against CORRUPTION,
// which is what actually occurs: a genuine tag with one flipped byte, which the exact landing
// catches. It does not defend against forgery, and it does not claim to. **
static bool apev2BodyIsPlausible(const uint8_t* p, const uint8_t* end, int64_t* walkEnd)
{
    *walkEnd = 32;  // until the whole tag verifies, only the header bytes were truly seen
    const int64_t avail = end - p;
    if (avail < 32)
        return true;  // the header is not all here yet, so it is judged when it is

    const auto le32 = [](const uint8_t* q) {
        return static_cast<int64_t>(q[0]) | (static_cast<int64_t>(q[1]) << 8) |
               (static_cast<int64_t>(q[2]) << 16) | (static_cast<int64_t>(q[3]) << 24);
    };
    const int64_t version = le32(p + 8);
    const int64_t size = le32(p + 12);
    const int64_t count = le32(p + 16);
    const auto flags = static_cast<uint32_t>(le32(p + 20));

    if ((flags & 0x20000000u) == 0)
        return true;  // a footer: the items are behind us and the caller returns a fixed 32
    if (version != 2000)
        return false;  // only APEv2 carries a header at all
    if ((flags & 0x1FFFFFF8u) != 0)
        return false;  // the undefined flag bits must be zero
    if ((p[24] | p[25] | p[26] | p[27] | p[28] | p[29] | p[30] | p[31]) != 0)
        return false;  // and so must the reserved eight
    if (count > (1LL << 20))
        return false;  // not a streaming format; this is a corrupted count

    const int64_t declaredEnd = 32 + size;
    int64_t at = 32;
    for (int64_t i = 0; i < count; ++i)
    {
        if (declaredEnd - at < 8)
            return false;  // the tag does not declare room for another item prefix
        if (avail - at < 8)
            return true;  // the prefix has not arrived
        const int64_t vsize = le32(p + at);
        int64_t k = at + 8;
        int64_t klen = 0;
        for (;;)
        {
            if (k >= declaredEnd)
                return false;  // the key ran past the tag without a terminator
            if (avail - k < 1)
                return true;  // the key has not arrived
            const uint8_t c = p[k];
            if (c == 0)
                break;
            if (c < 0x20 || c > 0x7E)
                return false;  // a key is printable ASCII, so this is not one
            if (++klen > 255)
                return false;  // longer than a key may be, still with no terminator
            ++k;
        }
        if (klen < 2)
            return false;  // shorter than the two character minimum
        const int64_t valAt = k + 1;
        if (vsize > declaredEnd - valAt)
            return false;  // the value overruns the length the tag declared for itself
        const int64_t nxt = valAt + vsize;
        if (nxt > avail)
            return true;  // the value has not arrived
        at = nxt;
    }

    const int64_t leftover = declaredEnd - at;
    if (leftover == 0)
    {
        *walkEnd = declaredEnd;  // header and items, and the size lands exactly on the end
        return true;
    }
    if (leftover != 32)
        return false;  // the declared size lands neither on the end nor on a footer

    if (avail - at < 32)
        return true;  // the footer has not arrived
    const uint8_t* f = p + at;
    if (memcmp(f, "APETAGEX", 8) != 0)
        return false;
    if ((static_cast<uint32_t>(le32(f + 20)) & 0x20000000u) != 0)
        return false;  // a footer must not claim to be the header
    if (le32(f + 12) != size || le32(f + 16) != count)
        return false;  // and it must agree with the header about both figures

    *walkEnd = declaredEnd;
    return true;
}

// The length of ONE metadata tag beginning at p, or 0 if there is not one there.
//
// Only the three that actually occur in front of, behind and between elementary audio streams are
// recognised. Each one declares its own length, so nothing here is a guess about how long a tag
// might be, and an unrecognised block is still counted as lost, which is the safe direction.
static int64_t oneTagLength(const uint8_t* p, const uint8_t* end)
{
    const int64_t avail = end - p;

    // ID3v2: "ID3", two version bytes, flags, then four SYNCSAFE length bytes, each below 0x80.
    // The length excludes the 10 byte header, and a footer adds another 10.
    //
    // ** THE FOOTER FLAG EXISTS ONLY FROM ID3v2.4. ** The 2.4 specification defines the flags as
    // %abcd0000 and says of bit 4 that it "indicates that a footer is present at the very end of
    // the tag". In 2.2 and 2.3 that bit is not the footer flag and is to be ignored, so honouring
    // it there makes the length ten bytes too long and steps over the first ten bytes of audio.
    if (avail >= 10 && p[0] == 'I' && p[1] == 'D' && p[2] == '3' && p[3] != 0xff && p[4] != 0xff &&
        (p[6] | p[7] | p[8] | p[9]) < 0x80)
    {
        const int64_t size = (static_cast<int64_t>(p[6]) << 21) | (static_cast<int64_t>(p[7]) << 14) |
                             (static_cast<int64_t>(p[8]) << 7) | static_cast<int64_t>(p[9]);
        const bool hasFooter = p[3] >= 4 && (p[5] & 0x10) != 0;
        const int64_t len = 10 + size + (hasFooter ? 10 : 0);
        return id3v2BodyIsPlausible(p, end, len) ? len : 0;
    }

    // APEv2: "APETAGEX", version, then the size INCLUDING the footer but EXCLUDING the header.
    //
    // ** WHICH 32 BYTE BLOCK THIS IS DECIDES EVERYTHING, AND IT IS BIT 29, NOT BIT 31. ** Bit 31
    // says the tag CONTAINS a header; bit 29 says THIS block IS the header. Reading 31 for 29 made
    // a footer look like a header and returned the whole tag size from a position where the tag
    // body is already BEHIND us, overshooting into the audio by size minus 32.
    //
    // Landing on a FOOTER means exactly 32 bytes remain to step over. Landing on a HEADER means
    // the header plus everything it declares.
    //
    // A footer's 32 bytes are fixed and need no checking. A HEADER's length is whatever it says it
    // is, so it gets the body walk, for the reason apev2BodyIsPlausible sets out above.
    if (avail >= 32 && memcmp(p, "APETAGEX", 8) == 0)
    {
        const int64_t size = static_cast<int64_t>(p[12]) | (static_cast<int64_t>(p[13]) << 8) |
                             (static_cast<int64_t>(p[14]) << 16) | (static_cast<int64_t>(p[15]) << 24);
        const bool isHeader = (p[23] & 0x20) != 0;
        if (!isHeader)
            return 32;
        int64_t walkEnd = 32;
        return apev2BodyIsPlausible(p, end, &walkEnd) ? 32 + size : 0;
    }

    // Enhanced ID3v1, "TAG+", a fixed 227 bytes that sits immediately BEFORE the 128 byte ID3v1.
    // Checked before plain ID3v1 because "TAG" is a prefix of "TAG+" and would otherwise win.
    if (avail >= 227 && p[0] == 'T' && p[1] == 'A' && p[2] == 'G' && p[3] == '+')
        return 227;

    // Lyrics3, v1 and v2. It declares its length only at its END, so the length cannot be read
    // going forward the way every other format's can. What CAN be done is look for the terminator
    // it is required to carry, within what is already in the buffer, and take everything up to it.
    // The chain is confirmed against a real frame afterwards either way, so a wrong guess here
    // costs nothing but a fallback to the ordinary search.
    if (avail >= 11 && memcmp(p, "LYRICSBEGIN", 11) == 0)
    {
        // ** v2 IS WALKED, NOT SEARCHED, BECAUSE ITS FIELDS DECLARE THEIR OWN SIZES. **
        //
        // A bounded search stood here and it was wrong in both directions. The bound was 16 KB,
        // justified by "v2's fields cannot approach this figure", and that justification is simply
        // false: a v2 field carries a SIX character size, so one field alone may be 999,999 bytes.
        // Every tag past the bound was charged to the user as lost audio. Measured, on files whose
        // demuxed output is identical to the same file with no tag at all:
        //
        //     a 3 KB tag        silent, correct
        //     a 20 KB tag       "20052 bytes of the 5540872 read"
        //     a 900 KB tag      "900099 bytes of the 6420919 read"
        //
        // The format gives a better answer than any bound. A v2 block is "LYRICSBEGIN", then field
        // records of a three character field ID and FIVE characters of size followed by that many
        // bytes, then a SIX character size and "LYRICS200". So the fields can be STEPPED THROUGH
        // the way every other tag's length is read, and the terminator is recognised by its shape
        // rather than hunted for: six digits followed by "LYRICS200" cannot be a field record,
        // whose first three bytes are letters.
        //
        // ** THE FIELD SIZE IS FIVE CHARACTERS AND THE CLOSING SIZE IS SIX, WHICH IS NOT WHAT ONE
        // PUBLISHED DESCRIPTION OF THE FORMAT SAYS. ** It calls the field size six characters.
        // Real tags disagree with it, and the tags win: a 3,044 byte example reads
        // "IND" "00002" "11" then "LYR" "03000" and three thousand bytes, ending "003029"
        // "LYRICS200", where 3,044 - 15 = 3,029 and 11 + 10 + 3,008 = 3,029 exactly. Four
        // independent tags of different sizes all parse on the five character reading and none on
        // the six.
        //
        // Stepping through does NOT make the pathological input cheaper, and it was worth measuring
        // rather than assuming. The caller asks at EVERY byte of a region, and the ninth review
        // measured a 2 MB block full of the word "LYRICSBEGIN" driving that to a scan per byte.
        // Such a block leaves this walk at once, because "LYR" is three letters but "ICSBE" is not
        // five digits, and then meets the same bounded search as before. Measured on that input,
        // three runs each, minima: 1,311 ms before this change and 1,321 ms after, with identical
        // output. The walk buys correctness on large tags and costs nothing here.
        const auto isDigit = [](const uint8_t c) { return c >= '0' && c <= '9'; };
        const auto isUpper = [](const uint8_t c) { return c >= 'A' && c <= 'Z'; };
        int64_t at = 11;
        bool sawField = false;
        while (avail - at >= 15)
        {
            // The terminator: six digits, then the closing keyword.
            if (isDigit(p[at]) && isDigit(p[at + 1]) && isDigit(p[at + 2]) && isDigit(p[at + 3]) &&
                isDigit(p[at + 4]) && isDigit(p[at + 5]) && memcmp(p + at + 6, "LYRICS200", 9) == 0)
                return at + 15;
            // Otherwise a field record: three letters, five digits. Anything else is not v2, and
            // falls through to the v1 search below rather than being rejected outright.
            if (!isUpper(p[at]) || !isUpper(p[at + 1]) || !isUpper(p[at + 2]))
                break;
            bool sized = true;
            int64_t fsize = 0;
            for (int i = 3; i < 8; ++i)
            {
                if (!isDigit(p[at + i]))
                {
                    sized = false;
                    break;
                }
                fsize = fsize * 10 + (p[at + i] - '0');
            }
            if (!sized)
                break;
            at += 8 + fsize;
            sawField = true;
            if (at > avail)
                return 0;  // the tag runs past what has arrived, so no length can be stated
        }
        if (sawField)
            return 0;  // v2 fields parsed but the end has not arrived. Claiming a length here would
                       // be a guess, and the search below would be hunting a keyword inside field
                       // TEXT, where finding one proves nothing

        // Lyrics3 v1 has no field structure at all, only text and a closing keyword, so it is the
        // one thing here that must still be searched for. The bound stays where it was: a v2 tag
        // this walk cannot parse would otherwise be charged as lost, which is the defect being
        // fixed, so the old search is kept underneath as a safety net rather than narrowed.
        constexpr int64_t LYRICS3_MAX = 16384;
        const uint8_t* stop = end - p > LYRICS3_MAX ? p + LYRICS3_MAX : end;
        for (const uint8_t* q = p + 11; q + 9 <= stop; ++q)
        {
            if (memcmp(q, "LYRICS200", 9) == 0)
                return q + 9 - p;
            if (memcmp(q, "LYRICSEND", 9) == 0)
                return q + 9 - p;
        }
        return 0;  // no terminator within reach, so nothing is claimed
    }

    // ID3v1 is always exactly 128 bytes and always begins "TAG".
    //
    // ** THREE BYTES IS NOT ENOUGH EVIDENCE TO STEP OVER 128, AND THE YEAR FIELD IS FREE. ** With
    // "TAG" alone this was measured eating whole undamaged frames: an MP2 stream whose frames are
    // 96 bytes lost one entirely; a legal MLP frame header IS 0x54 0x41 0x47, meaning "length
    // 2178", and 18,926 bytes went with it; and AC-3 damage beginning "TAG" turned a recoverable
    // file into a destroyed one. The control in each case differed by a single bit, "UAG" or
    // "TAH", and behaved correctly, which is what proved the three bytes were the whole cause.
    //
    // The layout is fixed: 3 + 30 title + 30 artist + 30 album, then FOUR YEAR CHARACTERS at 93.
    // Requiring those to be digits, spaces or nul costs nothing and multiplies the evidence by
    // about eleven bits per character. "TAG" alone is one chance in 16 million; with the year it
    // is one in millions of millions.
    if (avail >= 128 && p[0] == 'T' && p[1] == 'A' && p[2] == 'G')
    {
        bool yearOk = true;
        for (int i = 93; i < 97; ++i)
            if (!(p[i] == 0 || p[i] == ' ' || (p[i] >= '0' && p[i] <= '9')))
                yearOk = false;
        if (yearOk)
            return 128;
    }

    return 0;
}

// How many of the bytes starting at p are metadata tags rather than data that was lost.
//
// ** THIS NEVER MOVES THE READ POSITION, AND THAT IS THE WHOLE POINT. **
//
// An earlier version of this change recognised a tag and then JUMPED over it by its declared
// length. Eight rounds of review found THIRTEEN separate ways for that to destroy real audio in
// silence, and every one of them had the same shape: a length was believed, the position moved,
// and whatever lay between the real end of the tag and the false one was gone. Measured, on the
// user's own music and on synthetic streams alike: 835,584 bytes from one flipped byte;
// 2,094,810 cut out of the middle of a file; 9,752,382, being 97 per cent of a stream, from a
// bound that had been raised to quiet a warning.
//
// ** THE AUDIO NEVER NEEDED IT. ** The frame search already steps over a tag on its own, because
// none of a tag looks like a frame, and commit 14638a4 made that search strict enough to prove
// it: measured over the whole corpus, an ID3v2 tag at the HEAD of an MP2, MP3, AAC or AC-3 file
// comes out EXACTLY right with none of this code present. What the search cannot do is know that
// what it stepped over was a TAG rather than lost data, so it reported an ordinary tagged file as
// damaged.
//
// So the recogniser survives, and its only job now is to answer that one question. Being wrong
// here costs a wrong number in a warning. It cannot cost a byte of audio, because nothing that
// follows depends on it.

// The chain of tags beginning EXACTLY at p, or 0. It is probed while m_curPos is still standing on
// the tag, because by the time the loss is charged the header is often already behind us: measured
// on a join of two tagged MP3s, the reader decodes a FALSE frame out of the tag's first 419 bytes,
// and the 360,570 that then get charged contain no header at all for a scan to find.
//
// ** THE WHOLE DECLARED LENGTH IS CREDITED, EVEN THE PART THAT HAS NOT ARRIVED. ** A tag bigger
// than one read block is charged across several buffers and only the first still has a header in
// it. Believing a declared length is what destroyed audio in eight rounds of review - but that was
// believing it enough to MOVE. This only ever subtracts from a number that is printed.
static int64_t tagRunAt(const uint8_t* p, const uint8_t* end)
{
    int64_t total = 0;
    for (;;)
    {
        const int64_t len = oneTagLength(p + total, end);
        if (len <= 0)
            return total;
        total += len;
        if (p + total >= end)
            return total;
    }
}

// The bytes inside [p, end) that belong to a metadata tag, found by SEARCHING the region.
//
// ** THIS AND tagRunAt ABOVE CATCH DIFFERENT TAGS AND BOTH ARE NEEDED. ** tagRunAt handles a tag
// whose header is already BEHIND the region being charged - the reader decodes a false frame out
// of a tag's first few hundred bytes and the header goes with it. This one handles a tag whose
// header sits INSIDE the region, which happens at a join seam where the reader arrives part way
// through the previous file's last frame.
//
// Removing this and keeping only the run made the empty tag seam case charge 531 where 521 is
// right - the ten byte tag went back on the bill. Keeping BOTH naively is what the ninth review
// caught as DOUBLE CREDIT, every tag subtracted twice. The caller therefore gives each of them a
// DISJOINT part of the region: the run covers the prefix, the scan covers whatever is left.
static int64_t tagBytesAt(const uint8_t* p, const uint8_t* end)
{
    int64_t total = 0;
    for (const uint8_t* q = p; q < end;)
    {
        // ** ONE COMPARE BEFORE THE EXPENSIVE QUESTION. ** This asks oneTagLength at EVERY byte of
        // the region, and that function runs several memcmp calls before it can say no. Every tag
        // it knows begins with one of four letters, so anything else is rejected for the price of
        // a single test. Measured on a 2 MB block engineered to be the worst case: 53.5 seconds
        // before the Lyrics3 bound, 6.1 after it, and this line is what closes the rest of the gap
        // to the unmodified reader's 0.16.
        const uint8_t c = *q;
        if (c != 'I' && c != 'A' && c != 'T' && c != 'L')
        {
            ++q;
            continue;
        }
        const int64_t len = oneTagLength(q, end);
        if (len <= 0)
        {
            ++q;
            continue;
        }
        const int64_t take = len < end - q ? len : end - q;
        total += take;
        q += take;
    }
    return total;
}

static constexpr double mplsEps = INTERNAL_PTS_FREQ / 45000.0 / 2.0;

void SimplePacketizerReader::doMplsCorrection()
{
    if (m_curMplsIndex == -1)
        return;
    if (m_curPts >= (m_lastMplsTime - mplsEps) && m_curMplsIndex < static_cast<int>(m_mplsInfo.size() - 1))
    {
        m_curMplsIndex++;
        if (m_mplsInfo[m_curMplsIndex].connection_condition == 5)
        {
            m_mplsOffset += m_curPts - m_lastMplsTime;
        }
        m_lastMplsTime +=
            (m_mplsInfo[m_curMplsIndex].OUT_time - m_mplsInfo[m_curMplsIndex].IN_time) * (INTERNAL_PTS_FREQ / 45000.0);
    }
}

void SimplePacketizerReader::setBuffer(uint8_t* data, const uint32_t dataLen, bool lastBlock)
{
    // ** COUNTED HERE, WHERE THE BYTES ARRIVE, BECAUSE EVERY OTHER PLACE MISSES SOME OF THEM. **
    // The loss report says "N bytes ... read for this track", and N used to be m_processedBytes,
    // which counts what the reader got THROUGH rather than what it was GIVEN. A tag at the end of
    // a file is stepped over on purpose and never added, so N came up short by exactly its length:
    //     X3, a 27,435,292 byte file whose last 1,900,000 bytes are a tag -> "of the 25535292"
    //     X4, the same file with junk instead of a tag                    -> "of the 27435292"
    // Two files of identical size, the same real loss, and two different denominators.
    //
    // Trying to add the missing bytes back at the point they are skipped means finding every such
    // point, and there are several: the tag skip, the park, and the flush that drops what the park
    // was holding. This is the one place all of them pass through, so it cannot miss any.
    //
    // getProcessedSize is deliberately LEFT ALONE. metaDemuxer.cpp uses it to drive the progress
    // display, where "how far have we got" is the right question and this is not the answer.
    m_readBytes += dataLen;

    if (static_cast<size_t>(m_tmpBufferLen + dataLen) > m_tmpBuffer.size())
        m_tmpBuffer.resize(m_tmpBufferLen + dataLen);

    if (!m_tmpBuffer.empty())
        memcpy(m_tmpBuffer.data() + m_tmpBufferLen, data + MAX_AV_PACKET_SIZE, dataLen);
    m_tmpBufferLen += dataLen;

    if (!m_tmpBuffer.empty())
        m_curPos = m_buffer = m_tmpBuffer.data();
    else
        m_curPos = m_buffer = nullptr;
    m_bufEnd = m_buffer + m_tmpBufferLen;
    m_tmpBufferLen = 0;
}

int64_t SimplePacketizerReader::getProcessedSize() { return m_processedBytes; }

int SimplePacketizerReader::flushPacket(AVPacket& avPacket)
{
    avPacket.duration = 0;
    avPacket.data = nullptr;
    avPacket.size = 0;
    avPacket.stream_index = m_streamIndex;
    avPacket.flags = m_flags + AVPacket::IS_COMPLETE_FRAME;
    avPacket.codecID = getCodecInfo().codecID;
    avPacket.codec = this;
    int skipBytes = 0;
    int skipBeforeBytes = 0;
    if (m_tmpBufferLen >= getHeaderLen())
    {
        const int size =
            decodeFrame(m_tmpBuffer.data(), m_tmpBuffer.data() + m_tmpBufferLen, skipBytes, skipBeforeBytes);
        if (size + skipBytes + skipBeforeBytes <= 0 && size != NOT_ENOUGH_BUFFER)
            return 0;
    }
    avPacket.dts = avPacket.pts = static_cast<int64_t>(m_curPts * m_stretch) + m_timeOffset;
    if (m_tmpBufferLen > 0)
    {
        avPacket.data = m_tmpBuffer.data();
        avPacket.data += skipBeforeBytes;
        avPacket.size = static_cast<int>(m_tmpBufferLen);
        if (isPriorityData(&avPacket))
            avPacket.flags |= AVPacket::PRIORITY_DATA;
        if (isIFrame(&avPacket))
            avPacket.flags |= AVPacket::IS_IFRAME;  // can be used in split points
    }
    LTRACE(LT_DEBUG, 0, "Processed " << m_frameNum << " " << getCodecInfo().displayName << " frames");
    m_processedBytes += avPacket.size + skipBytes + skipBeforeBytes;
    return static_cast<int>(m_tmpBufferLen);
}

int SimplePacketizerReader::readPacket(AVPacket& avPacket)
{
    do
    {
        avPacket.flags = m_flags + AVPacket::IS_COMPLETE_FRAME | AVPacket::FORCE_NEW_FRAME;
        avPacket.stream_index = m_streamIndex;
        avPacket.codecID = getCodecInfo().codecID;
        avPacket.codec = this;
        avPacket.data = nullptr;
        avPacket.size = 0;
        avPacket.duration = 0;
        avPacket.dts = avPacket.pts = static_cast<int64_t>(m_curPts * m_stretch) + m_timeOffset;
        assert(m_curPos <= m_bufEnd);
        if (m_curPos == m_bufEnd)
            return NEED_MORE_DATA;
        // ** NOTICED HERE, CHARGED LATER, AND THE POSITION IS NEVER MOVED. ** If a tag chain begins
        // exactly where we are standing, remember how long it is. The frame search will step over
        // it in its own time, and when the bill for that arrives this credit cancels it. Looking
        // only at the moment of charging is too late: the reader often decodes a false frame out of
        // the first few hundred bytes of a tag, so by then the header is behind us and there is
        // nothing left to recognise.
        // ** THE CREDIT IS A RUN OF BYTES, NOT AN AMOUNT, AND THIS IS THE THIRD ATTEMPT AT IT. **
        //
        // m_tagCredit holds how much of the chain we are standing on has NOT yet been accounted
        // for, and it is decremented by every byte the reader advances - whether those bytes are
        // emitted as a false frame, skipped one at a time, or charged as loss. When it reaches
        // zero the chain is spent and nothing later can draw on it.
        //
        // Attempt one credited an AMOUNT and let it stand. The ninth review measured what that
        // costs: a credit banked at one place cancelled a hole FIVE MEGABYTES later, and 400
        // ID3v1 tags scattered through a file excused 51,200 bytes of completely unrelated audio
        // damage - the report said 40,472 where the untagged control said 91,672.
        // Attempt two expired it when audio resumed, which fixed the distance but not the aim:
        // the credit was still spent on whatever bill arrived next, tag or not.
        // ** A CHAIN CAN ONLY EXCUSE THE BYTES IT IS ACTUALLY MADE OF. **
        if (m_tagCredit <= 0 && m_curPos != m_tagProbePos)
        {
            m_tagProbePos = m_curPos;
            m_tagCredit = tagRunAt(m_curPos, m_bufEnd);
        }
        int skipBytes = 0;
        int skipBeforeBytes = 0;
        if (m_needSync)
        {
            uint8_t* frame = findFrame(m_curPos, m_bufEnd);
            if (frame == nullptr)
            {
                m_processedBytes += m_bufEnd - m_curPos;
                // Nothing recognisable is left in this buffer. HELD BACK, not counted: if a frame
                // turns up later this really was a hole and it is promoted to lost; if none ever
                // does, these were the trailing bytes of the file and a file is entitled to carry
                // them. An identifier tag at the end of an audio file is the ordinary case, and
                // counting it made the report fire on files whose audio was byte perfect.
                //
                // KNOWN LIMIT, measured and deliberately left: whatever is still held when the
                // stream ends is never counted, so damage that runs to the end of the file is
                // silent AT ANY SIZE. 8,388,608 bytes of it were measured silent here, while the
                // same bytes moved into the middle of the same stream are reported exactly.
                if (m_everSynced)
                {
                    const int64_t region = m_bufEnd - m_curPos;
                    const int64_t paid = m_tagCredit < region ? m_tagCredit : region;
                    m_tagCredit -= paid;
                    const int64_t held = region - paid - tagBytesAt(m_curPos + paid, m_bufEnd);
                    m_pendingLost += held > 0 ? held : 0;
                }
                return NEED_MORE_DATA;
            }
            int decodeRez = decodeFrame(frame, m_bufEnd, skipBytes, skipBeforeBytes);
            if (decodeRez == NOT_ENOUGH_BUFFER)
            {
                if (m_bufEnd - frame > DEFAULT_FILE_BLOCK_SIZE)
                    THROW(ERR_COMMON,
                          getCodecInfo().displayName << " stream (track " << m_streamIndex << "): invalid stream.")
                memmove(m_tmpBuffer.data(), m_curPos, m_bufEnd - m_curPos);
                m_tmpBufferLen = m_bufEnd - m_curPos;
                m_curPos = m_bufEnd;
                return NEED_MORE_DATA;
            }
            if (decodeRez + skipBytes + skipBeforeBytes <= 0)
            {
                m_curPos++;
                m_processedBytes++;
                if (m_tagCredit > 0)
                    m_tagCredit--;  // that byte belonged to the chain, so it is not also a loss
                else if (m_everSynced)
                    m_lostBytes++;
                return 0;
            }
            m_processedBytes += frame - m_curPos;
            // The bytes stepped over to reach this frame. On the FIRST sync they are the leading
            // fragment of a stream that started mid frame, which is not a loss; on every later one
            // they are data that could not be parsed and is being abandoned.
            //
            // EXCEPT FOR THE PART OF THEM THAT IS A TAG, which is metadata and is not a loss
            // wherever it sits. Two ordinary files appended, or two named on one join line, put a
            // tag in the MIDDLE of the stream, and counting it reported a loss on a file whose
            // output was byte identical to the same file without the tags. Measured on AAC, AC-3,
            // MPEG audio Layer II and Layer III, and on the user's own music, where a join of two
            // tagged MP3s announced 4,239 bytes dropped on a byte perfect result.
            //
            // ** THE SEARCH HAS ALREADY DONE THE STEPPING OVER. ** All that happens here is that
            // the tag's share of it is not charged to the user. Nothing below depends on this
            // number, so the worst a mistake can do is print a wrong figure.
            if (m_everSynced)
            {
                const int64_t region = frame - m_curPos;
                const int64_t paid = m_tagCredit < region ? m_tagCredit : region;
                m_tagCredit -= paid;
                const int64_t bill = region - paid - tagBytesAt(m_curPos + paid, frame);
                m_lostBytes += bill > 0 ? bill : 0;
            }
            else
                m_everSynced = true;
            // A frame HAS turned up, so anything held back earlier was a hole in the middle of the
            // stream after all, not its tail. Promote it. Whatever is still held when the stream
            // ends is never promoted, and that is exactly the trailing data.
            m_lostBytes += m_pendingLost;
            m_pendingLost = 0;
            LTRACE(LT_INFO, 2,
                   "Decoding " << getCodecInfo().displayName << " stream (track " << m_streamIndex
                               << "): " << getStreamInfo());
            m_curPos = frame;
            m_needSync = false;
        }
        avPacket.dts = avPacket.pts = static_cast<int64_t>(m_curPts * m_stretch) + m_timeOffset;
        if (m_bufEnd - m_curPos < getHeaderLen())
        {
            memmove(m_tmpBuffer.data(), m_curPos, m_bufEnd - m_curPos);
            m_tmpBufferLen = m_bufEnd - m_curPos;
            m_curPos = m_bufEnd;
            return NEED_MORE_DATA;
        }
        skipBytes = 0;
        skipBeforeBytes = 0;
        int frameLen = decodeFrame(m_curPos, m_bufEnd, skipBytes, skipBeforeBytes);
        if (frameLen == NOT_ENOUGH_BUFFER)
        {
            if (m_bufEnd - m_curPos > DEFAULT_FILE_BLOCK_SIZE)
                THROW(ERR_COMMON,
                      getCodecInfo().displayName << " stream (track " << m_streamIndex << "): invalid stream.")
            memmove(m_tmpBuffer.data(), m_curPos, m_bufEnd - m_curPos);
            m_tmpBufferLen = m_bufEnd - m_curPos;
            m_curPos = m_bufEnd;
            return NEED_MORE_DATA;
        }
        if (frameLen + skipBytes + skipBeforeBytes <= 0)
        {
            LTRACE(LT_INFO, 2,
                   getCodecInfo().displayName
                       << " stream (track " << m_streamIndex << "): bad frame detected at position"
                       << floatToTime((double)(avPacket.pts - PTS_CONST_OFFSET) / INTERNAL_PTS_FREQ, ',')
                       << ". Resync stream.");
            m_needSync = true;
            return 0;
        }
        if (m_bufEnd - m_curPos < frameLen + skipBytes + skipBeforeBytes)
        {
            memmove(m_tmpBuffer.data(), m_curPos, m_bufEnd - m_curPos);
            m_tmpBufferLen = m_bufEnd - m_curPos;
            m_curPos = m_bufEnd;
            return NEED_MORE_DATA;
        }
        avPacket.data = m_curPos;
        avPacket.data += skipBeforeBytes;
        if (frameLen > getMaxFrameSize())
            THROW(ERR_AV_FRAME_TOO_LARGE, "AV frame too large (" << frameLen << " bytes). Increase AV buffer.")
        avPacket.size = frameLen;
        if (isPriorityData(&avPacket))
            avPacket.flags |= AVPacket::PRIORITY_DATA;
        if (isIFrame(&avPacket))
            avPacket.flags |= AVPacket::IS_IFRAME;  // can be used in split points

        if (m_halfFrameLen == 0.0)
            m_halfFrameLen = getFrameDuration() / 2.0;
        m_curPts += getFrameDuration();
        int64_t nextDts = static_cast<int64_t>(m_curPts * m_stretch) + m_timeOffset;
        avPacket.duration = nextDts - avPacket.dts;
        // doMplsCorrection();
        m_frameNum++;
        if (m_frameNum % 1000 == 0)
            LTRACE(LT_DEBUG, 0, "Processed " << m_frameNum << " " << getCodecInfo().displayName << " frames");
        m_curPos += frameLen + skipBytes + skipBeforeBytes;
        m_processedBytes += frameLen + skipBytes + skipBeforeBytes;
        // These bytes came out of the chain too, if we are inside one. A false frame decoded from
        // the first few hundred bytes of a tag is the ordinary case, and if it were not counted
        // here the same bytes would be excused twice.
        m_tagCredit -= frameLen + skipBytes + skipBeforeBytes;
        if (m_tagCredit < 0)
            m_tagCredit = 0;

        if (needMPLSCorrection())
        {
            if (/*m_demuxMode && */ m_mplsOffset > m_halfFrameLen)
            {  // overlap frame detected. skip frame
                if (avPacket.duration)
                    LTRACE(LT_INFO, 2,
                           getCodecInfo().displayName
                               << " stream (track " << m_streamIndex << "): overlapped frame detected at position "
                               << floatToTime((double)(avPacket.pts - PTS_CONST_OFFSET) / INTERNAL_PTS_FREQ, ',')
                               << ". Remove frame.");
                m_mplsOffset -= getFrameDuration();
                m_curPts -= getFrameDuration();
                return readPacket(avPacket);  // ignore overlapped packet, get next one
            }
            doMplsCorrection();
        }

        if (needSkipFrame(avPacket))
            continue;

        return 0;

    } while (true);
}

StreamDiscoveryData SimplePacketizerReader::probeStream(uint8_t* buffer, const int len,
                                                        const ContainerType containerType, const int containerDataType,
                                                        const int containerStreamIndex)
{
    StreamDiscoveryData data;
    const CheckStreamRez rez = checkStream(buffer, len, containerType, containerDataType, containerStreamIndex);
    if (!rez.codecInfo.codecID)
        return data;  // detection failed – return empty data

    data.discovered = true;
    data.codecName = rez.codecInfo.programName;
    data.streamDescr = rez.streamDescr;

    // Let the concrete reader fill in codec-specific fields.
    fillDiscoveryData(data);

    return data;
}

void SimplePacketizerReader::fillDiscoveryData(StreamDiscoveryData& data)
{
    // Base implementation fills common audio fields.
    data.sampleRate = getFreq();
    data.channels = getChannels();
}

static constexpr int CHECK_FRAMES_COUNT = 10;

CheckStreamRez SimplePacketizerReader::checkStream(uint8_t* buffer, const int len, const ContainerType containerType,
                                                   const int containerDataType, const int containerStreamIndex)
{
    m_containerType = containerType;
    m_containerDataType = containerDataType;
    m_containerStreamIndex = containerStreamIndex;
    setTestMode(true);

    CheckStreamRez rez;
    uint8_t* end = buffer + len;
    uint8_t* frame = findFrame(buffer, end);
    if (frame == nullptr)
    {
        setTestMode(false);
        return rez;
    }
    int skipBytes = 0;
    int skipBeforeBytes = 0;
    for (int i = 0; i < 5; ++i)
    {
        if (decodeFrame(frame, end, skipBytes, skipBeforeBytes) <= 0)
        {
            // setTestMode(false);
            // return rez;
            frame = findFrame(frame + 2, end);

            if (frame == nullptr)
            {
                setTestMode(false);
                return rez;
            }
        }
        else
            break;
    }
    const int freq = getFreq();
    bool firstStep = true;
    for (int i = 0; i < CHECK_FRAMES_COUNT && frame < end;)
    {
        const int frameLen = decodeFrame(frame, end, skipBytes, skipBeforeBytes);
        if (frameLen <= 0 || getFreq() != freq || (firstStep && frameLen > end - frame))
        {
            setTestMode(false);
            return rez;
        }
        firstStep = false;
        frame += frameLen + skipBytes + skipBeforeBytes;
        if (getFrameDuration() > 0)
            i++;
    }
    setTestMode(false);
    rez.codecInfo = getCodecInfo();
    rez.streamDescr = getStreamInfo();
    return rez;
}
