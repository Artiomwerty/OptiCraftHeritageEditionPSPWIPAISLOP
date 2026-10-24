#pragma once

#include <fstream>
#include <ios>
#include <string>

#include "RegionStorageStream.h"
#include "platform/storage/AssetPak.h"

#ifdef PSP_PLATFORM
#include <pspiofilemgr.h>

// The PSP toolchain's std::fstream opens files but cannot seek in them (tellg() after seeking to the
// end returns -1), so every region file failed to open with "Unable to determine region file length"
// and every saved chunk read as missing. This is the same interface on top of sceIo.
class PspIoFile
{
public:
    void open(const std::string &path, std::ios::openmode mode)
    {
        close();
        int flags = 0;
        if ((mode & std::ios::out) != 0)
        {
            flags = PSP_O_RDWR;
            if ((mode & std::ios::trunc) != 0)
                flags |= PSP_O_CREAT | PSP_O_TRUNC;
        }
        else
            flags = PSP_O_RDONLY;
        fd_ = sceIoOpen(path.c_str(), flags, 0777);
        fail_ = fd_ < 0;
        pos_ = 0;
        gcount_ = 0;
    }
    bool is_open() const { return fd_ >= 0; }
    void close()
    {
        if (fd_ >= 0)
            sceIoClose(fd_);
        fd_ = -1;
    }
    void clear() { fail_ = false; }
    explicit operator bool() const { return fd_ >= 0 && !fail_; }
    std::streamsize gcount() const { return gcount_; }
    std::streampos tellg() { return fail_ ? std::streampos(-1) : std::streampos(pos_); }
    void seekg(std::streampos pos) { seekTo(static_cast<long long>(pos)); }
    void seekg(std::streamoff off, std::ios::seekdir dir) { seekRel(off, dir); }
    void seekp(std::streampos pos) { seekTo(static_cast<long long>(pos)); }
    void seekp(std::streamoff off, std::ios::seekdir dir) { seekRel(off, dir); }
    void read(char *dst, std::streamsize count)
    {
        gcount_ = 0;
        if (fd_ < 0 || fail_) { fail_ = true; return; }
        int got = sceIoRead(fd_, dst, static_cast<SceSize>(count));
        if (got < 0) got = 0;
        gcount_ = got;
        pos_ += got;
        if (got != count) fail_ = true;
    }
    void write(const char *src, std::streamsize count)
    {
        if (fd_ < 0 || fail_) { fail_ = true; return; }
        const int put = sceIoWrite(fd_, src, static_cast<SceSize>(count));
        if (put > 0) pos_ += put;
        if (put != count) fail_ = true;
    }
    void flush() {}

private:
    void seekTo(long long pos)
    {
        if (fd_ < 0) { fail_ = true; return; }
        const SceOff r = sceIoLseek(fd_, static_cast<SceOff>(pos), PSP_SEEK_SET);
        if (r < 0) { fail_ = true; return; }
        pos_ = static_cast<long long>(r);
    }
    void seekRel(std::streamoff off, std::ios::seekdir dir)
    {
        if (fd_ < 0) { fail_ = true; return; }
        const int whence = dir == std::ios::beg ? PSP_SEEK_SET : (dir == std::ios::cur ? PSP_SEEK_CUR : PSP_SEEK_END);
        const SceOff r = sceIoLseek(fd_, static_cast<SceOff>(off), whence);
        if (r < 0) { fail_ = true; return; }
        pos_ = static_cast<long long>(r);
    }

    SceUID fd_ = -1;
    bool fail_ = false;
    long long pos_ = 0;
    std::streamsize gcount_ = 0;
};
#endif

// RegionFile's handle on platforms that keep region files open as real
// streams (PC, Wii). A path on disk is a std::fstream exactly as before; a
// "pak://" path -- a region of the tutorial world shipped inside assets.pak --
// is read through RegionStorageStream, which keeps only the header resident
// and fetches sector ranges on demand from the pak. Pak regions are read-only:
// opening one for writing fails, and RegionFile never asks for that on a
// read-only save. Only the subset of the std::fstream interface RegionFile
// uses is forwarded.
class RegionFileStream
{
public:
    void open(const std::string &path, std::ios::openmode mode)
    {
        close();
        if (AssetPak::isPakPath(path))
        {
            if ((mode & std::ios::out) != 0)
                return;
            pakOpen_ = pak_.open(path);
            return;
        }
        file_.open(path, mode);
    }

    bool is_open() const { return pakOpen_ || file_.is_open(); }

    void close()
    {
        if (file_.is_open())
            file_.close();
        pakOpen_ = false;
    }

    void clear()
    {
        if (pakOpen_) pak_.clear(); else file_.clear();
    }

    explicit operator bool() const
    {
        return pakOpen_ ? static_cast<bool>(pak_) : static_cast<bool>(file_);
    }

    bool operator!() const { return !static_cast<bool>(*this); }

    std::streamsize gcount() const { return pakOpen_ ? pak_.gcount() : file_.gcount(); }

    std::streampos tellg() { return pakOpen_ ? pak_.tellg() : file_.tellg(); }

    void seekg(std::streampos pos)
    {
        if (pakOpen_) pak_.seekg(pos); else file_.seekg(pos);
    }

    void seekg(std::streamoff off, std::ios::seekdir dir)
    {
        if (pakOpen_) pak_.seekg(off, dir); else file_.seekg(off, dir);
    }

    void seekp(std::streampos pos)
    {
        if (pakOpen_) pak_.seekp(pos); else file_.seekp(pos);
    }

    void seekp(std::streamoff off, std::ios::seekdir dir)
    {
        if (pakOpen_) pak_.seekp(off, dir); else file_.seekp(off, dir);
    }

    void read(char *dst, std::streamsize count)
    {
        if (pakOpen_) pak_.read(dst, count); else file_.read(dst, count);
    }

    void write(const char *src, std::streamsize count)
    {
        if (pakOpen_) pak_.write(src, count); else file_.write(src, count);
    }

    void flush()
    {
        if (pakOpen_) pak_.flush(); else file_.flush();
    }

private:
#ifdef PSP_PLATFORM
    PspIoFile file_;
#else
    std::fstream file_;
#endif
    RegionStorageStream pak_;
    bool pakOpen_ = false;
};
