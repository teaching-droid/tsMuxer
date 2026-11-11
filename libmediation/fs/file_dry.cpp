// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
//
// ** A RUN THAT WRITES NOTHING. ** How large an output will be, and whether it fits its disc, was known only when it
// was written: hours for a film, and the room for it on a drive. In a dry run every file opened for writing is one that
// only counts: what is written to it moves its position and its size and goes nowhere, a seek and a later write act
// as on a file, and a read gives zeros. Nothing is created on the drive, no folder either (createDir), and nothing is
// deleted (deleteFile). The run is the mux itself, so the size is the one the mux gives, and what it warns of is said
// as in a real run. The sizes of the files so "written" are kept by name, for the summary of the scan (main.cpp).
#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>

#include "file.h"

namespace
{
std::mutex g_lock;
bool g_enabled = false;
std::map<std::string, int64_t> g_sizes;  // every file "written", by name

std::string keyOf(const char* name)
{
    std::string key = name ? name : "";
    std::replace(key.begin(), key.end(), '\\', '/');
    return key;
}
}  // namespace

namespace dryRun
{
void enable()
{
    const std::lock_guard<std::mutex> lock(g_lock);
    g_enabled = true;
}

bool enabled() { return g_enabled; }

std::vector<std::pair<std::string, int64_t>> files()
{
    const std::lock_guard<std::mutex> lock(g_lock);
    return {g_sizes.begin(), g_sizes.end()};
}

bool has(const std::string& name)
{
    const std::lock_guard<std::mutex> lock(g_lock);
    return g_sizes.find(keyOf(name.c_str())) != g_sizes.end();
}

int64_t sizeOf(const std::string& name)
{
    const std::lock_guard<std::mutex> lock(g_lock);
    const auto found = g_sizes.find(keyOf(name.c_str()));
    return found == g_sizes.end() ? -1 : found->second;
}

void forget(const std::string& name)
{
    const std::lock_guard<std::mutex> lock(g_lock);
    g_sizes.erase(keyOf(name.c_str()));
}

bool rename(const std::string& from, const std::string& to)
{
    const std::lock_guard<std::mutex> lock(g_lock);
    const auto found = g_sizes.find(keyOf(from.c_str()));
    if (found == g_sizes.end())
        return false;
    const int64_t size = found->second;
    g_sizes.erase(found);
    g_sizes[keyOf(to.c_str())] = size;
    return true;
}
}  // namespace dryRun

struct File::DryView
{
    std::string key;
    int64_t size = 0;
    bool readOnly = false;  // opened to read: its size is not written back
};

// True when the file is one of a dry run: opened for writing while the dry run is on
bool File::openDry(const char* fName, const unsigned int oflag)
{
    if (!g_enabled || (oflag & ofForReal))
        return false;
    if (!(oflag & ofWrite))
    {
        // read back: only where the caller asks for it, and only a file the run "wrote"
        const int64_t size = (oflag & ofDryRead) ? dryRun::sizeOf(fName) : -1;
        if (size < 0)
            return false;
        closeDry();
        auto* view = new DryView;
        view->key = keyOf(fName);
        view->size = size;
        view->readOnly = true;
        m_dry = view;
        m_pos = 0;
        return true;
    }
    closeDry();
    auto* view = new DryView;
    view->key = keyOf(fName);
    {
        const std::lock_guard<std::mutex> lock(g_lock);
        const auto known = g_sizes.find(view->key);
        // a file written before in this run keeps what it holds where the real one would
        if (known != g_sizes.end() && (oflag & (ofAppend | ofNoTruncate | ofOpenExisting)))
            view->size = known->second;
        g_sizes[view->key] = view->size;
    }
    m_dry = view;
    m_pos = (oflag & ofAppend) ? view->size : 0;
    return true;
}

void File::closeDry()
{
    if (!m_dry)
        return;
    if (!m_dry->readOnly)
    {
        const std::lock_guard<std::mutex> lock(g_lock);
        g_sizes[m_dry->key] = m_dry->size;
    }
    delete m_dry;
    m_dry = nullptr;
}

int File::writeDry(const uint32_t count)
{
    if (m_dry->readOnly)
        return -1;
    m_pos += count;
    m_dry->size = (std::max)(m_dry->size, m_pos);
    return static_cast<int>(count);
}

int File::readDry(void* buffer, const uint32_t count) const
{
    const int64_t left = m_dry->size - m_pos;
    const uint32_t got = left <= 0 ? 0 : static_cast<uint32_t>((std::min)(static_cast<int64_t>(count), left));
    memset(buffer, 0, got);
    m_pos += got;
    return static_cast<int>(got);
}

int64_t File::seekDry(const int64_t offset, const SeekMethod whence) const
{
    int64_t to = offset;
    if (whence == SeekMethod::smCurrent)
        to += m_pos;
    else if (whence == SeekMethod::smEnd)
        to += m_dry->size;
    if (to < 0)
        return -1;
    m_pos = to;
    return m_pos;
}

int64_t File::sizeDry() const { return m_dry->size; }

bool File::truncateDry(const uint64_t newFileSize) const
{
    m_dry->size = static_cast<int64_t>(newFileSize);
    return true;
}
