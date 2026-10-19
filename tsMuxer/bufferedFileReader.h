#ifndef BUFFERED_FILE_READER_H_
#define BUFFERED_FILE_READER_H_

#include <fs/file.h>
#include <types/types.h>

#include "bufferedReader.h"
#include "tsPacket.h"
#include "vodCoreException.h"

// A part of a play timeline (a Matroska ordered edition): the stretch of its file to play, in the file's own
// nanoseconds, and where the part starts in the timeline. startNs -1: the whole file, as a plain join plays it.
struct JoinRange
{
    int64_t startNs = -1;
    int64_t endNs = INT64_MAX;
    int64_t offsetNs = 0;
    std::string name;  // the chapter's
};

// Used to automatically switch to reading the next file in a given list
class FileListIterator final : public FileNameIterator
{
   public:
    FileListIterator() : m_index(0) {}
    ~FileListIterator() override = default;

    std::string getNextName() override { return ++m_index < m_files.size() ? m_files[m_index] : ""; }

    void addFile(const std::string& fileName) { m_files.push_back(fileName); }
    void addFile(const std::string& fileName, const JoinRange& range)
    {
        m_files.push_back(fileName);
        m_ranges.push_back(range);
    }
    [[nodiscard]] const std::vector<std::string>& getFiles() const { return m_files; }
    // One for each file of a play timeline; empty for a plain join.
    [[nodiscard]] const std::vector<JoinRange>& getRanges() const { return m_ranges; }

    // A clip of a Blu-ray playlist of which only the part its play item plays is read
    void addFile(const std::string& fileName, const ClipPart& part)
    {
        m_parts.resize(m_files.size());
        m_files.push_back(fileName);
        m_parts.push_back(part);
    }
    // One for each file when any of them is cut to a part; empty otherwise.
    [[nodiscard]] const std::vector<ClipPart>& getClipParts() const { return m_parts; }
    bool getByteRange(int64_t& first, int64_t& end) const override
    {
        if (m_index >= m_parts.size() || !m_parts[m_index].isCut())
            return false;
        first = m_parts[m_index].firstByte;
        end = m_parts[m_index].endByte;
        return true;
    }

   private:
    std::vector<std::string> m_files;
    std::vector<JoinRange> m_ranges;
    std::vector<ClipPart> m_parts;
    size_t m_index;
};

struct FileReaderData final : ReaderData
{
    typedef ReaderData base_class;

    FileReaderData(uint32_t blockSize, uint32_t allocSize) : m_fileHeaderSize(0) {}

    ~FileReaderData() override = default;

    int readBlock(uint8_t* buffer, uint32_t max_size) override
    {
        if (m_readEnd >= 0)
        {
            if (m_readPos >= m_readEnd)
                return 0;
            if (m_readEnd - m_readPos < max_size)
                max_size = static_cast<uint32_t>(m_readEnd - m_readPos);
        }
        const int read = m_file.read(buffer, max_size);
        if (read > 0)
            m_readPos += read;
        return read;
    }

    bool openStream() override;
    bool closeStream() override { return m_file.close(); }
    bool incSeek(const int64_t offset) override
    {
        m_readPos += offset;
        return m_file.seek(offset, File::SeekMethod::smCurrent) != -1;
    }

    File m_file;
    uint32_t m_fileHeaderSize;
    int64_t m_readPos = 0;   // where the next read starts
    int64_t m_readEnd = -1;  // where the part of the file that is read ends; -1: at the end of the file
};

class BufferedFileReader final : public BufferedReader
{
   public:
    BufferedFileReader(uint32_t blockSize, uint32_t allocSize = 0, uint32_t prereadThreshold = 0);

    bool openStream(int readerID, const char* streamName, int pid = 0, const CodecInfo* codecInfo = nullptr) override;
    bool gotoByte(int readerID, int64_t seekDist) override;

   protected:
    ReaderData* intCreateReader() override { return new FileReaderData(m_blockSize, m_allocSize); }
};

#endif
