#include "platform/Storage.h"
#include "platform/storage/AssetPak.h"
#include "platform/storage/PakStorage.h"
#include "platform/storage/PathUtils.h"

#include <pspiofilemgr.h>

#include <cstring>

// "pak://" paths are the mounted assets.pak (read-only; see PakStorage), so a
// world shipped inside it -- the legacy tutorial -- opens through the same
// save-format code as one on disk.
namespace
{
bool statPath(const std::string& path, SceIoStat& st)
{
    std::memset(&st, 0, sizeof(st));
    return sceIoGetstat(path.c_str(), &st) >= 0;
}

bool readAll(SceUID fd, unsigned char* dst, std::size_t length)
{
    while (length > 0)
    {
        const int got = sceIoRead(fd, dst, static_cast<SceSize>(length > 65536 ? 65536 : length));
        if (got <= 0)
            return false;
        dst += got;
        length -= static_cast<std::size_t>(got);
    }
    return true;
}

bool writeFlags(const std::string& path, const void* data, std::size_t length, int flags)
{
    const SceUID fd = sceIoOpen(path.c_str(), flags, 0777);
    if (fd < 0)
        return false;
    const unsigned char* p = static_cast<const unsigned char*>(data);
    bool ok = true;
    while (length > 0)
    {
        const std::size_t n = length > 65536 ? 65536 : length;
        const int put = sceIoWrite(fd, p, static_cast<SceSize>(n));
        if (put != static_cast<int>(n))
        {
            ok = false;
            break;
        }
        p += n;
        length -= n;
    }
    sceIoClose(fd);
    return ok;
}

bool listImpl(const std::string& path, std::vector<std::string>& out, bool dirsOnly)
{
    out.clear();
    const SceUID d = sceIoDopen(path.c_str());
    if (d < 0)
        return false;
    SceIoDirent ent;
    std::memset(&ent, 0, sizeof(ent));
    while (sceIoDread(d, &ent) > 0)
    {
        if (ent.d_name[0] == '.' && (ent.d_name[1] == 0 || (ent.d_name[1] == '.' && ent.d_name[2] == 0)))
        {
            std::memset(&ent, 0, sizeof(ent));
            continue;
        }
        if (!dirsOnly || FIO_S_ISDIR(ent.d_stat.st_mode))
            out.push_back(ent.d_name);
        std::memset(&ent, 0, sizeof(ent));
    }
    sceIoDclose(d);
    return true;
}
} // namespace

// Everything on disk goes through raw sceIo: libc / std::filesystem path handling on "ms0:" paths
// proved unreliable (a new world failed with "Failed to write session lock").
namespace PlatformStorage
{
bool exists(const std::string& path)
{
    if (AssetPak::isPakPath(path))
        return PakStorage::exists(path);
    SceIoStat st;
    return statPath(path, st);
}

bool pathIsDirectory(const std::string& path)
{
    if (AssetPak::isPakPath(path))
        return PakStorage::isDirectory(path);
    SceIoStat st;
    return statPath(path, st) && FIO_S_ISDIR(st.st_mode);
}

bool mkdirs(const std::string& path)
{
    // Nothing to create inside the pak; the directory is there if entries are.
    if (AssetPak::isPakPath(path))
        return PakStorage::isDirectory(path);
    if (path.empty())
        return false;
    if (pathIsDirectory(path))
        return true;
    // Create each component after the device ("ms0:/") in turn.
    std::size_t start = path.find(':');
    start = (start == std::string::npos) ? 0 : start + 1;
    while (start < path.size() && path[start] == '/')
        ++start;
    for (std::size_t i = start; i <= path.size(); ++i)
    {
        if (i == path.size() || path[i] == '/')
        {
            if (i > start)
                sceIoMkdir(path.substr(0, i).c_str(), 0777);
        }
    }
    return pathIsDirectory(path);
}

bool removeFile(const std::string& path)
{
    SceIoStat st;
    if (!statPath(path, st))
        return false;
    return (FIO_S_ISDIR(st.st_mode) ? sceIoRmdir(path.c_str()) : sceIoRemove(path.c_str())) >= 0;
}

bool renameFile(const std::string& from, const std::string& to)
{
    if (exists(to))
        removeFile(to);
    // sceIoRename wants the destination without the device prefix on real hardware.
    std::string dest = to;
    const std::size_t colon = dest.find(':');
    if (colon != std::string::npos)
    {
        const std::size_t fromColon = from.find(':');
        if (fromColon != std::string::npos && from.compare(0, fromColon, dest, 0, colon) == 0)
            dest = dest.substr(colon + 1);
    }
    if (sceIoRename(from.c_str(), dest.c_str()) >= 0)
        return true;
    return sceIoRename(from.c_str(), to.c_str()) >= 0;
}

bool supportsAtomicRename() { return true; }
bool supportsSessionLocks() { return true; }

bool readFile(const std::string& path, std::vector<unsigned char>& out)
{
    if (AssetPak::isPakPath(path))
        return PakStorage::readFile(path, out);
    SceIoStat st;
    if (!statPath(path, st))
        return false;
    const SceUID fd = sceIoOpen(path.c_str(), PSP_O_RDONLY, 0);
    if (fd < 0)
        return false;
    out.resize(static_cast<std::size_t>(st.st_size));
    const bool ok = out.empty() || readAll(fd, out.data(), out.size());
    sceIoClose(fd);
    return ok;
}

bool writeFile(const std::string& path, const void* data, std::size_t length)
{
    const std::string parentDir = parent(path);
    if (!parentDir.empty() && !mkdirs(parentDir))
        return false;
    return writeFlags(path, data, length, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC);
}

bool appendFile(const std::string& path, const void* data, std::size_t length)
{
    const std::string parentDir = parent(path);
    if (!parentDir.empty() && !mkdirs(parentDir))
        return false;
    return writeFlags(path, data, length, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND);
}

std::int64_t getFileSize(const std::string& path)
{
    if (AssetPak::isPakPath(path))
        return PakStorage::fileSize(path);
    SceIoStat st;
    return statPath(path, st) ? static_cast<std::int64_t>(st.st_size) : -1;
}

bool readFileRange(const std::string& path, std::size_t offset, void* out, std::size_t length)
{
    if (AssetPak::isPakPath(path))
        return PakStorage::readFileRange(path, offset, out, length);
    const SceUID fd = sceIoOpen(path.c_str(), PSP_O_RDONLY, 0);
    if (fd < 0)
        return false;
    bool ok = sceIoLseek32(fd, static_cast<int>(offset), PSP_SEEK_SET) == static_cast<int>(offset);
    ok = ok && readAll(fd, static_cast<unsigned char*>(out), length);
    sceIoClose(fd);
    return ok;
}

bool listPathEntries(const std::string& path, std::vector<std::string>& out)
{
    if (AssetPak::isPakPath(path))
        return PakStorage::listEntries(path, out);
    return listImpl(path, out, false);
}

bool listDirs(const std::string& path, std::vector<std::string>& out)
{
    if (AssetPak::isPakPath(path))
        return PakStorage::listDirs(path, out);
    return listImpl(path, out, true);
}
}
