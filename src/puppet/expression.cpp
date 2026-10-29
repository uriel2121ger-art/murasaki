#include "puppet/expression.h"

#include "puppetlimits.h"
#include "puppet/strictjson.h"

#include <algorithm>
#include <cmath>

namespace murasaki::puppet {

namespace {

ExpressionResult fail(std::string why)
{
    ExpressionResult r;
    r.error = std::move(why);
    return r;
}

} // namespace

ExpressionResult parseExpression(std::string_view text)
{
    const json::ParseResult parsed = json::parse(text, {kMaxJsonDepth, kMaxJsonValues, kMaxJsonStringBytes});
    if (!parsed.ok)
        return fail("not strict JSON: " + parsed.error);
    const json::Value &root = parsed.value;
    if (!root.isObject())
        return fail("the top level is not an object");
    if (const json::Value *version = root.find("Version"); version && (!version->isNumber() || version->number != 3))
        return fail("Version is not 3");
    const json::Value *type = root.find("Type");
    if (!type || !type->isString() || type->text != "Live2D Expression")
        return fail("Type is not Live2D Expression");
    ExpressionResult result;
    Expression &e = result.expression;
    for (const auto &[key, slot] : {std::pair<const char *, double *>{"FadeInTime", &e.fadeIn}, {"FadeOutTime", &e.fadeOut}}) {
        if (const json::Value *v = root.find(key)) {
            if (!v->isNumber() || v->number < 0 || v->number > static_cast<double>(kMaxFadeSeconds))
                return fail(std::string(key) + " is not a time from 0 to kMaxFadeSeconds");
            *slot = v->number;
        }
    }
    const json::Value *parameters = root.find("Parameters");
    if (!parameters || !parameters->isArray())
        return fail("Parameters is missing");
    if (static_cast<std::int64_t>(parameters->size()) > kMaxParameters)
        return fail("too many parameters (kMaxParameters)");
    if (!parameters->numbers.empty())
        return fail("a parameter is not an object");
    for (const json::Value &item : parameters->items) {
        if (!item.isObject())
            return fail("a parameter is not an object");
        ExpressionTarget target;
        const json::Value *id = item.find("Id");
        if (!id || !id->isString() || id->text.empty() || static_cast<std::int64_t>(id->text.size()) > kMaxIdBytes)
            return fail("a parameter has no usable Id");
        target.id = id->text;
        const json::Value *value = item.find("Value");
        if (!value || !value->isNumber())
            return fail("a parameter has no Value");
        target.value = value->number;
        if (const json::Value *blend = item.find("Blend")) {
            if (!blend->isString())
                return fail("a Blend is not a string");
            if (blend->text == "Add")
                target.blend = Blend::Add;
            else if (blend->text == "Multiply")
                target.blend = Blend::Multiply;
            else if (blend->text == "Overwrite")
                target.blend = Blend::Overwrite;
            else
                return fail("a Blend is not Add, Multiply or Overwrite");
        }
        e.targets.push_back(std::move(target));
    }
    result.ok = true;
    return result;
}

double ExpressionManager::Layer::weight() const
{
    const Expression &e = *expression;
    double w = e.fadeIn > 0 ? fadeEase(age / e.fadeIn) : 1.0;
    if (fadingOut)
        w *= e.fadeOut > 0 ? fadeEase(1.0 - outAge / e.fadeOut) : 0.0;
    return w;
}

void ExpressionManager::set(std::shared_ptr<const Expression> expression)
{
    for (Layer &l : m_layers)
        l.fadingOut = true;
    if (expression) {
        m_layers.push_back({std::move(expression), 0, false, 0});
        if (m_layers.size() > kMaxExpressionLayers)
            m_layers.erase(m_layers.begin());
    }
}

void ExpressionManager::update(double dt)
{
    if (!std::isfinite(dt) || dt < 0)
        return;
    for (Layer &l : m_layers) {
        l.age += dt;
        if (l.fadingOut)
            l.outAge += dt;
    }
    m_layers.erase(std::remove_if(m_layers.begin(), m_layers.end(),
                                  [](const Layer &l) { return l.fadingOut && l.outAge >= l.expression->fadeOut; }),
                   m_layers.end());
}

void ExpressionManager::apply(ParameterSet &parameters) const
{
    for (const Layer &layer : m_layers) {
        const double w = layer.weight();
        if (w <= 0)
            continue;
        for (const ExpressionTarget &t : layer.expression->targets) {
            const int index = parameters.find(t.id);
            if (index < 0)
                continue;
            const double cur = parameters.value(index);
            switch (t.blend) {
            case Blend::Add: parameters.set(index, cur + t.value * w); break;
            case Blend::Multiply: parameters.set(index, cur * (1 + (t.value - 1) * w)); break;
            case Blend::Overwrite: parameters.set(index, cur + (t.value - cur) * w); break;
            }
        }
    }
}

} // namespace murasaki::puppet
