#include "platform/storage/AssetPak.h"

#include "platform/Log.h"
#include "platform/PlatformConfig.h"
#include "platform/storage/PakArchive.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <streambuf>

#if PLATFORM_PS2 || PLATFORM_WII
#include <malloc.h>
#endif

namespace
{
const char kScheme[] = "pak://";
const std::size_t kSchemeLength = sizeof(kScheme) - 1;
const char kArchiveName[] = "assets.pak";

PakArchive &archive()
{
    static PakArchive s_archive;
    return s_archive;
}

// Last path a mount was refused for, so a caller that retries every lookup
// (the PS2 locator re-probing its candidates) logs each candidate once.
std::string s_lastRefusedPath;

unsigned char *allocateAligned(std::size_t bytes)
{
#if PLATFORM_PS2 || PLATFORM_WII
    // Same alignment the loose-file loaders hand out: PS2 DMA sources and the
    // Wii cache line both want 64.
    return static_cast<unsigned char *>(memalign(64, bytes));
#else
    return static_cast<unsigned char *>(std::malloc(bytes));
#endif
}

// istream over a buffer this object owns.
class MemoryStream : public std::istream
{
public:
    explicit MemoryStream(std::vector<char> &&bytes)
        : std::istream(nullptr), bytes_(std::move(bytes)), buffer_(bytes_)
    {
        rdbuf(&buffer_);
    }

private:
    class Buffer : public std::streambuf
    {
    public:
        explicit Buffer(std::vector<char> &bytes)
        {
            char *begin = bytes.empty() ? nullptr : bytes.data();
            setg(begin, begin, begin + bytes.size());
        }

    protected:
        pos_type seekoff(off_type offset, std::ios_base::seekdir dir, std::ios_base::openmode) override
        {
            const off_type size = egptr() - eback();
            off_type base = 0;
            if (dir == std::ios_base::cur)
                base = gptr() - eback();
            else if (dir == std::ios_base::end)
                base = size;
            const off_type target = base + offset;
            if (target < 0 || target > size)
                return pos_type(off_type(-1));
            setg(eback(), eback() + target, egptr());
            return pos_type(target);
        }

        pos_type seekpos(pos_type position, std::ios_base::openmode mode) override
        {
            return seekoff(off_type(position), std::ios_base::beg, mode);
        }
    };

    std::vector<char> bytes_;
    Buffer buffer_;
};

// istream over one pak entry through a 16 KB window; every refill is one
// PakArchive::read on the shared handle.
class EntryStream : public std::istream
{
public:
    EntryStream(const PakArchive::Entry &entry)
        : std::istream(nullptr), buffer_(entry)
    {
        rdbuf(&buffer_);
    }

private:
    class Buffer : public std::streambuf
    {
    public:
        explicit Buffer(const PakArchive::Entry &entry)
            : entry_(entry), windowStart_(0)
        {
            setg(window_, window_, window_);
        }

    protected:
        int_type underflow() override
        {
            if (gptr() < egptr())
                return traits_type::to_int_type(*gptr());
            const std::uint32_t consumed = windowStart_ + static_cast<std::uint32_t>(egptr() - window_);
            if (consumed >= entry_.size)
                return traits_type::eof();
            const std::uint32_t length = std::min<std::uint32_t>(kWindowBytes, entry_.size - consumed);
            if (!archive().read(entry_, consumed, window_, length))
                return traits_type::eof();
            windowStart_ = consumed;
            setg(window_, window_, window_ + length);
            return traits_type::to_int_type(*gptr());
        }

        pos_type seekoff(off_type offset, std::ios_base::seekdir dir, std::ios_base::openmode mode) override
        {
            off_type base = 0;
            if (dir == std::ios_base::cur)
                base = static_cast<off_type>(windowStart_) + (gptr() - window_);
            else if (dir == std::ios_base::end)
                base = static_cast<off_type>(entry_.size);
            return seekpos(pos_type(base + offset), mode);
        }

        pos_type seekpos(pos_type position, std::ios_base::openmode) override
        {
            const off_type target = off_type(position);
            if (target < 0 || target > static_cast<off_type>(entry_.size))
                return pos_type(off_type(-1));
            const off_type windowEnd = static_cast<off_type>(windowStart_) + (egptr() - window_);
            if (target >= static_cast<off_type>(windowStart_) && target < windowEnd)
                setg(window_, window_ + (target - static_cast<off_type>(windowStart_)), egptr());
            else
            {
                // Empty window at the target; underflow() refills from there.
                windowStart_ = static_cast<std::uint32_t>(target);
                setg(window_, window_, window_);
            }
            return pos_type(target);
        }

    private:
        enum { kWindowBytes = 16u * 1024u };
        PakArchive::Entry entry_;
        std::uint32_t windowStart_;
        char window_[kWindowBytes];
    };

    Buffer buffer_;
};
}

namespace AssetPak
{

bool mountFrom(const std::string &baseDir)
{
    if (baseDir.empty())
        return false;
    std::string path = baseDir;
    // A bare device prefix ("host:", "mass:") takes the name directly.
    if (path.back() != '/' && path.back() != '\\' && path.back() != ':')
        path.push_back('/');
    path += kArchiveName;
    return mountFile(path);
}

bool mountFile(const std::string &path)
{
    if (path.empty())
        return false;

    // One pak per session. A different root asking afterwards is told no, so
    // a locator cannot adopt a root on the strength of a pak mounted elsewhere.
    if (archive().isOpen())
        return archive().path() == path;
    if (!archive().open(path))
    {
        if (s_lastRefusedPath != path)
        {
            s_lastRefusedPath = path;
            MC_LOG_INFO("assets", "[pak] no %s; reading loose files\n", path.c_str());
        }
        return false;
    }
    return true;
}

bool mounted()
{
    return archive().isOpen();
}

const std::string &archivePath()
{
    return archive().path();
}

bool isPakPath(const std::string &path)
{
    return path.compare(0, kSchemeLength, kScheme) == 0;
}

std::string makePath(const std::string &key)
{
    return std::string(kScheme) + PakArchive::normalizeKey(key);
}

std::string keyOf(const std::string &pakPath)
{
    return isPakPath(pakPath) ? pakPath.substr(kSchemeLength) : pakPath;
}

bool exists(const std::string &key)
{
    return archive().find(key) != nullptr;
}

long size(const std::string &key)
{
    const PakArchive::Entry *entry = archive().find(key);
    return entry != nullptr ? static_cast<long>(entry->size) : -1L;
}

bool isDirectory(const std::string &key)
{
    std::vector<std::string> children;
    return archive().find(key) == nullptr && archive().listChildren(key, children);
}

bool listChildren(const std::string &directoryKey, std::vector<std::string> &out)
{
    return archive().listChildren(directoryKey, out);
}

unsigned char *load(const std::string &key, unsigned int *outSize)
{
    if (outSize != nullptr)
        *outSize = 0;
    const PakArchive::Entry *entry = archive().find(key);
    if (entry == nullptr)
        return nullptr;
    unsigned char *data = allocateAligned(entry->size > 0 ? entry->size : 1u);
    if (data == nullptr)
        return nullptr;
    if (!archive().read(*entry, 0, data, entry->size))
    {
        std::free(data);
        return nullptr;
    }
    if (outSize != nullptr)
        *outSize = entry->size;
    return data;
}

bool read(const std::string &key, std::uint32_t offset, void *dst, std::uint32_t length)
{
    const PakArchive::Entry *entry = archive().find(key);
    return entry != nullptr && archive().read(*entry, offset, dst, length);
}

bool locate(const std::string &key, std::uint32_t *dataOffset, std::uint32_t *size)
{
    const PakArchive::Entry *entry = archive().find(key);
    if (entry == nullptr)
        return false;
    if (dataOffset != nullptr)
        *dataOffset = entry->dataOffset;
    if (size != nullptr)
        *size = entry->size;
    return true;
}

std::unique_ptr<std::istream> openStream(const std::string &key)
{
    const PakArchive::Entry *entry = archive().find(key);
    if (entry == nullptr)
        return nullptr;
    if (entry->size <= kStreamInMemoryBytes)
    {
        std::vector<char> bytes(entry->size);
        if (entry->size > 0 && !archive().read(*entry, 0, bytes.data(), entry->size))
            return nullptr;
        return std::unique_ptr<std::istream>(new MemoryStream(std::move(bytes)));
    }
    return std::unique_ptr<std::istream>(new EntryStream(*entry));
}

}
