// scene-vk-particlesurvey [--assets DIR] ITEMDIR...: lists every particle system in the given scene items (the scene
// objects with a "particle" file and the children those files name) and counts the emitter, initializer, operator and
// renderer types, the control-point usage, flags and materials. Markdown on stdout. Development tool: its output is
// docs/scene-vk/particles.md and decides which particle features come first.
#include "assets/vfs.h"
#include "scene/scene.h"
#include "util/json.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace murasaki::scene;

namespace {

struct Tally {
    std::map<std::string, int> uses;          // total occurrences
    std::map<std::string, std::set<std::string>> items; // item ids that use it
    void add(const std::string &name, const std::string &item)
    {
        ++uses[name];
        items[name].insert(item);
    }
};

struct Survey {
    Tally emitters, initializers, operators, renderers, children, materials, flags, overrides, controlPoints, operatorKeys;
    int items = 0;
    int itemsWithParticles = 0;
    int systems = 0;
    int missing = 0;
    int controlPointSystems = 0;
    int maxCountTotal = 0;
    int maxCountMax = 0;
    std::map<std::string, std::string> notes; // item -> note about unreadable files
};

std::string baseName(const std::string &path)
{
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

void list(const Json &j, const char *key, Tally &into, const std::string &item, Tally *keys = nullptr, const std::string &prefix = {})
{
    const Json *array = j.find(key);
    if (!array || !array->isArray()) {
        return;
    }
    for (const Json &entry : array->items()) {
        const std::string name = entry.stringOr("name", "(unnamed)");
        into.add(name, item);
        if (keys) {
            for (const auto &[member, value] : entry.members()) {
                if (member != "name" && member != "id") {
                    keys->add(prefix + name + "." + member, item);
                }
            }
        }
    }
}

void system(Survey &s, const Vfs &vfs, const std::string &item, const std::string &file, int depth)
{
    const auto text = vfs.readText(file);
    if (!text) {
        ++s.missing;
        s.notes[item] += " missing " + file;
        return;
    }
    std::string error;
    const auto json = parseJson(*text, &error);
    if (!json) {
        s.notes[item] += " unparsable " + file + " (" + error + ")";
        return;
    }
    ++s.systems;
    list(*json, "emitter", s.emitters, item, &s.operatorKeys, "emitter:");
    list(*json, "initializer", s.initializers, item, &s.operatorKeys, "init:");
    list(*json, "operator", s.operators, item, &s.operatorKeys, "op:");
    list(*json, "renderer", s.renderers, item, &s.operatorKeys, "render:");
    s.materials.add(json->stringOr("material", "(none)"), item);
    s.flags.add(std::to_string(static_cast<long>(json->numberOr("flags", 0))), item);
    const int maxCount = static_cast<int>(json->numberOr("maxcount", 0));
    s.maxCountTotal += maxCount;
    s.maxCountMax = std::max(s.maxCountMax, maxCount);
    if (const Json *cp = json->find("controlpoint"); cp && cp->isArray() && !cp->items().empty()) {
        ++s.controlPointSystems;
        for (const Json &c : cp->items()) {
            s.controlPoints.add("controlpoint flags " + std::to_string(static_cast<long>(c.numberOr("flags", 0))), item);
        }
    }
    if (const Json *kids = json->find("children"); kids && kids->isArray()) {
        for (const Json &k : kids->items()) {
            const std::string name = k.stringOr("name");
            s.children.add(k.stringOr("type", "(untyped)"), item);
            if (!name.empty() && depth < 4) {
                system(s, vfs, item, name, depth + 1);
            }
        }
    }
}

void scene(Survey &s, const std::string &dir, const std::string &assets)
{
    const std::string item = baseName(dir.back() == '/' ? dir.substr(0, dir.size() - 1) : dir);
    const Vfs vfs = Vfs::forItem(dir, assets);
    ++s.items;
    const auto text = vfs.readText(sceneFileName(vfs));
    if (!text) {
        return;
    }
    const auto json = parseJson(*text);
    const Json *objects = json ? json->find("objects") : nullptr;
    if (!objects || !objects->isArray()) {
        return;
    }
    bool any = false;
    for (const Json &o : objects->items()) {
        const Json *particle = o.find("particle");
        if (!particle || !particle->isString()) {
            continue;
        }
        any = true;
        if (const Json *ov = o.find("instanceoverride"); ov && ov->isObject()) {
            for (const auto &[member, value] : ov->members()) {
                s.overrides.add(member, item);
            }
        }
        system(s, vfs, item, particle->asString(), 0);
    }
    s.itemsWithParticles += any ? 1 : 0;
}

void print(const char *title, const Tally &t)
{
    std::printf("\n## %s\n\n| name | uses | items |\n|---|---|---|\n", title);
    std::vector<std::pair<std::string, int>> rows(t.uses.begin(), t.uses.end());
    std::sort(rows.begin(), rows.end(), [&](const auto &a, const auto &b) {
        const size_t ia = t.items.at(a.first).size();
        const size_t ib = t.items.at(b.first).size();
        return ia != ib ? ia > ib : a.first < b.first;
    });
    for (const auto &[name, uses] : rows) {
        std::printf("| `%s` | %d | %zu |\n", name.c_str(), uses, t.items.at(name).size());
    }
}

} // namespace

int main(int argc, char **argv)
{
    std::string assets;
    std::vector<std::string> dirs;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--assets") && i + 1 < argc) {
            assets = argv[++i];
        } else {
            dirs.push_back(argv[i]);
        }
    }
    if (dirs.empty()) {
        std::fprintf(stderr, "usage: scene-vk-particlesurvey [--assets DIR] ITEMDIR...\n");
        return 2;
    }
    Survey s;
    for (const std::string &d : dirs) {
        scene(s, d, assets.empty() ? findAssetsDir({}, d) : assets);
    }
    std::printf("# Particle survey\n\n%d scene items read, %d with particle objects, %d particle systems (children included), %d unreadable files, "
                "%d systems with control points, `maxcount` total %d, largest %d.\n",
                s.items, s.itemsWithParticles, s.systems, s.missing, s.controlPointSystems, s.maxCountTotal, s.maxCountMax);
    print("Emitters", s.emitters);
    print("Initializers", s.initializers);
    print("Operators", s.operators);
    print("Renderers", s.renderers);
    print("Children", s.children);
    print("Control points", s.controlPoints);
    print("Instance overrides in scene.json", s.overrides);
    print("Flags", s.flags);
    print("Materials", s.materials);
    print("Members of emitters, initializers, operators and renderers", s.operatorKeys);
    if (!s.notes.empty()) {
        std::printf("\n## Notes\n\n");
        for (const auto &[item, note] : s.notes) {
            std::printf("- %s:%s\n", item.c_str(), note.c_str());
        }
    }
    return 0;
}
