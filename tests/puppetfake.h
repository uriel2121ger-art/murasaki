#pragma once

// A hand-made ModelBackend for the tests of the engine: its shapes, parameters and the function that fills a frame are given by the test. No model file is involved.

#include "puppet/model.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace puppetfake {

using namespace murasaki::puppet;

class FakeModel : public ModelBackend
{
public:
    using Evaluator = std::function<void(const ParameterSet &, const PartSet &, Frame &)>;

    std::string label = "fake";
    ParameterSet params;
    PartSet partSet;
    std::vector<Shape> shapes;
    std::vector<std::string> ids; // drawableId(i), when a test names the drawables
    Bounds extent{0, 0, 100, 100};
    Evaluator fill;
    std::atomic<int> evaluations{0};
    int failFrom = -1; // evaluate() refuses the values from this call on (-1: never)

    std::string name() const override { return label; }
    const ParameterSet &parameters() const override { return params; }
    const PartSet &parts() const override { return partSet; }
    size_t drawableCount() const override { return shapes.size(); }
    std::string drawableId(int drawable) const override { return drawable >= 0 && static_cast<size_t>(drawable) < ids.size() ? ids[static_cast<size_t>(drawable)] : std::string(); }
    const Shape &shape(int drawable) const override
    {
        static const Shape none;
        return drawable >= 0 && static_cast<size_t>(drawable) < shapes.size() ? shapes[static_cast<size_t>(drawable)] : none;
    }
    Bounds bounds() const override { return extent; }
    bool evaluate(const ParameterSet &values, const PartSet &opacities, Frame &out) override
    {
        if (values.size() != params.size())
            return false;
        if (failFrom >= 0 && evaluations.load() >= failFrom)
            return false;
        ++evaluations;
        out.items.clear();
        out.maskSources.clear();
        if (fill)
            fill(values, opacities, out);
        return true;
    }

    // A shape of a rectangle (two triangles) with uvs over the whole texture.
    static Shape quadShape()
    {
        Shape s;
        s.uvs = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        s.indices = {0, 1, 2, 0, 2, 3};
        return s;
    }
    static std::vector<Vec2> rect(double x, double y, double w, double h) { return {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}}; }

    // A mesh item for drawable `drawable` with the given rectangle.
    static DrawItem mesh(int drawable, double x, double y, double w, double h, int texture = 0)
    {
        DrawItem item;
        item.kind = DrawKind::Mesh;
        item.drawable = drawable;
        item.positions = rect(x, y, w, h);
        item.texture = texture;
        return item;
    }
};

} // namespace puppetfake
