#pragma once

#include "puppet/motion.h"
#include "puppet/params.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace murasaki::puppet {

// Expressions (docs/pets/puppets.md, section 3.4): a set of parameter targets, each with a blend (Add, Multiply or Overwrite), faded in when the expression is chosen and out when
// another replaces it. Several can be active while one fades out. The specification does not say the default blend or the fade defaults (Q-F4): ours are Add and one second.
enum class Blend { Add, Multiply, Overwrite };

struct ExpressionTarget {
    std::string id;
    double value = 0;
    Blend blend = Blend::Add;
};

struct Expression {
    double fadeIn = kDefaultFadeSeconds;
    double fadeOut = kDefaultFadeSeconds;
    std::vector<ExpressionTarget> targets;
};

struct ExpressionResult {
    bool ok = false;
    Expression expression;
    std::string error;
};

// Reads a `*.exp3.json`: Type "Live2D Expression", Version 3 or absent, fades from 0 up, targets with finite values, at most kMaxParameters of them.
ExpressionResult parseExpression(std::string_view text);

class ExpressionManager
{
public:
    // Makes `expression` the current one; the previous ones start fading out. A null expression only fades the others out. At most kMaxExpressionLayers are kept (the oldest goes).
    void set(std::shared_ptr<const Expression> expression);
    void update(double dt);
    // Applies every active expression, oldest first, to the parameters: Add moves the value by target x weight; Multiply scales it by 1 + (target - 1) x weight; Overwrite moves it
    // toward the target by the weight.
    void apply(ParameterSet &parameters) const;
    size_t active() const { return m_layers.size(); }

    static constexpr size_t kMaxExpressionLayers = 8;

private:
    struct Layer {
        std::shared_ptr<const Expression> expression;
        double age = 0;
        bool fadingOut = false;
        double outAge = 0;
        double weight() const;
    };
    std::vector<Layer> m_layers;
};

} // namespace murasaki::puppet
