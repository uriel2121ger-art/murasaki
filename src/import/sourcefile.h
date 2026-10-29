#pragma once

#include <QFile>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace murasaki::import {

// Opens `file` (its fileName) for reading without ever blocking on it: the path may have been swapped for a FIFO or a device since it was checked, so it is
// opened non-blocking and the descriptor itself must be a regular file (a symlink is followed once, as the checks do).
inline bool openSource(QFile &file)
{
    const int fd = ::open(QFile::encodeName(file.fileName()).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return false;
    struct stat st;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        return false;
    }
    if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(fd);
        return false;
    }
    return true;
}

} // namespace murasaki::import
