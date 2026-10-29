// scene-vk-shadercheck: translates and compiles every shader the scenes of a Steam library use, with the combos
// and bound textures their materials give, and reports how many compile and why the rest do not. A development tool
// (manual, not a test): the library is only read. Also compiles every shader of the Wallpaper Engine assets with its
// default combos as a second corpus.
//
//   scene-vk-shadercheck [--assets DIR] [--workshop DIR] [--only ID] [--verbose] [--dump-failed DIR]

#include "assets/vfs.h"
#include "scene/scene.h"
#include "shaders/source.h"
#include "shaders/translate.h"
#include "util/json.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

using namespace murasaki::scene;
namespace fs = std::filesystem;

namespace {

struct Job {
    std::string shader;                 // "effects/tint" or "genericimage2"
    std::map<std::string, int> combos;
    std::vector<uint32_t> textures;
    std::string where;                  // item id
    std::string itemDir;                // the item whose files (its own shaders and effects) apply; empty: assets only
};

int comboValue(const Json &v)
{
    if (v.isNumber()) {
        return static_cast<int>(v.asNumber());
    }
    if (v.isBool()) {
        return v.asBool() ? 1 : 0;
    }
    return 0;
}

void takeCombos(const Json *combos, std::map<std::string, int> &out)
{
    if (combos && combos->isObject()) {
        for (const auto &[k, v] : combos->members()) {
            std::string up = k;
            for (char &c : up) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            out[up] = comboValue(v);
        }
    }
}

void takeTextures(const Json *textures, std::set<uint32_t> &bound)
{
    if (textures && textures->isArray()) {
        for (size_t i = 0; i < textures->items().size(); ++i) {
            if (textures->items()[i].isString() && !textures->items()[i].asString().empty()) {
                bound.insert(static_cast<uint32_t>(i));
            }
        }
    }
}

// The jobs of one material file (all passes); `instance` is the scene's per-pass override of that effect pass.
void materialJobs(const Vfs &vfs, const std::string &itemDir, const std::string &materialPath, const Json *instance, const std::string &where,
                  std::vector<Job> &jobs)
{
    const auto text = vfs.readText(materialPath);
    const auto json = text ? parseJson(*text) : std::nullopt;
    if (!json) {
        return;
    }
    const Json *passes = json->find("passes");
    if (!passes || !passes->isArray()) {
        return;
    }
    for (const Json &pass : passes->items()) {
        Job job;
        job.shader = pass.stringOr("shader");
        job.where = where;
        job.itemDir = itemDir;
        takeCombos(pass.find("combos"), job.combos);
        std::set<uint32_t> bound;
        takeTextures(pass.find("textures"), bound);
        if (instance) {
            takeCombos(instance->find("combos"), job.combos);
            takeTextures(instance->find("textures"), bound);
        }
        job.textures.assign(bound.begin(), bound.end());
        if (!job.shader.empty()) {
            jobs.push_back(std::move(job));
        }
    }
}

void sceneJobs(const std::string &itemDir, const std::string &assets, const std::string &id, std::vector<Job> &jobs)
{
    const Vfs vfs = Vfs::forItem(itemDir, assets);
    const auto project = readProject(vfs);
    if (project && !project->type.empty()) {
        std::string t = project->type;
        for (char &c : t) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (t != "scene") {
            return;
        }
    }
    const auto sceneText = vfs.readText(project && !project->file.empty() ? project->file : "scene.json");
    const auto scene = sceneText ? parseJson(*sceneText) : std::nullopt;
    if (!scene) {
        return;
    }
    const Json *objects = scene->find("objects");
    if (!objects || !objects->isArray()) {
        return;
    }
    for (const Json &o : objects->items()) {
        const Json *image = o.find("image");
        if (image && image->isString()) {
            if (const auto model = vfs.readText(image->asString())) {
                if (const auto m = parseJson(*model)) {
                    materialJobs(vfs, itemDir, m->stringOr("material"), nullptr, id, jobs);
                }
            }
        }
        const Json *effects = o.find("effects");
        if (!effects || !effects->isArray()) {
            continue;
        }
        for (const Json &fx : effects->items()) {
            const auto effectText = vfs.readText(fx.stringOr("file"));
            const auto effect = effectText ? parseJson(*effectText) : std::nullopt;
            const Json *passes = effect ? effect->find("passes") : nullptr;
            const Json *instancePasses = fx.find("passes");
            if (!passes || !passes->isArray()) {
                continue;
            }
            for (size_t i = 0; i < passes->items().size(); ++i) {
                const Json *instance = instancePasses && instancePasses->isArray() && i < instancePasses->items().size()
                    ? &instancePasses->items()[i]
                    : nullptr;
                materialJobs(vfs, itemDir, passes->items()[i].stringOr("material"), instance, id, jobs);
            }
        }
    }
}

// "ERROR: 0:123: 'foo' : message" -> "message" (what, not where), so equal causes group together.
std::string category(const std::string &error)
{
    std::istringstream in(error);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("ERROR:", 0) == 0 && line.find("compilation terminated") == std::string::npos
            && line.find("compile errors") == std::string::npos
            && line.find("compilation errors") == std::string::npos) {
            // strip "ERROR: 0:N:" prefix
            size_t colon = line.find(':', 7);
            colon = colon == std::string::npos ? colon : line.find(':', colon + 1);
            std::string rest = colon == std::string::npos ? line : line.substr(colon + 1);
            // drop quoted symbol names that vary: keep the text after the last " : "
            const size_t sep = rest.rfind(" : ");
            if (sep != std::string::npos) {
                rest = rest.substr(sep + 3);
            }
            while (!rest.empty() && rest.front() == ' ') {
                rest.erase(rest.begin());
            }
            return rest.substr(0, 90);
        }
    }
    return error.substr(0, 90);
}

} // namespace

int main(int argc, char **argv)
{
    std::string assets = findAssetsDir({}, {});
    std::string workshop;
    std::string only;
    std::string dumpDir;
    bool verbose = false;
    bool dumpUniforms = false;
    std::map<std::string, int> engineUniforms; // uniforms with no `material` key: the engine has to fill them
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--assets" && i + 1 < argc) {
            assets = argv[++i];
        } else if (a == "--workshop" && i + 1 < argc) {
            workshop = argv[++i];
        } else if (a == "--only" && i + 1 < argc) {
            only = argv[++i];
        } else if (a == "--dump-failed" && i + 1 < argc) {
            dumpDir = argv[++i];
        } else if (a == "--engine-uniforms") {
            dumpUniforms = true;
        } else if (a == "--verbose") {
            verbose = true;
        } else {
            std::fprintf(stderr, "usage: scene-vk-shadercheck [--assets DIR] [--workshop DIR] [--only ID] [--verbose] [--dump-failed DIR]\n");
            return 2;
        }
    }
    if (assets.empty()) {
        std::fprintf(stderr, "Wallpaper Engine assets not found (--assets DIR)\n");
        return 2;
    }
    if (workshop.empty()) {
        workshop = fs::path(assets).parent_path().parent_path().parent_path().string() + "/workshop/content/431960";
    }

    // --- corpus A: the scenes' own shader uses ---
    std::vector<Job> jobs;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(workshop, ec)) {
        if (entry.is_directory(ec) && (only.empty() || entry.path().filename() == only)) {
            sceneJobs(entry.path().string(), assets, entry.path().filename().string(), jobs);
        }
    }
    // --- corpus B: every shipped shader with its default combos ---
    std::vector<Job> shipped;
    if (only.empty()) {
        for (const auto &entry : fs::recursive_directory_iterator(fs::path(assets) / "shaders", ec)) {
            if (entry.path().extension() == ".frag") {
                const std::string rel = fs::relative(entry.path(), fs::path(assets) / "shaders").replace_extension("").string();
                if (fs::exists(fs::path(entry.path()).replace_extension(".vert"))) {
                    shipped.push_back({rel, {}, {}, "shipped"});
                }
            }
        }
    }

    std::map<std::string, Vfs> vfsFor;
    struct Outcome {
        bool ok = false;
        std::string error;
    };
    const auto run = [&](const std::vector<Job> &list, const char *title) {
        std::map<std::string, Outcome> unique; // key -> outcome
        std::map<std::string, std::string> firstWhere;
        std::map<std::string, std::string> shaderOf;
        int ok = 0;
        for (const Job &job : list) {
            std::ostringstream key;
            key << job.shader;
            for (const auto &[k, v] : job.combos) {
                key << ' ' << k << '=' << v;
            }
            key << " [" << job.itemDir << "] tex";
            for (uint32_t t : job.textures) {
                key << t << ',';
            }
            if (unique.count(key.str())) {
                continue;
            }
            Outcome outcome;
            std::string error;
            auto found = vfsFor.find(job.itemDir);
            if (found == vfsFor.end()) {
                found = vfsFor.emplace(job.itemDir, Vfs::forItem(job.itemDir.empty() ? "." : job.itemDir, assets)).first;
            }
            const Vfs &vfs = found->second;
            const auto vert = loadShaderSource(vfs, "shaders/" + job.shader + ".vert", &error);
            const auto frag = vert ? loadShaderSource(vfs, "shaders/" + job.shader + ".frag", &error) : std::nullopt;
            if (vert && frag) {
                TranslateOptions options;
                options.combos = job.combos;
                options.boundTextures = job.textures;
                const auto translated = translateShader(*vert, *frag, options, &error);
                outcome.ok = translated.has_value();
                if (translated && dumpUniforms) {
                    for (const StageProgram *st : {&translated->vertex, &translated->fragment}) {
                        for (const UniformInfo &u : st->uniforms) {
                            if (u.materialKey.empty()) {
                                ++engineUniforms[u.type + " " + u.name];
                            }
                        }
                    }
                }
                if (!outcome.ok && !dumpDir.empty()) {
                    if (const auto text = translateToGlsl(*vert, *frag, options, nullptr)) {
                        fs::create_directories(dumpDir, ec);
                        std::string base = job.shader;
                        std::replace(base.begin(), base.end(), '/', '_');
                        std::ofstream(dumpDir + "/" + base + ".vert") << text->vertex;
                        std::ofstream(dumpDir + "/" + base + ".frag") << text->fragment;
                    }
                }
            }
            outcome.error = error;
            ok += outcome.ok ? 1 : 0;
            firstWhere[key.str()] = job.where;
            shaderOf[key.str()] = job.shader;
            unique.emplace(key.str(), std::move(outcome));
        }
        std::printf("\n== %s ==\nunique (shader, combos, textures): %zu, compiled: %d, failed: %zu (%.1f%% ok)\n", title, unique.size(), ok,
                    unique.size() - static_cast<size_t>(ok), unique.empty() ? 0.0 : 100.0 * ok / static_cast<double>(unique.size()));
        std::map<std::string, int> causes;
        std::map<std::string, std::set<std::string>> shadersByCause;
        for (const auto &[key, outcome] : unique) {
            if (!outcome.ok) {
                const std::string cat = category(outcome.error);
                ++causes[cat];
                shadersByCause[cat].insert(shaderOf[key]);
            }
        }
        std::vector<std::pair<int, std::string>> ranked;
        for (const auto &[c, n] : causes) {
            ranked.emplace_back(n, c);
        }
        std::sort(ranked.rbegin(), ranked.rend());
        std::printf("top causes:\n");
        for (size_t i = 0; i < ranked.size() && i < 25; ++i) {
            std::string names;
            for (const std::string &s : shadersByCause[ranked[i].second]) {
                if (names.size() < 100) {
                    names += (names.empty() ? "" : ", ") + s;
                }
            }
            std::printf("  %3d  %s   [%s]\n", ranked[i].first, ranked[i].second.c_str(), names.c_str());
        }
        if (verbose) {
            for (const auto &[key, outcome] : unique) {
                if (!outcome.ok) {
                    std::printf("--- %s (item %s)\n%s\n", key.c_str(), firstWhere[key].c_str(), outcome.error.substr(0, 600).c_str());
                }
            }
        }
    };
    run(jobs, "shaders used by the library's scenes");
    if (dumpUniforms) {
        std::printf("\nuniforms without a material key (count of unique shader variants):\n");
        for (const auto &[name, n] : engineUniforms) {
            std::printf("  %4d  %s\n", n, name.c_str());
        }
    }
    if (!shipped.empty()) {
        run(shipped, "every shipped shader, default combos");
    }
    return 0;
}
