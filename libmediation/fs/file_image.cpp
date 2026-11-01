#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "discImage.h"
#include "file.h"

struct File::ImageView
{
    discImage::FileView file;
    std::vector<uint64_t> starts;  // where each part begins in the file
    File image;                    // the file of the part read last, opened for this file alone so that readers do not
                                   // share a position: the image file, or a file a virtual file is made of
    mutable int imageIndex = -1;   // which of file.files it is
    mutable int64_t imagePos = -1;
};

// Only read: an output cannot go into an image, and a path into one is no place to make folders in either.
bool File::openInImage(const char* fName, const unsigned int oflag)
{
    if (oflag & (ofWrite | ofAppend | ofCreateNew | ofNoTruncate))
        throw std::runtime_error(std::string("Can't write ") + fName + ": a file inside a disc image is only read");
    auto* view = new ImageView;
    try
    {
        if (!discImage::openFile(fName, &view->file))
        {
            delete view;
            return false;
        }
    }
    catch (...)
    {
        delete view;
        throw;
    }
    uint64_t start = 0;
    for (const auto& e : view->file.extents)
    {
        view->starts.push_back(start);
        start += e.length;
    }
    m_view = view;
    m_pos = 0;
    return true;
}

void File::closeInImage()
{
    delete m_view;
    m_view = nullptr;
}

int File::readInImage(void* buffer, const uint32_t count) const
{
    const auto& file = m_view->file;
    if (m_pos < 0 || static_cast<uint64_t>(m_pos) >= file.size)
        return 0;
    const auto want = static_cast<uint32_t>((std::min)(static_cast<uint64_t>(count), file.size - m_pos));
    auto* out = static_cast<uint8_t*>(buffer);
    if (file.inEntry)
    {
        memcpy(out, file.data->data() + m_pos, want);
        m_pos += want;
        return static_cast<int>(want);
    }
    uint32_t done = 0;
    while (done < want)
    {
        const auto pos = static_cast<uint64_t>(m_pos);
        const size_t i =
            std::upper_bound(m_view->starts.begin(), m_view->starts.end(), pos) - m_view->starts.begin() - 1;
        const discImage::Extent& e = file.extents[i];
        const uint64_t within = pos - m_view->starts[i];
        auto n = static_cast<uint32_t>((std::min)(static_cast<uint64_t>(want - done), e.length - within));
        if (e.hole)
            memset(out + done, 0, n);
        else
        {
            const auto at = static_cast<int64_t>(e.offset + within);
            if (m_view->imageIndex != e.file)
            {
                if (m_view->image.isOpen())
                    m_view->image.close();
                m_view->imagePos = -1;
                m_view->imageIndex = -1;
                if (e.file < 0 || static_cast<size_t>(e.file) >= file.files.size() ||
                    !m_view->image.open(file.files[static_cast<size_t>(e.file)].c_str(), ofRead | ofOpenExisting))
                    return done ? static_cast<int>(done) : -1;
                m_view->imageIndex = e.file;
            }
            if (m_view->imagePos != at && m_view->image.seek(at) != at)
                return done ? static_cast<int>(done) : -1;
            const int r = m_view->image.read(out + done, n);
            if (r <= 0)
            {
                m_view->imagePos = -1;
                return done ? static_cast<int>(done) : -1;  // the image file ends too early: cut or damaged
            }
            n = static_cast<uint32_t>(r);
            m_view->imagePos = at + r;
        }
        done += n;
        m_pos += n;
    }
    return static_cast<int>(done);
}

int64_t File::seekInImage(const int64_t offset, const SeekMethod whence) const
{
    int64_t base = 0;
    if (whence == SeekMethod::smCurrent)
        base = m_pos;
    else if (whence == SeekMethod::smEnd)
        base = static_cast<int64_t>(m_view->file.size);
    if (base + offset < 0)
        return -1;
    m_pos = base + offset;
    return m_pos;
}

int64_t File::sizeInImage() const { return static_cast<int64_t>(m_view->file.size); }
