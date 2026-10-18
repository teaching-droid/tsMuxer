#include "wave.h"

#include <cstring>

#include "vodCoreException.h"

namespace wave_format
{
uint32_t getWaveChannelMask(const int channels, const bool lfeExists)
{
    switch (channels)
    {
    case 1:
        return SPEAKER_FRONT_CENTER;
    case 2:
        return SPEAKER_FRONT_LEFT + SPEAKER_FRONT_RIGHT;
    case 3:
    {
        if (lfeExists)
            return SPEAKER_FRONT_LEFT + SPEAKER_FRONT_RIGHT + SPEAKER_LOW_FREQUENCY;
        return SPEAKER_FRONT_LEFT + SPEAKER_FRONT_RIGHT + SPEAKER_FRONT_CENTER;
    }
    case 4:
    {
        if (lfeExists)
            return SPEAKER_FRONT_LEFT + SPEAKER_FRONT_RIGHT + SPEAKER_FRONT_CENTER + SPEAKER_LOW_FREQUENCY;
        return SPEAKER_FRONT_LEFT + SPEAKER_FRONT_RIGHT + SPEAKER_BACK_LEFT + SPEAKER_BACK_RIGHT;
    }
    case 5:
        return SPEAKER_FRONT_LEFT + SPEAKER_FRONT_RIGHT + SPEAKER_FRONT_CENTER + SPEAKER_SIDE_LEFT + SPEAKER_SIDE_RIGHT;
    case 6:
        return SPEAKER_FRONT_LEFT + SPEAKER_FRONT_RIGHT + SPEAKER_FRONT_CENTER + SPEAKER_SIDE_LEFT +
               SPEAKER_SIDE_RIGHT + SPEAKER_LOW_FREQUENCY;
    case 7:
        return SPEAKER_FRONT_LEFT + SPEAKER_FRONT_RIGHT + SPEAKER_FRONT_CENTER + SPEAKER_BACK_LEFT +
               SPEAKER_BACK_RIGHT + SPEAKER_SIDE_LEFT + SPEAKER_SIDE_RIGHT;
    case 8:
        return SPEAKER_FRONT_LEFT + SPEAKER_FRONT_RIGHT + SPEAKER_FRONT_CENTER + SPEAKER_BACK_LEFT +
               SPEAKER_BACK_RIGHT + SPEAKER_SIDE_LEFT + SPEAKER_SIDE_RIGHT + SPEAKER_LOW_FREQUENCY;
    default:;
    }
    return 0;  // unknown value
}

void buildWaveHeader(MemoryBlock& waveBuffer, const int samplerate, const uint16_t channels, const bool lfeExist,
                     const uint16_t bitdepth)
{
    waveBuffer.clear();
    waveBuffer.grow(40 + 28);
    uint8_t* curPos = waveBuffer.data();
    for (const char c : "RIFF\x00\x00\x00\x00WAVEfmt ") *curPos++ = c;
    curPos--;
    const auto fmtSize = UnalignedPtr<uint32_t>(curPos);
    *fmtSize = static_cast<uint32_t>(sizeof(WAVEFORMATPCMEX));
    curPos += 4;
    const auto waveFormatPCMEx = reinterpret_cast<WAVEFORMATPCMEX*>(curPos);

    waveFormatPCMEx->wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    waveFormatPCMEx->nChannels = channels;
    waveFormatPCMEx->nSamplesPerSec = samplerate;
    waveFormatPCMEx->nAvgBytesPerSec = channels * samplerate * ((bitdepth + 4) >> 3);
    waveFormatPCMEx->nBlockAlign = channels * waveFormatPCMEx->wBitsPerSample / 8;
    waveFormatPCMEx->wBitsPerSample = bitdepth == 20 ? 24 : bitdepth;
    waveFormatPCMEx->cbSize = 22;  // After this to GUID
    waveFormatPCMEx->Samples.wValidBitsPerSample = bitdepth;
    waveFormatPCMEx->dwChannelMask = getWaveChannelMask(channels, lfeExist);  // Specify PCM
    waveFormatPCMEx->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;

    curPos += sizeof(WAVEFORMATPCMEX);
    for (const char c : "data\x00\x00\x00\x00") *curPos++ = c;
}

// Only whole samples are turned round. A block that ends inside a sample, as the first block read to probe a track can,
// was read and written up to two bytes past its end; the bytes of such a part sample are now copied as they are.
// 32 bit samples are turned round as 4 bytes: they went through the 3 byte path, which mixed bytes of two samples.
void toLittleEndian(uint8_t* dstData, uint8_t* srcData, const int size, const int bitdepth)
{
    if (size <= 0)
        return;
    if (bitdepth == 16)
    {
        const int whole = size / 2 * 2;
        auto dst = UnalignedPtr<uint16_t>(dstData);
        auto src = UnalignedPtr<uint16_t>(srcData);
        const auto srcEnd = UnalignedPtr<uint16_t>(srcData + whole);
        while (src < srcEnd) *dst++ = my_ntohs(*src++);
        if (whole < size)
            memmove(dstData + whole, srcData + whole, static_cast<size_t>(size - whole));
    }
    else if (bitdepth > 16)
    {
        const int width = bitdepth == 32 ? 4 : 3;
        const int whole = size / width * width;
        uint8_t* dst = dstData;
        const uint8_t* src = srcData;
        const uint8_t* srcEnd = srcData + whole;
        while (src < srcEnd)
        {
            if (width == 4)
            {
                const uint8_t b0 = src[0], b1 = src[1];
                dst[0] = src[3];
                dst[1] = src[2];
                dst[2] = b1;
                dst[3] = b0;
            }
            else
            {
                const uint8_t tmp = src[0];
                dst[0] = src[2];
                dst[1] = src[1];
                dst[2] = tmp;
            }
            dst += width;
            src += width;
        }
        if (whole < size)
            memmove(dstData + whole, srcData + whole, static_cast<size_t>(size - whole));
    }
    else
    {
        THROW(ERR_WAV_PARSE, "Unsupported LPCM big depth " << bitdepth << " for /LIT codec")
    }
}

}  // namespace wave_format
