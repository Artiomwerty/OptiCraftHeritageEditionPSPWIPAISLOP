// PSP resources: game data comes from assets.pak beside the EBOOT (see AssetPak), with loose
// files under the install directory as a fallback. No SDL and no std::filesystem.
#include "platform/Resources.h"
#include "platform/storage/AssetPak.h"
#include "psp/system/PspSystem.h"

#include <cstdio>
#include <cstdlib>

std::string PlatformResources::baseDir() { return PspSystem::appDir(); }
std::string PlatformResources::assetsDir() { return baseDir() + "/assets"; }
std::string PlatformResources::audioDir() { return baseDir() + "/resources"; }

static bool fileExists(const std::string &path)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    std::fclose(f);
    return true;
}

std::string PlatformResources::resolveExisting(const std::string &path)
{
    if (AssetPak::mountFrom(baseDir()) && AssetPak::exists(path))
        return AssetPak::makePath(path);

    std::string resolved;
    if (path.rfind("assets/", 0) == 0)
        resolved = assetsDir() + "/" + path.substr(7);
    else
        resolved = baseDir() + "/" + path;
    return fileExists(resolved) ? resolved : std::string();
}

std::string PlatformResources::resolveAsset(const std::string &input)
{
    std::string path = input;
    if (!path.empty() && path[0] == '/')
        path.erase(path.begin());
    return resolveExisting("assets/" + path);
}

long PlatformResources::fileSize(const std::string &path)
{
    if (AssetPak::isPakPath(path))
        return AssetPak::size(AssetPak::keyOf(path));
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return -1L;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fclose(f);
    return size;
}

unsigned char *PlatformResources::loadFile(const std::string &path, unsigned int *outSize)
{
    if (AssetPak::isPakPath(path))
        return AssetPak::load(AssetPak::keyOf(path), outSize);
    if (outSize)
        *outSize = 0;
    const long size = fileSize(path);
    if (size <= 0)
        return nullptr;
    unsigned char *data = static_cast<unsigned char *>(std::malloc(static_cast<std::size_t>(size)));
    if (!data)
        return nullptr;
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f || std::fread(data, 1, static_cast<std::size_t>(size), f) != static_cast<std::size_t>(size))
    {
        if (f)
            std::fclose(f);
        std::free(data);
        return nullptr;
    }
    std::fclose(f);
    if (outSize)
        *outSize = static_cast<unsigned int>(size);
    return data;
}
