// Fuzz targets for the parsers of hostile input in the pet import (BoundedZip and the .mddo parser), in two forms:
//
//   * with clang and -fsanitize=fuzzer (define MURASAKI_LIBFUZZER): LLVMFuzzerTestOneInput, one target per binary (MURASAKI_FUZZ_TARGET=zip|mddo);
//   * everywhere else (gcc): a seeded mutation driver, `fuzz_import zip|mddo [iterations] [seed]`, that starts from valid inputs and mutates them
//     (flips, inserts, deletes, truncation, splices), run under -fsanitize=address,undefined in a build directory of its own:
//         cmake -S . -B build/asan -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=undefined -g" -DMURASAKI_BUILD_FUZZ=ON
//         cmake --build build/asan --target fuzz_import && build/asan/tools/fuzz/fuzz_import zip 2000000
// Any sanitizer report or crash is a failure; the inputs are never written anywhere.

#include "boundedzip.h"
#include "mddoparser.h"

#include "../../tests/importfixtures.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QRandomGenerator>

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace murasaki::import;

namespace {

void fuzzZip(const QByteArray &data)
{
    QBuffer device;
    device.setData(data);
    if (!device.open(QIODevice::ReadOnly))
        return;
    ZipLimits limits;
    limits.maxMemoryBytes = 1 << 20;
    limits.maxMemberBytes = 8 << 20;
    limits.maxTotalBytes = 32 << 20;
    limits.skipOversize = (data.size() & 1) != 0; // both modes
    QString error;
    const auto zip = BoundedZip::open(&device, limits, &error);
    if (!zip)
        return;
    const int count = int(qMin<qsizetype>(zip->entries().size(), 24));
    for (int i = 0; i < count; ++i) {
        const QString name = zip->entries().at(i).name; // names are text only
        if (name.contains(QChar(0)) || name.startsWith(QLatin1Char('/')) || name.contains(QStringLiteral("..")) )
            std::abort();                                   // the reader promised these never get through
        (void)zip->head(i, 32, &error);
        (void)zip->read(i, &error);
        (void)zip->sha256(i, &error);
    }
}

void fuzzMddo(const QByteArray &data)
{
    const MddoTable table = parseMddo(data);
    for (const MddoAction &a : table.actions) {
        for (const QChar c : a.path)
            if (c.unicode() < 0x20)
                std::abort(); // a control character in a path was refused
    }
    QString reason;
    (void)parseMddoLine(data.left(4096), 1, &reason, {});
}

QByteArray mutate(QByteArray input, QRandomGenerator &rng, const QList<QByteArray> &seeds)
{
    const int steps = 1 + int(rng.bounded(6));
    for (int s = 0; s < steps; ++s) {
        switch (rng.bounded(7)) {
        case 0: // flip a byte
            if (!input.isEmpty())
                input[int(rng.bounded(quint32(input.size())))] = char(rng.bounded(256));
            break;
        case 1: // flip a bit
            if (!input.isEmpty())
                input[int(rng.bounded(quint32(input.size())))] ^= char(1 << rng.bounded(8));
            break;
        case 2: // insert random bytes
            input.insert(int(rng.bounded(quint32(input.size() + 1))), QByteArray(1 + int(rng.bounded(8)), char(rng.bounded(256))));
            break;
        case 3: // delete a span
            if (!input.isEmpty())
                input.remove(int(rng.bounded(quint32(input.size()))), 1 + int(rng.bounded(16)));
            break;
        case 4: // truncate
            if (!input.isEmpty() && rng.bounded(4) == 0)
                input.truncate(int(rng.bounded(quint32(input.size()))));
            break;
        case 5: // splice a piece of another seed
        {
            const QByteArray &other = seeds.at(int(rng.bounded(quint32(seeds.size()))));
            if (!other.isEmpty())
                input.insert(int(rng.bounded(quint32(input.size() + 1))), other.mid(int(rng.bounded(quint32(other.size()))), 1 + int(rng.bounded(64))));
            break;
        }
        default: // an interesting 16/32 bit value over a field: sizes, offsets, lengths
            if (input.size() > 4) {
                static const quint32 values[] = {0, 1, 0xffff, 0xffffffffu, 0x7fffffffu, 0x80000000u, 0x10000, 46, 30, 22};
                const int at = int(rng.bounded(quint32(input.size() - 3)));
                qToLittleEndian<quint32>(values[rng.bounded(10)], input.data() + at);
            }
        }
        if (input.size() > (1 << 20))
            input.truncate(1 << 20);
    }
    return input;
}

QList<QByteArray> zipSeeds()
{
    using importfixtures::ZipMember;
    const auto m = [](const char *name, const QByteArray &data, bool deflate) {
        ZipMember z;
        z.name = name;
        z.data = data;
        z.deflate = deflate;
        return z;
    };
    QList<QByteArray> seeds;
    seeds << importfixtures::buildZip({m("a.png", QByteArray(3000, 'a'), true), m("dir/b.gif", "GIF89a....", false), m("actions.mddo", "[1,'a','1-(1,2)']\n", true)});
    seeds << importfixtures::buildZip({m("x/", {}, false), m("x/y.bin", QByteArray(200000, 'q'), true)}, "a comment");
    ZipMember extra = m("e.bin", "extra extra extra", false);
    extra.localExtra = QByteArray(40, 'e');
    seeds << importfixtures::buildZip({extra, m("f.bin", "ffff", true)});
    seeds << QByteArray("PK\x05\x06", 4) + QByteArray(18, '\0');
    return seeds;
}

QList<QByteArray> mddoSeeds()
{
    return {QByteArray("[1, \"a.gif\", \"1-(120, 340)\"]\n(0, 'b.gif', '1-(0,0)')\n"), QByteArray("\xef\xbb\xbf[1, 'dir\\\\x.gif', [5.5, -6.25]]\r\n\r\n"),
            QByteArray("[1, 'caf\\xe9', (1,2),]\n[True, 'a', '1-(1,2)']\n__import__('os')\n[1,'a','2-(0,0)']"), QByteArray("[[[[[[1]]]]]]\n['a' 'b']\n")};
}

#ifdef MURASAKI_LIBFUZZER
} // namespace
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const QByteArray bytes(reinterpret_cast<const char *>(data), qsizetype(size));
#ifdef MURASAKI_FUZZ_MDDO
    fuzzMddo(bytes);
#else
    fuzzZip(bytes);
#endif
    return 0;
}
#else
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const std::string target = argc > 1 ? argv[1] : "";
    const long iterations = argc > 2 ? std::atol(argv[2]) : 200000;
    const quint32 seed = argc > 3 ? quint32(std::atol(argv[3])) : 20261024u;
    if (target != "zip" && target != "mddo") {
        std::fprintf(stderr, "usage: fuzz_import zip|mddo [iterations] [seed]\n");
        return 2;
    }
    const bool zip = target == "zip";
    const QList<QByteArray> seeds = zip ? zipSeeds() : mddoSeeds();
    QRandomGenerator rng(seed);
    long accepted = 0;
    for (long i = 0; i < iterations; ++i) {
        const QByteArray input = mutate(seeds.at(int(rng.bounded(quint32(seeds.size())))), rng, seeds);
        if (zip)
            fuzzZip(input);
        else
            fuzzMddo(input);
        ++accepted;
        if ((i + 1) % 100000 == 0)
            std::fprintf(stderr, "%s: %ld inputs\n", target.c_str(), i + 1);
    }
    std::fprintf(stderr, "%s: %ld inputs, no failure\n", target.c_str(), accepted);
    return 0;
}
#endif
