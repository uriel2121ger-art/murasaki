#include "protocol.h"

#include "petaudioasset.h"

#include <charconv>
#include <vector>

namespace murasaki::audio {

namespace {

std::vector<std::string_view> split(std::string_view line)
{
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i <= line.size()) {
        const std::size_t j = line.find(' ', i);
        const std::size_t end = j == std::string_view::npos ? line.size() : j;
        out.push_back(line.substr(i, end - i));
        if (j == std::string_view::npos)
            break;
        i = j + 1;
    }
    return out;
}

template <typename T>
bool number(std::string_view text, T *out)
{
    if (text.empty() || text.size() > 20)
        return false;
    T value{};
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc() || end != text.data() + text.size())
        return false;
    *out = value;
    return true;
}

} // namespace

std::optional<Command> parseCommand(std::string_view line)
{
    if (line.empty() || line.size() > kMaxLineBytes)
        return std::nullopt;
    for (char c : line)
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7e)
            return std::nullopt;
    const std::vector<std::string_view> w = split(line);
    Command c;
    const auto pet = [&](std::string_view s) {
        int v = 0;
        if (!number(s, &v) || v < 0 || v >= 256)
            return false;
        c.pet = v;
        return true;
    };
    if (w[0] == "quit" && w.size() == 1) {
        c.kind = Command::Kind::Quit;
        return c;
    }
    if ((w[0] == "stop" || w[0] == "cancel" || w[0] == "pause" || w[0] == "resume") && w.size() == 2 && pet(w[1])) {
        c.kind = w[0] == "stop" ? Command::Kind::Stop : w[0] == "cancel" ? Command::Kind::Cancel : w[0] == "pause" ? Command::Kind::Pause : Command::Kind::Resume;
        return c;
    }
    if (w[0] == "gain" && w.size() == 3 && pet(w[1]) && number(w[2], &c.gain) && c.gain >= 0 && c.gain <= 1000) {
        c.kind = Command::Kind::Gain;
        return c;
    }
    if (w[0] == "master" && w.size() == 2 && number(w[1], &c.gain) && c.gain >= 0 && c.gain <= 1000) {
        c.kind = Command::Kind::Master;
        return c;
    }
    // play <pet> <gen> <asset64hex> <startFrame> <gain> <loop> <atNs>
    int loop = 0;
    if (w[0] == "play" && w.size() == 8 && pet(w[1]) && number(w[2], &c.generation) && isAssetId(w[3]) && number(w[4], &c.startFrame) && c.startFrame >= 0
        && number(w[5], &c.gain) && c.gain >= 0 && c.gain <= 1000 && number(w[6], &loop) && (loop == 0 || loop == 1) && number(w[7], &c.atNs) && c.atNs >= 0) {
        c.kind = Command::Kind::Play;
        c.asset = std::string(w[3]);
        c.loop = loop == 1;
        return c;
    }
    return std::nullopt;
}

std::string formatCommand(const Command &c)
{
    switch (c.kind) {
    case Command::Kind::Quit:
        return "quit";
    case Command::Kind::Stop:
        return "stop " + std::to_string(c.pet);
    case Command::Kind::Cancel:
        return "cancel " + std::to_string(c.pet);
    case Command::Kind::Pause:
        return "pause " + std::to_string(c.pet);
    case Command::Kind::Resume:
        return "resume " + std::to_string(c.pet);
    case Command::Kind::Gain:
        return "gain " + std::to_string(c.pet) + " " + std::to_string(c.gain);
    case Command::Kind::Master:
        return "master " + std::to_string(c.gain);
    case Command::Kind::Play:
        return "play " + std::to_string(c.pet) + " " + std::to_string(c.generation) + " " + c.asset + " " + std::to_string(c.startFrame) + " "
            + std::to_string(c.gain) + " " + (c.loop ? "1" : "0") + " " + std::to_string(c.atNs);
    }
    return "quit";
}

} // namespace murasaki::audio
