#include "GameResources.h"
#include "platform/Resources.h"
#include "platform/storage/AssetPak.h"
#ifdef PS2_PLATFORM
#include "platform/storage/StdioStream.h"
#else
#include <fstream>
#endif

#include <sstream>
#include "platform/Storage.h"

namespace GameResources
{
std::string getExeDir() { return PlatformResources::baseDir(); }
std::string getAssetsDir() { return PlatformResources::assetsDir(); }
std::string getAudioResourcesDir() { return PlatformResources::audioDir(); }
std::string resolve(const std::string& mcPath) { return PlatformResources::resolveAsset(mcPath); }

std::unique_ptr<std::istream> openPath(const std::string& resolvedPath)
{
    if (resolvedPath.empty()) return nullptr;
    if (AssetPak::isPakPath(resolvedPath))
        return AssetPak::openStream(AssetPak::keyOf(resolvedPath));
#ifdef PS2_PLATFORM
    return PlatformStorage::openStdioInputStream(resolvedPath);
#else
    auto input = std::make_unique<std::ifstream>(resolvedPath, std::ios::binary);
    if (!input->good()) return nullptr;
    return std::unique_ptr<std::istream>(std::move(input));
#endif
}

std::unique_ptr<std::istream> open(const std::string& mcPath)
{
    if (mcPath.find(':') != std::string::npos || mcPath.rfind("./", 0) == 0 || PlatformStorage::exists(mcPath))
    {
        std::vector<unsigned char> fileBytes;
        if (PlatformStorage::readFile(mcPath, fileBytes) && !fileBytes.empty())
        {
            std::string s(reinterpret_cast<const char*>(fileBytes.data()), fileBytes.size());
            return std::make_unique<std::istringstream>(s, std::ios::binary);
        }
    }

    std::string resolved = resolve(mcPath);
#ifdef PS2_PLATFORM
    if (resolved.empty())
    {
        std::string fallback = mcPath;
        if (!fallback.empty() && fallback[0] == '/') fallback.erase(fallback.begin());
        resolved = PlatformResources::resolveExisting(fallback);
    }
#endif
    return openPath(resolved);
}
}
