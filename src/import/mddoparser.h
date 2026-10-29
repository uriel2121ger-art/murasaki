#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

#include <optional>

namespace murasaki::import {

// The action table of an Anima `.mdaa` package (`actions.mddo`): one action per line, blank lines skipped. A line is
// read as JSON first and, failing that, as a Python LITERAL; it must be a list or tuple of exactly three items [activation, path, position]. This parser is
// Murasaki's own and strict: it never evaluates anything. The literal grammar is small and closed:
//
//   value   := list | tuple | string | number | True | False | None
//   list    := '[' [value {',' value} [',']] ']'          tuple := '(' ... ')'   (a parenthesised single value without a comma is that value, as in Python)
//   string  := '...' or "..." with the escapes \\ \' \" \n \r \t \0 \xHH \uHHHH     (no prefixes r b u f, no triple quotes, no concatenation of neighbours)
//   number  := [+-] digits | [+-] digits '.' digits [exponent] | ... (decimal only; no underscores, no hex, no inf or nan)
//
// Anything else (names, operators, calls, comments, other brackets) is a refused line. Limits keep a hostile file small: see MddoLimits.
struct MddoLimits {
    qsizetype maxFileBytes = 1 << 20;
    qsizetype maxLineBytes = 4096;
    int maxLines = 10000;
    int maxDepth = 4;
    int maxItems = 64;
    qsizetype maxStringBytes = 1024;
    double maxCoordinate = 100000;
};

// One accepted line. `path` is the text as written (a backslash is not converted; resolving it inside the package is the importer's job).
struct MddoAction {
    int line = 0;                 // 1-based
    bool show = false;            // activation 1: show the animation (and move it); 0: hide it and stop the sound
    QString path;
    bool hasPosition = false;     // false when the position string said "no move" (N other than 1)
    double x = 0;
    double y = 0;
};

struct MddoRefusal {
    int line = 0;
    QString reason;               // short, plain English
};

struct MddoTable {
    QList<MddoAction> actions;
    QList<MddoRefusal> refused;   // the lines skipped, each with its reason
    QString error;                // the file as a whole is refused (a name of petimport.h): then actions is empty
    bool ok() const { return error.isEmpty(); }
};

MddoTable parseMddo(const QByteArray &file, const MddoLimits &limits = {});

// One line on its own (the same code), for the tests and the fuzz run: the action, or nullopt with *reason.
std::optional<MddoAction> parseMddoLine(const QByteArray &line, int lineNumber, QString *reason, const MddoLimits &limits = {});

} // namespace murasaki::import
