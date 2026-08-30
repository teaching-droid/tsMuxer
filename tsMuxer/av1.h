#ifndef AV1_H_
#define AV1_H_

#include <cstdint>
#include <vector>

#include "bitStream.h"

// AV1 OBU type codes (AV1 spec Section 6.2.2)
enum class Av1ObuType
{
    RESERVED_0 = 0,
    SEQUENCE_HEADER = 1,
    TEMPORAL_DELIMITER = 2,
    FRAME_HEADER = 3,
    TILE_GROUP = 4,
    METADATA = 5,
    FRAME = 6,
    REDUNDANT_FRAME_HEADER = 7,
    TILE_LIST = 8,
    PADDING = 15
};

// AV1 frame types (AV1 spec Section 6.8.2)
enum class Av1FrameType
{
    KEY_FRAME = 0,
    INTER_FRAME = 1,
    INTRA_ONLY_FRAME = 2,
    SWITCH_FRAME = 3
};

// AV1 color primaries (AV1 spec Section 6.4.2)
static constexpr uint8_t AV1_CP_BT_709 = 1;
static constexpr uint8_t AV1_CP_BT_601 = 6;
static constexpr uint8_t AV1_CP_BT_2020 = 9;

// AV1 transfer characteristics
static constexpr uint8_t AV1_TC_SRGB = 13;
static constexpr uint8_t AV1_TC_PQ = 16;
static constexpr uint8_t AV1_TC_HLG = 18;

// AV1 matrix coefficients
static constexpr uint8_t AV1_MC_IDENTITY = 0;

// Decode a LEB128 variable-length integer from buffer. Returns the decoded
// value and advances *pos past the consumed bytes. Returns -1 on error.
int64_t decodeLeb128(const uint8_t* buf, const uint8_t* end, int& bytesRead);

// Encode a value as LEB128 into dst. Returns number of bytes written.
int encodeLeb128(uint8_t* dst, uint64_t value);

struct Av1ObuHeader
{
    Av1ObuType obu_type;
    bool obu_extension_flag;
    bool obu_has_size_field;
    uint8_t temporal_id;
    uint8_t spatial_id;

    // Parse OBU header from buffer. Returns number of header bytes consumed, or -1 on error.
    int parse(const uint8_t* buf, const uint8_t* end);
};

struct Av1SequenceHeader
{
    Av1SequenceHeader();

    int deserialize(const uint8_t* buf, int size);

    uint8_t seq_profile;
    bool still_picture;
    bool reduced_still_picture_header;
    uint8_t seq_level_idx_0;
    uint8_t seq_tier_0;

    // timing info
    bool timing_info_present_flag;
    uint32_t num_units_in_display_tick;
    uint32_t time_scale;
    bool equal_picture_interval;
    uint32_t num_ticks_per_picture_minus_1;

    // frame size
    unsigned max_frame_width;
    unsigned max_frame_height;

    // color config
    bool high_bitdepth;
    bool twelve_bit;
    bool mono_chrome;
    uint8_t color_primaries;
    uint8_t transfer_characteristics;
    uint8_t matrix_coefficients;
    bool color_range;
    uint8_t chroma_subsampling_x;
    uint8_t chroma_subsampling_y;
    uint8_t chroma_sample_position;
    bool separate_uv_delta_q;

    bool frame_id_numbers_present_flag;

    [[nodiscard]] int getBitDepth() const;
    [[nodiscard]] double getFPS() const;

   private:
    bool parseColorConfig(BitStreamReader& reader);
    bool parseTimingInfo(BitStreamReader& reader);
    static uint32_t parseUvlc(BitStreamReader& reader);
};

struct Av1FrameHeader
{
    Av1FrameHeader();

    // Parse just enough of the frame header to determine frame type.
    // Requires the sequence header to know reduced_still_picture_header.
    int deserialize(const uint8_t* buf, int size, const Av1SequenceHeader& seqHdr);

    bool show_existing_frame;
    Av1FrameType frame_type;
    bool show_frame;
    uint8_t frame_to_show_map_idx;
};

// Parse AV1CodecConfigurationRecord (from MP4/MKV codec private data) and
// extract config OBUs as start-code-prefixed buffers suitable for the
// TS start-code format.
std::vector<std::vector<uint8_t>> av1_extract_priv_data(const uint8_t* buff, int size);

// Add AV1 TS emulation prevention bytes: escape 0x000000/0x000001/0x000002
// sequences by inserting 0x03 bytes. Uses the same algorithm as H.264 Annex-B.
// Returns encoded size, or -1 on error.
int av1_add_emulation_prevention(const uint8_t* src, const uint8_t* srcEnd, uint8_t* dst, size_t dstSize);

// Remove AV1 TS emulation prevention bytes. Returns decoded size, or -1 on error.
int av1_remove_emulation_prevention(const uint8_t* src, const uint8_t* srcEnd, uint8_t* dst, size_t dstSize);

/*
 * Turn tsMuxeR's internal AV1 framing back into what an AV1 file on disk actually holds.
 *
 * Internally, and inside a transport stream, an OBU is written the way H.264 and HEVC write a NAL:
 * a start code, the OBU header with obu_has_size_field CLEARED, and an emulation prevented payload.
 * On disk there are no start codes at all. The AV1 specification's low overhead bitstream format is
 * a bare chain of OBUs, each carrying obu_has_size_field and its own leb128 length, and that is what
 * every encoder writes and every other program reads.
 *
 * Unlike MatroskaMuxer::convertAV1ToLowOverhead, which does the same conversion for Matroska, this
 * keeps EVERY OBU: Matroska strips temporal delimiters and deduplicates sequence headers because its
 * own rules say to, and a standalone .obu file needs them both.
 *
 * Returns the number of bytes appended to out, or -1 if the input cannot be walked.
 */
int av1_start_codes_to_low_overhead(const uint8_t* src, const uint8_t* end, std::vector<uint8_t>& out);

#endif  // AV1_H_
