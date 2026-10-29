#pragma once

// The line protocol between the daemon and murasaki-screenmatch. One command or reply per line, ASCII,
// single spaces, at most 4096 bytes with the newline, over one inherited socketpair. Everything the daemon says is a command parsed by a strict grammar: an unknown word, a wrong count,
// a number out of range or a byte outside printable ASCII refuses the line (and the helper answers `err`). Everything the helper says is a reply: scores and yes/no, and, only when
// asked, the one picture of a capture. No pixel of the live screen is in any other line.
//
// Daemon to helper:
//   clear                                      forget every trigger, reference and pet rectangle
//   trigger <id> <hash|structure>              declare a trigger (at most 4); <id> is 1 to 64 of A-Z a-z 0-9 _ -
//   region <id> <r> <stream> <x> <y> <w> <h>   region r (0 to 7) of the trigger, in the pixels of the stream (at most 16 regions in all)
//   ref <id> <r> <step> <base64>               the reference of step (0 to 63): its 32x32 grey grid, 1024 bytes
//   want <id> <step>                           which step the next ticks compare with (default 0)
//   pets <n> (<stream> <x> <y> <w> <h>)...     rectangles of our own pets, at most 64: a region more than half covered is skipped that tick
//   rate <n>                                   ticks a second, 1 to 16
//   start | stop                               watch the screen, or not
//   capture <req> <stream> <x> <y> <w> <h>     one picture of a rectangle, scaled to a longer side of 360 (when the helper was started to allow it)
//   ping | quit
// Helper to daemon:
//   hello <version>
//   ok <word> | err <word> <reason>
//   s <id> <step> <p>...                       the scores of regions 0 to the last declared one of a trigger this tick, in permille 0 to 1000, `-` for a region that could not be scored (or was not declared)
//   t <n>                                      end of tick n
//   state <waiting|watching|stopped>
//   shot <req> <w> <h> <bytes>, chunk <req> <seq> <base64>, shotend <req>   a capture: RGB, row by row
//   pong

#include "screenmatch.h"

#include <QByteArray>
#include <QList>
#include <QString>

#include <optional>

namespace murasaki::screenmatch {

inline constexpr int kProtocolVersion = 1;
inline constexpr int kMaxLineBytes = 4096;
inline constexpr int kMaxTriggers = 4;
inline constexpr int kMaxRegionsPerTrigger = 8;
inline constexpr int kMaxRegionsTotal = 16;
inline constexpr int kMaxSteps = 64;
inline constexpr int kMaxPets = 64;
inline constexpr int kMaxStreams = 8;
inline constexpr int kMinRate = 1;
inline constexpr int kMaxRate = 16;
inline constexpr int kMaxCoordinate = 65535;
inline constexpr int kMaxSide = 16384;
inline constexpr int kGridBytes = kScreenStructureSide * kScreenStructureSide;
inline constexpr int kChunkBytes = 2048;               // raw bytes in one `chunk` line (2732 base64 characters)
inline constexpr int kMaxCaptureBytes = 360 * 360 * 3; // the most a capture can hold

enum class CommandType { Clear, Trigger, Region, Ref, Want, Pets, Rate, Start, Stop, Capture, Ping, Quit };

struct PetRect {
    int stream = 0;
    int x = 0, y = 0, w = 0, h = 0;
};

struct Command {
    CommandType type = CommandType::Ping;
    QString trigger;
    ScreenMatchMode mode = ScreenMatchMode::Hash;
    int region = 0;
    int stream = 0;
    int x = 0, y = 0, w = 0, h = 0;
    int step = 0;
    QByteArray grid;           // Ref: kGridBytes bytes
    QList<PetRect> pets;       // Pets
    int rate = 0;              // Rate
    int request = 0;           // Capture
};

// 1 to 64 of A-Z a-z 0-9 _ -
bool validTriggerId(const QString &id);

// The first word of a line, for the answers (`ok trigger`).
QString commandWord(CommandType type);

// Parses one line (without its newline). nullopt with *error set to a short reason; the reason never echoes the line.
std::optional<Command> parseCommand(const QByteArray &line, QString *error = nullptr);
// The line of a command (no newline). The inverse of parseCommand for every command it accepts.
QByteArray formatCommand(const Command &command);

enum class ReplyType { Hello, Ok, Err, Scores, Tick, State, Shot, Chunk, ShotEnd, Pong };

struct Reply {
    ReplyType type = ReplyType::Pong;
    int version = 0;           // Hello
    QString word;              // Ok, Err, State
    QString reason;            // Err
    QString trigger;           // Scores
    int step = 0;              // Scores
    QList<int> permille;       // Scores: -1 for `-`
    qint64 tick = 0;           // Tick
    int request = 0;           // Shot, Chunk, ShotEnd
    int width = 0, height = 0; // Shot
    int bytes = 0;             // Shot
    int sequence = 0;          // Chunk
    QByteArray data;           // Chunk: the decoded bytes
};

std::optional<Reply> parseReply(const QByteArray &line, QString *error = nullptr);
QByteArray formatReply(const Reply &reply);

// Cuts a byte stream into lines of at most kMaxLineBytes. A longer line is dropped to its newline and counted; the bytes of a line are never kept beyond the limit.
class LineBuffer
{
public:
    // Appends bytes and returns the complete lines (without newline) they finish.
    QList<QByteArray> feed(const char *data, qsizetype length);
    int overlong() const { return m_overlong; } // lines refused for their length so far
    qsizetype pending() const { return m_buffer.size(); }

private:
    QByteArray m_buffer;
    bool m_discarding = false;
    int m_overlong = 0;
};

} // namespace murasaki::screenmatch
