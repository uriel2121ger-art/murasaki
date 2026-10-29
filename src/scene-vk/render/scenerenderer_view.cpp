#include "render/scenerenderer.h"

#include "assets/texfile.h"
#include "assets/imagedecode.h"
#include "util/parallel.h"
#include "render/puppetdraw.h"

#include "core/framebuffers.h"
#include "render/gpuprofiler.h"
#include "render/shake.h"
#include "render/sprite.h"
#include "render/viewmapping.h"
#include "util/log.h"


#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <set>
#include <stdexcept>

namespace murasaki::scene {

namespace {

constexpr float kOrthographicEyeZ = 10000.0f; // g_EyePosition's z in an orthographic scene: far above the view's middle
constexpr double kCameraFadeSeconds = 0.4; // the camera fade's ramp at each end of a path (an assumption: formats.md, "Camera")

} // namespace

// The scene's sound objects and, when a shader reads it, the spectrum of what the system is playing.
// The part of the scene the view shows, in scene units: the window, moved by the camera object and zoomed about its centre. The
// shake is a movement of the picture on the screen (a share of the view), so a zoom does not enlarge it.
void SceneRenderer::viewBox(Vec2 &low, Vec2 &size) const
{
    size = {m_plan.windowSize.x / m_cameraZoom, m_plan.windowSize.y / m_cameraZoom};
    low = {m_plan.windowMin.x + m_plan.windowSize.x * 0.5f + m_cameraOffset.x + m_shake.x / m_cameraZoom - size.x * 0.5f,
           m_plan.windowMin.y + m_plan.windowSize.y * 0.5f + m_cameraOffset.y + m_shake.y / m_cameraZoom - size.y * 0.5f};
}

Mat4 SceneRenderer::screenProjection() const
{
    Vec2 low, size;
    viewBox(low, size);
    return orthographicGl(low.x, low.x + size.x, low.y, low.y + size.y, -10000.0f, 10000.0f);
}

Mat4 SceneRenderer::sceneProjection() const
{
    if (!m_scene.perspective) {
        return screenProjection() * m_view;
    }
    Vec2 low, size;
    viewBox(low, size);
    return perspectiveWindow(m_cameraFov, m_scene.orthoSize, low, size, m_scene.camera.nearZ, m_scene.camera.farZ) * m_view;
}

// The camera. In an orthographic scene the first visible camera object moves and zooms the view (its origin's x and y, its zoom);
// in a perspective one it is the camera itself: its world matrix places it, its fov and zoom frame the picture, and its paths
// (when it has any) move it, each in turn as its queue mode says. Without one, the scene's `camera` block (which scripts move
// with setCameraTransforms) is the camera. formats.md, "Camera".
void SceneRenderer::updateCameraObject(double time)
{
    Vec2 offset{0.0f, 0.0f};
    float zoom = 1.0f;
    CameraBasis basis = lookAtBasis(m_scene.camera.eye, m_scene.camera.center, m_scene.camera.up);
    float fov = resolve(Bound<float>{m_scene.camera.fov, m_scene.camera.fovUser}, m_props);
    bool pathPlaying = false;
    float fade = 0.0f;
    // how far the camera looks ahead: what a full parallax displacement moves the eye by (the block's and a path's eye to centre;
    // a camera object has no centre: one unit, what every camera block of the library has)
    float reach = camvec::length(camvec::sub(m_scene.camera.center, m_scene.camera.eye));
    for (size_t c = 0; c < m_scene.cameraObjects.size(); ++c) {
        const SceneCameraObject &camera = m_scene.cameraObjects[c];
        if (camera.node >= m_scene.nodes.size() || !nodeVisible(m_scene, camera.node, m_props)) {
            continue;
        }
        const float z = resolve(camera.zoom, m_props);
        zoom = std::isfinite(z) && z > 0.0f ? z : 1.0f;
        if (!m_scene.perspective) {
            const Mat4 world = nodeWorldMatrix(m_scene, camera.node);
            offset = {world.at(0, 3), world.at(1, 3)};
            if (!std::isfinite(offset.x) || !std::isfinite(offset.y)) {
                offset = {0.0f, 0.0f};
            }
            break;
        }
        fov = resolve(camera.fov, m_props);
        reach = 1.0f;
        if (c < m_cameraPaths.size()) {
            const auto [index, seconds] = m_cameraPaths[c].at(time);
            if (index >= 0) {
                pathPlaying = true;
                if (resolve(m_scene.camera.fade, m_props)) {
                    // black at the start and the end of every path, linear over kCameraFadeSeconds (at most a third of a short path)
                    const double length = camera.paths[static_cast<size_t>(index)].seconds();
                    const double ramp = std::max(1e-3, std::min(kCameraFadeSeconds, length / 3.0));
                    fade = static_cast<float>(1.0 - std::clamp(std::min(seconds, length - seconds) / ramp, 0.0, 1.0));
                }
                const CameraPathPose pose = sampleCameraPath(camera.paths[static_cast<size_t>(index)], seconds);
                if (pose.eye && pose.center) {
                    // the path moves the object (so what hangs on it follows): its origin and angles. The path is in world units;
                    // an object that hangs on another gets them in its parent's space (the parent's world matrix undone)
                    const CameraBasis moved = lookAtBasis(*pose.eye, *pose.center, pose.up.value_or(Vec3{0.0f, 1.0f, 0.0f}));
                    reach = camvec::length(camvec::sub(*pose.center, *pose.eye));
                    SceneNode &node = m_scene.nodes[camera.node];
                    const auto parent = node.parentId < 0 ? m_scene.nodeIndexById.end() : m_scene.nodeIndexById.find(node.parentId);
                    if (parent == m_scene.nodeIndexById.end()) {
                        node.origin = moved.eye;
                        node.angles = eulerFromBasis(moved);
                    } else {
                        const Mat4 local = inverse(nodeWorldMatrix(m_scene, parent->second)) * translation(moved.eye) * rotationOf(moved);
                        const CameraBasis inParent = basisFromWorld(local);
                        node.origin = inParent.eye;
                        node.angles = eulerFromBasis(inParent);
                    }
                }
                if (pose.fov) {
                    fov = *pose.fov;
                }
                if (pose.zoom && *pose.zoom > 0.0f) {
                    zoom = *pose.zoom;
                }
            }
        }
        basis = basisFromWorld(nodeWorldMatrix(m_scene, camera.node));
        break;
    }
    // the scene's own zoom (general.zoom, or a script's setCameraTransforms) on top of the camera object's
    if (const float general = resolve(m_scene.camera.zoom, m_props); std::isfinite(general) && general > 0.0f) {
        zoom *= general;
    }
    zoom = std::clamp(zoom, 0.01f, 100.0f);
    fov = clampFov(fov);
    const auto same = [](Vec3 a, Vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; };
    if (offset.x != m_cameraOffset.x || offset.y != m_cameraOffset.y || zoom != m_cameraZoom || fov != m_cameraFov || !same(basis.eye, m_cameraBasis.eye)
        || !same(basis.forward, m_cameraBasis.forward) || !same(basis.up, m_cameraBasis.up)) {
        m_cameraOffset = offset;
        m_cameraZoom = zoom;
        m_cameraFov = fov;
        m_cameraBasis = basis;
        m_dirty = true;
    }
    m_cameraReach = std::isfinite(reach) && reach > 0.0f ? reach : 1.0f;
    m_cameraPathPlaying = pathPlaying;
    m_cameraFadeLevel = fade;
}

// The view matrix and what shaders read of the camera (g_EyePosition, g_ViewForward / Right / Up). Perspective: the camera
// parallax moves the eye in its own plane, by the displacement (amount and mouse influence in it) times the camera's reach (its eye
// to centre distance; assumption, formats.md), and the depth of the scene does the rest. Orthographic: the camera block's direction
// turns the scene about its centre (the default, -z with y up, is no turn at all); the eye is above the middle of the view, 10000
// units up, so lit materials see the view along -z (the mesh branch's convention).
void SceneRenderer::updateView()
{
    CameraBasis basis = m_cameraBasis;
    if (m_scene.perspective) {
        basis.eye = camvec::add(basis.eye, camvec::mul(camvec::add(camvec::mul(basis.right, -m_parallax.x), camvec::mul(basis.up, -m_parallax.y)), m_cameraReach));
        m_view = viewMatrix(basis);
    } else {
        const bool turned = std::fabs(basis.forward.x) > 1e-6f || std::fabs(basis.forward.y) > 1e-6f || basis.forward.z > -1.0f + 1e-6f
            || std::fabs(basis.up.x) > 1e-6f || std::fabs(basis.up.z) > 1e-6f;
        if (turned) {
            const Vec3 centre{m_scene.orthoSize.x * 0.5f, m_scene.orthoSize.y * 0.5f, 0.0f};
            CameraBasis atCentre = basis;
            atCentre.eye = centre;
            m_view = translation(centre) * viewMatrix(atCentre);
        } else {
            m_view = Mat4::identity();
        }
        Vec2 low, size;
        viewBox(low, size);
        basis.eye = {low.x + size.x * 0.5f, low.y + size.y * 0.5f, kOrthographicEyeZ};
    }
    m_inputs.eye = basis.eye;
    m_inputs.viewForward = basis.forward;
    m_inputs.viewRight = basis.right;
    m_inputs.viewUp = basis.up;
}

} // namespace murasaki::scene
