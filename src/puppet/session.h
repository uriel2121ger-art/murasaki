#pragma once

#include "puppet/expression.h"
#include "puppet/lookat.h"
#include "puppet/model.h"
#include "puppet/motion.h"
#include "puppet/params.h"
#include "puppet/physics.h"
#include "puppet/pose.h"
#include "puppet/view.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace murasaki::puppet {

// What the engine process holds between the host's messages and the renderer (docs/pets/puppets.md, section 19): the model, the values the host set, and the small amounts of life the engine adds
// on its own (breathing and blinking, on by default; looking at the pointer, off until the host turns it on). Pure: no GPU, no clock (the engine passes the elapsed time), no bridge.
//
// The host's commands arrive as the JSON of a bridge Properties message:
//   {"set": {"<parameter id>": number, ...}, "reset": true, "look": bool, "breath": bool, "blink": bool, "part": {"<part id>": number},
//    "play": {"group": "<motion group>", "index": n (optional: one at random, from the seed), "priority": "idle"|"normal"|"force" (default normal)}, "stop": true,
//    "expression": "<name>" | null (null: fade the current ones out),
//    "click": {"u": 0..1, "v": 0..1, "fallback": ["<motion group>", ...]} }  (a click on the picture, as fractions of it; see applyClick)
// Motions, expressions, physics and pose act on the values the host set, in that order (motions, expressions, then physics and pose), after the life (breathing, blinking, gaze). A model without assets
// has none of them and behaves as before.
// Unknown keys are ignored; a value of the wrong type, an unknown parameter or part, or a document that is not strict JSON is refused (and named in `error`), changing nothing.
// What a model brings besides its shapes (read by the engine from the item folder, never from the host's messages): motions by group, expressions by name, the physics rigs and the pose groups. All optional.
struct MotionAsset {
    std::shared_ptr<const Motion> motion;
    double fadeIn = -1;  // seconds; negative: the file's, then the default
    double fadeOut = -1;
};

// A hit area of the model (Cubism): the drawable it is the area of, by id, and its name - the motion group a click on it plays.
struct HitAreaAsset {
    std::string drawableId;
    std::string name;
};

struct SessionAssets {
    std::vector<HitAreaAsset> hitAreas;
    std::map<std::string, std::vector<MotionAsset>> motionGroups;
    std::map<std::string, std::shared_ptr<const Expression>> expressions;
    std::optional<PhysicsSettings> physics;
    std::optional<Pose> pose;
};

class PuppetSession
{
public:
    PuppetSession(std::unique_ptr<ModelBackend> model, std::uint64_t seed, SessionAssets assets = {});

    ModelBackend &model() { return *m_model; }
    const ModelBackend &model() const { return *m_model; }

    bool command(std::string_view json, std::string &error);
    // The pointer is at (x, y) pixels of a width x height picture (top-left origin). Used only while looking is on.
    void pointer(double x, double y, uint32_t width, uint32_t height);
    void setLook(bool on);
    void setBreath(bool on) { m_breathOn = on; }
    void setBlink(bool on) { m_blinkOn = on; }
    bool looking() const { return m_look; }
    // A click that command() accepted and that has not been applied: the engine, which knows the picture, applies it (applyClick) right after the command.
    struct ClickRequest {
        double u = 0;
        double v = 0;
        std::vector<std::string> fallback;
    };
    std::optional<ClickRequest> takeClick();
    // Plays the motion group of the hit area the click is on - a hit area is tested where its drawable is drawn (`shown`, laid out by `view` on a width x height picture), whatever is drawn over it, as Cubism does -
    // else the first of the fallback groups the model has, else nothing. Returns what it did: "area:<name>", "fallback:<group>" or "" (and the same in lastClick()).
    std::string applyClick(const ClickRequest &click, const Frame &shown, const ViewTransform &view, uint32_t width, uint32_t height);
    const std::string &lastClick() const { return m_lastClick; }
    // What the model brings: counts for the check and the tests.
    size_t motionGroupCount() const { return m_assets.motionGroups.size(); }
    size_t expressionCount() const { return m_assets.expressions.size(); }
    bool hasPhysics() const { return m_physics != nullptr; }
    const std::vector<std::string> &physicsSkipped() const;
    bool motionPlaying() const { return m_motions.playing(); }
    const std::string &motionTag() const { return m_motions.currentTag(); }
    bool hasMotionGroup(const std::string &name) const { return m_assets.motionGroups.count(name) != 0; }
    bool hasExpression(const std::string &name) const { return m_assets.expressions.count(name) != 0; }

    // Advances the life by dt seconds (a negative or non-finite step is ignored; a long one is cut to kMaxStep).
    void step(double dt);

    // Evaluates the model with the host's values plus the life, into `out`. `changed` is false when the result is certain to equal the previous evaluation's (the same inputs): the caller may keep
    // the picture it has. False return: the model refused the values.
    bool evaluate(Frame &out, bool &changed);

    // Whether anything the session does on its own can still change the picture (breath or blink on a model that has their parameters, or the gaze still moving).
    bool animated() const;

    static constexpr double kMaxStep = 0.25;

private:
    bool playRandom(const std::string &group); // a random motion of the group, at normal priority; false when the model has no such group
    std::unique_ptr<ModelBackend> m_model;
    ParameterSet m_host;      // what the host set
    PartSet m_hostParts;
    ParameterSet m_frame;     // host + life
    Breath m_breath;
    AutoBlink m_blink;
    LookAt m_lookAt;
    bool m_look = false;
    bool m_breathOn = true;
    bool m_blinkOn = true;
    std::vector<double> m_last;
    std::vector<double> m_lastParts;
    bool m_evaluated = false;
    bool m_hasBreath = false;
    bool m_hasBlink = false;
    Vec2 m_lookTarget;
    SessionAssets m_assets;
    std::optional<ClickRequest> m_click;
    std::string m_lastClick;
    MotionManager m_motions;
    ExpressionManager m_expressions;
    std::unique_ptr<Physics> m_physics;
    std::unique_ptr<PoseState> m_pose;
    Rng m_rng;
    PartSet m_frameParts;      // host + motions + pose
    double m_pendingDt = 0;    // time since the physics and the pose last ran
    double m_tail = 0;         // seconds left in which a motion, an expression or the pose may still be changing the picture
    double m_physicsAwake = 0; // seconds left in which the rigs may still be swinging
};

} // namespace murasaki::puppet
