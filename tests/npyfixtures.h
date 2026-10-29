#pragma once

// Captured-frames files for the tests: a .npy of [frames, height, width, 3] uint8 and the .npz that holds it, built the way NumPy writes them.

#include "importfixtures.h"

#include <QByteArray>

namespace npyfixtures {

inline importfixtures::ZipMember zipMember(const QByteArray &name, const QByteArray &data, bool deflate = true, bool badCrc = false, bool lie = false, quint32 declared = 0)
{
    importfixtures::ZipMember m;
    m.name = name;
    m.data = data;
    m.deflate = deflate;
    m.badCrc = badCrc;
    m.lieAboutSize = lie;
    m.declaredSize = declared;
    return m;
}

// Frame f is a flat colour (f * 20 + 10, 100, 200 - f * 10) with a bright left half, so a scaled picture can be told from another frame's.
inline QByteArray framePixels(int frames, int h, int w)
{
    QByteArray out(frames * h * w * 3, 0);
    for (int f = 0; f < frames; ++f)
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                char *p = out.data() + ((f * h + y) * w + x) * 3;
                const int boost = x < w / 2 ? 40 : 0;
                p[0] = char(qMin(255, f * 20 + 10 + boost));
                p[1] = char(100);
                p[2] = char(qMax(0, 200 - f * 10));
            }
    return out;
}

// A version 1.0 file whose header says `shape` (text such as "(2, 4, 4, 3)") and that holds `data`.
inline QByteArray npyWithShape(const QByteArray &shape, const QByteArray &data)
{
    QByteArray dict = "{'descr': '|u1', 'fortran_order': False, 'shape': " + shape + ", }";
    while ((10 + dict.size() + 1) % 64 != 0)
        dict.append(' ');
    dict.append('\n');
    return QByteArray("\x93NUMPY\x01\x00", 8) + importfixtures::le16(quint16(dict.size())) + dict + data;
}

inline QByteArray npyFile(int frames, int h, int w)
{
    return npyWithShape("(" + QByteArray::number(frames) + ", " + QByteArray::number(h) + ", " + QByteArray::number(w) + ", 3)", framePixels(frames, h, w));
}

inline QByteArray npzFile(int frames, int h, int w, bool deflate = true)
{
    return importfixtures::buildZip({zipMember("frames.npy", npyFile(frames, h, w), deflate)});
}

} // namespace npyfixtures
