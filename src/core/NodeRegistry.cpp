#include "core/NodeRegistry.h"

#include <algorithm>

namespace mf {
namespace {

PinDefinition In(std::string name, PinType type, PropertyValue defaultValue = {}, std::string tooltip = {}) {
    PinDefinition pin;
    pin.name = std::move(name);
    pin.type = type;
    pin.defaultValue = std::move(defaultValue);
    pin.tooltip = std::move(tooltip);
    return pin;
}

PinDefinition Out(std::string name, PinType type, PropertyValue defaultValue = {}, std::string tooltip = {}) {
    PinDefinition pin;
    pin.name = std::move(name);
    pin.type = type;
    pin.defaultValue = std::move(defaultValue);
    pin.tooltip = std::move(tooltip);
    return pin;
}

PropertySpec PropFloat(std::string key, std::string label, float value, float speed = 0.01f,
                       bool range = false, float min = 0.0f, float max = 1.0f,
                       std::string tooltip = {}) {
    PropertySpec spec;
    spec.key = std::move(key);
    spec.label = std::move(label);
    spec.defaultValue = PropertyValue::Float(value);
    spec.dragSpeed = speed;
    spec.hasRange = range;
    spec.minValue = min;
    spec.maxValue = max;
    spec.tooltip = std::move(tooltip);
    return spec;
}

PropertySpec PropVec2(std::string key, std::string label, Vec2 value, float speed = 0.01f,
                      std::string tooltip = {}) {
    PropertySpec spec;
    spec.key = std::move(key);
    spec.label = std::move(label);
    spec.defaultValue = PropertyValue::Vec2Value(value.x, value.y);
    spec.dragSpeed = speed;
    spec.tooltip = std::move(tooltip);
    return spec;
}

PropertySpec PropColor3(std::string key, std::string label, Vec3 value, std::string tooltip = {}) {
    PropertySpec spec;
    spec.key = std::move(key);
    spec.label = std::move(label);
    spec.defaultValue = PropertyValue::Color3Value(value.x, value.y, value.z);
    spec.tooltip = std::move(tooltip);
    return spec;
}

PropertySpec PropColor4(std::string key, std::string label, Vec4 value, std::string tooltip = {}) {
    PropertySpec spec;
    spec.key = std::move(key);
    spec.label = std::move(label);
    spec.defaultValue = PropertyValue::Color4Value(value.x, value.y, value.z, value.w);
    spec.tooltip = std::move(tooltip);
    return spec;
}

PropertySpec PropString(std::string key, std::string label, std::string value, std::string tooltip = {}) {
    PropertySpec spec;
    spec.key = std::move(key);
    spec.label = std::move(label);
    spec.defaultValue = PropertyValue::StringValue(std::move(value));
    spec.tooltip = std::move(tooltip);
    return spec;
}

PropertySpec PropBool(std::string key, std::string label, bool value, std::string tooltip = {}) {
    PropertySpec spec;
    spec.key = std::move(key);
    spec.label = std::move(label);
    spec.defaultValue = PropertyValue::BoolValue(value);
    spec.tooltip = std::move(tooltip);
    return spec;
}

PropertySpec PropAsset(std::string key, std::string label, std::string value, std::string tooltip = {}) {
    PropertySpec spec;
    spec.key = std::move(key);
    spec.label = std::move(label);
    spec.defaultValue = PropertyValue::AssetPathValue(std::move(value));
    spec.tooltip = std::move(tooltip);
    return spec;
}

std::vector<PropertySpec> SamplerProperties() {
    return {
        PropString("filter", "Filter", "Linear",
                   "Point, Linear, Trilinear or Anisotropic filtering."),
        PropString("addressU", "Address U", "Repeat",
                   "Repeat, Clamp or Mirror addressing on U."),
        PropString("addressV", "Address V", "Repeat",
                   "Repeat, Clamp or Mirror addressing on V."),
        PropBool("srgb", "sRGB", true,
                 "Decode the texture as sRGB before sampling."),
        PropFloat("mipBias", "Mip Bias", 0.0f, 0.01f, false, 0.0f, 0.0f,
                  "Positive values select blurrier mip levels."),
        PropFloat("maxAnisotropy", "Max Anisotropy", 8.0f, 0.1f, true, 1.0f, 16.0f,
                  "Maximum anisotropy used by the Anisotropic filter."),
        PropString("textureGroup", "Texture Group", "World",
                   "UE texture group metadata used by the export bridge.")
    };
}

std::vector<PropertySpec> WithSamplerProperties(std::vector<PropertySpec> base) {
    std::vector<PropertySpec> sampler = SamplerProperties();
    base.insert(base.end(), std::make_move_iterator(sampler.begin()),
                std::make_move_iterator(sampler.end()));
    return base;
}

NodeDefinition MakeBinaryMath(const std::string& type, const std::string& description,
                              float defaultA, float defaultB) {
    NodeDefinition def;
    def.type = type;
    def.displayName = type;
    def.category = "Math";
    def.description = description;
    def.ueClass = "MaterialExpression" + type;
    def.typeRule = PinTypeRule::PromoteInputs;
    def.inputs = {
        In("A", PinType::Wildcard, PropertyValue::Float(defaultA), "Left operand."),
        In("B", PinType::Wildcard, PropertyValue::Float(defaultB), "Right operand.")
    };
    def.outputs = {Out("Result", PinType::Wildcard)};
    return def;
}

NodeDefinition MakeUnaryMath(const std::string& type, const std::string& description,
                             PinTypeRule rule = PinTypeRule::MatchFirstInput) {
    NodeDefinition def;
    def.type = type;
    def.displayName = type;
    def.category = "Math";
    def.description = description;
    def.ueClass = "MaterialExpression" + type;
    def.typeRule = rule;
    def.inputs = {In("Input", PinType::Wildcard, PropertyValue::Float(0.0f))};
    def.outputs = {Out("Result", PinType::Wildcard)};
    return def;
}

NodeDefinition MakeComparison(const std::string& type, const std::string& description) {
    NodeDefinition def;
    def.type = type;
    def.displayName = type;
    def.category = "Math";
    def.description = description;
    def.ueClass = "MaterialExpression" + type;
    def.inputs = {
        In("A", PinType::Float, PropertyValue::Float(0.0f), "Left operand."),
        In("B", PinType::Float, PropertyValue::Float(0.0f), "Right operand.")
    };
    def.outputs = {Out("Result", PinType::Float,
                       PropertyValue::Float(0.0f),
                       "1 when the comparison is true, otherwise 0.")};
    return def;
}

NodeDefinition MakeConstant(const std::string& type, const std::string& category,
                            std::vector<PinDefinition> outputs, std::vector<PropertySpec> properties,
                            const std::string& description) {
    NodeDefinition def;
    def.type = type;
    def.displayName = type;
    def.category = category;
    def.description = description;
    def.ueClass = "MaterialExpression" + type;
    def.outputs = std::move(outputs);
    def.properties = std::move(properties);
    return def;
}

} // namespace

const NodeRegistry& NodeRegistry::Get() {
    static NodeRegistry registry;
    return registry;
}

NodeRegistry::NodeRegistry() {
    // ------------------------------------------------------------------
    // Output
    // ------------------------------------------------------------------
    {
        NodeDefinition def;
        def.type = "MaterialOutput";
        def.displayName = "Material Output";
        def.category = "Output";
        def.description = "Final material attributes used by the GLSL preview and by the UE5 import.";
        def.ueClass = "Material";
        def.isMaterialOutput = true;
        def.inputs = {
            In("Base Color", PinType::Vector3, PropertyValue::Color3Value(0.18f, 0.18f, 0.18f)),
            In("Metallic", PinType::Float, PropertyValue::Float(0.0f)),
            In("Specular", PinType::Float, PropertyValue::Float(0.5f)),
            In("Roughness", PinType::Float, PropertyValue::Float(0.5f)),
            In("Emissive Color", PinType::Vector3, PropertyValue::Color3Value(0.0f, 0.0f, 0.0f)),
            In("Normal", PinType::Vector3, PropertyValue::Vec3Value(0.0f, 0.0f, 1.0f)),
            In("Ambient Occlusion", PinType::Float, PropertyValue::Float(1.0f)),
            In("Opacity", PinType::Float, PropertyValue::Float(1.0f)),
            In("Opacity Mask", PinType::Float, PropertyValue::Float(1.0f)),
            In("World Position Offset", PinType::Vector3,
               PropertyValue::Vec3Value(0.0f, 0.0f, 0.0f)),
            In("Pixel Depth Offset", PinType::Float, PropertyValue::Float(0.0f)),
            In("Refraction", PinType::Float, PropertyValue::Float(1.0f)),
            In("Clear Coat", PinType::Float, PropertyValue::Float(0.0f)),
            In("Clear Coat Roughness", PinType::Float, PropertyValue::Float(0.3f)),
            In("Subsurface Color", PinType::Vector3,
               PropertyValue::Color3Value(0.72f, 0.42f, 0.32f)),
            In("Anisotropy", PinType::Float, PropertyValue::Float(0.0f))
        };
        Register(std::move(def));
    }

    // ------------------------------------------------------------------
    // Constants and parameters
    // ------------------------------------------------------------------
    Register(MakeConstant("Constant", "Constants",
                          {Out("Value", PinType::Float)},
                          {PropFloat("value", "Value", 0.5f, 0.01f, false, 0.0f, 1.0f,
                                     "Scalar value baked into the generated shader.")},
                          "A single floating-point constant."));

    Register(MakeConstant("Constant2Vector", "Constants",
                          {Out("Value", PinType::Vector2)},
                          {PropVec2("value", "Value", Vec2(0.0f, 0.0f),
                                    0.01f, "Two-component constant vector.")},
                          "A two-component constant vector."));

    Register(MakeConstant("Constant3Vector", "Constants",
                          {Out("Value", PinType::Vector3)},
                          {PropColor3("value", "Value", Vec3(0.0f, 0.0f, 0.0f),
                                      "Three-component constant, usually edited as a color.")},
                          "A three-component constant vector / color."));

    Register(MakeConstant("Constant4Vector", "Constants",
                          {Out("Value", PinType::Vector4)},
                          {PropColor4("value", "Value", Vec4(0.0f, 0.0f, 0.0f, 1.0f),
                                      "Four-component constant vector / color.")},
                          "A four-component constant vector / color."));

    {
        NodeDefinition def = MakeConstant("ScalarParameter", "Parameters",
                                          {Out("Value", PinType::Float)},
                                          {PropString("parameterName", "Parameter Name", "Roughness",
                                                      "Name of the scalar parameter exposed in UE5."),
                                           PropFloat("value", "Default Value", 0.5f),
                                           PropString("parameterGroup", "Group", "Default",
                                                      "Material Instance parameter group."),
                                           PropFloat("sortPriority", "Sort Priority", 0.0f, 1.0f,
                                                     true, -1000.0f, 1000.0f,
                                                     "Lower values are shown first inside a group."),
                                           PropString("description", "Description", "",
                                                      "Tooltip shown for this parameter in Material Instances."),
                                           PropFloat("sliderMin", "Slider Min", 0.0f, 0.01f,
                                                     false, 0.0f, 0.0f,
                                                     "Minimum value used by the Material Instance slider."),
                                           PropFloat("sliderMax", "Slider Max", 1.0f, 0.01f,
                                                     false, 0.0f, 0.0f,
                                                     "Maximum value used by the Material Instance slider.")},
                                          "A scalar material parameter. Emitted as a uniform and exported as a UE5 ScalarParameter.");
        def.ueClass = "MaterialExpressionScalarParameter";
        Register(std::move(def));
    }
    {
        NodeDefinition def = MakeConstant("VectorParameter", "Parameters",
                                          {Out("Value", PinType::Vector4)},
                                          {PropString("parameterName", "Parameter Name", "Tint",
                                                      "Name of the vector parameter exposed in UE5."),
                                           PropColor4("value", "Default Value",
                                                      Vec4(0.2f, 0.8f, 1.0f, 1.0f)),
                                           PropString("parameterGroup", "Group", "Default",
                                                      "Material Instance parameter group."),
                                           PropFloat("sortPriority", "Sort Priority", 0.0f, 1.0f,
                                                     true, -1000.0f, 1000.0f,
                                                     "Lower values are shown first inside a group."),
                                           PropString("description", "Description", "",
                                                      "Tooltip shown for this parameter in Material Instances.")},
                                          "A vector material parameter. Emitted as a uniform and exported as a UE5 VectorParameter.");
        def.ueClass = "MaterialExpressionVectorParameter";
        Register(std::move(def));
    }

    // ------------------------------------------------------------------
    // Binary math
    // ------------------------------------------------------------------
    Register(MakeBinaryMath("Add", "Adds two values. Numeric types are promoted.",
                            0.0f, 0.0f));
    Register(MakeBinaryMath("Subtract", "Subtracts B from A. Numeric types are promoted.",
                            0.0f, 0.0f));
    Register(MakeBinaryMath("Multiply", "Multiplies two values. Numeric types are promoted.",
                            1.0f, 1.0f));
    Register(MakeBinaryMath("Divide", "Divides A by B. Numeric types are promoted.",
                            1.0f, 1.0f));
    Register(MakeBinaryMath("Power", "Raises A to the power of B.",
                            1.0f, 2.0f));
    Register(MakeBinaryMath("Min", "Component-wise minimum of A and B.", 0.0f, 1.0f));
    Register(MakeBinaryMath("Max", "Component-wise maximum of A and B.", 0.0f, 1.0f));
    Register(MakeBinaryMath("Fmod", "Component-wise floating-point remainder.", 1.0f, 1.0f));

    // ------------------------------------------------------------------
    // Unary / other math
    // ------------------------------------------------------------------
    Register(MakeUnaryMath("Abs", "Absolute value of each component."));
    Register(MakeUnaryMath("Ceil", "Smallest integer not less than the input."));
    Register(MakeUnaryMath("Floor", "Largest integer not greater than the input."));
    Register(MakeUnaryMath("Frac", "Fractional part of each component."));
    Register(MakeUnaryMath("Round", "Rounds each component to the nearest integer."));
    Register(MakeUnaryMath("Sqrt", "Square root of each component."));
    Register(MakeUnaryMath("OneMinus", "Returns 1 - Input for every component."));
    Register(MakeUnaryMath("Sine", "Sine of each component."));
    Register(MakeUnaryMath("Cosine", "Cosine of each component."));
    Register(MakeUnaryMath("Tangent", "Tangent of each component."));
    Register(MakeUnaryMath("Saturate", "Clamps each component to the 0..1 range."));

    {
        NodeDefinition def;
        def.type = "Reroute";
        def.displayName = "Reroute";
        def.category = "Utility";
        def.description = "A compact pass-through node for routing a connection.";
        def.ueClass = "MaterialExpressionReroute";
        def.typeRule = PinTypeRule::MatchFirstInput;
        def.inputs = {In("Input", PinType::Wildcard, PropertyValue::Float(0.0f))};
        def.outputs = {Out("Output", PinType::Wildcard)};
        Register(std::move(def));
    }
    {
        NodeDefinition def;
        def.type = "Comment";
        def.displayName = "Comment";
        def.category = "Utility";
        def.description = "A resizable comment box for organizing the graph.";
        def.ueClass = "";
        def.isComment = true;
        def.properties = {
            PropString("text", "Text", "Comment", "Comment text shown in the graph."),
            PropColor3("color", "Color", Vec3(0.18f, 0.34f, 0.42f),
                       "Accent color of the comment title.")
        };
        Register(std::move(def));
    }

    Register(MakeUnaryMath("Sign", "Returns -1, 0 or 1 for each component."));
    Register(MakeUnaryMath("Truncate", "Discards the fractional part of each component."));
    Register(MakeUnaryMath("Logarithm2", "Base-2 logarithm of each component."));
    Register(MakeUnaryMath("Arcsine", "Inverse sine of each component."));
    Register(MakeUnaryMath("Arccosine", "Inverse cosine of each component."));
    Register(MakeUnaryMath("Arctangent", "Inverse tangent of each component."));

    Register(MakeComparison("Greater", "Returns 1 when A is greater than B."));
    Register(MakeComparison("GreaterEqual", "Returns 1 when A is greater than or equal to B."));
    Register(MakeComparison("Less", "Returns 1 when A is less than B."));
    Register(MakeComparison("LessEqual", "Returns 1 when A is less than or equal to B."));
    Register(MakeComparison("Equal", "Returns 1 when A equals B (within tolerance)."));
    Register(MakeComparison("NotEqual", "Returns 1 when A does not equal B."));

    {
        NodeDefinition def;
        def.type = "If";
        def.displayName = "If";
        def.category = "Math";
        def.description =
            "Selects between the A > B, A == B and A < B values. Equivalent to "
            "MaterialExpressionIf.";
        def.ueClass = "MaterialExpressionIf";
        def.typeRule = PinTypeRule::PromoteInputs;
        def.inputs = {
            In("A", PinType::Float, PropertyValue::Float(0.0f)),
            In("B", PinType::Float, PropertyValue::Float(0.0f)),
            In("A > B", PinType::Wildcard, PropertyValue::Float(1.0f)),
            In("A == B", PinType::Wildcard, PropertyValue::Float(0.0f)),
            In("A < B", PinType::Wildcard, PropertyValue::Float(0.0f))
        };
        def.outputs = {Out("Result", PinType::Wildcard)};
        Register(std::move(def));
    }
    {
        NodeDefinition def;
        def.type = "Switch";
        def.displayName = "Switch";
        def.category = "Math";
        def.description = "Chooses A when Pick is greater than 0.5, otherwise B.";
        def.ueClass = "MaterialExpressionSwitch";
        def.typeRule = PinTypeRule::PromoteInputs;
        def.inputs = {
            In("Pick", PinType::Float, PropertyValue::Float(0.0f)),
            In("A", PinType::Wildcard, PropertyValue::Float(1.0f)),
            In("B", PinType::Wildcard, PropertyValue::Float(0.0f))
        };
        def.outputs = {Out("Result", PinType::Wildcard)};
        Register(std::move(def));
    }
    {
        NodeDefinition def;
        def.type = "StaticSwitchParameter";
        def.displayName = "StaticSwitchParameter";
        def.category = "Utility";
        def.description = "Selects True or False at compile time. Also exported as a static switch.";
        def.ueClass = "MaterialExpressionStaticSwitchParameter";
        def.typeRule = PinTypeRule::PromoteInputs;
        def.inputs = {
            In("True", PinType::Wildcard, PropertyValue::Float(1.0f)),
            In("False", PinType::Wildcard, PropertyValue::Float(0.0f))
        };
        def.outputs = {Out("Result", PinType::Wildcard)};
        def.properties = {
            PropBool("value", "Default Value", true,
                     "Compile-time selection used by the preview and exported to UE5.")
        };
        Register(std::move(def));
    }
    {
        NodeDefinition def;
        def.type = "RotateAboutAxis";
        def.displayName = "RotateAboutAxis";
        def.category = "Math";
        def.description = "Rotates a position around an axis by Angle radians.";
        def.ueClass = "MaterialExpressionRotateAboutAxis";
        def.inputs = {
            In("Position", PinType::Vector3, PropertyValue::Vec3Value(0.0f, 0.0f, 0.0f)),
            In("Axis", PinType::Vector3, PropertyValue::Vec3Value(0.0f, 0.0f, 1.0f)),
            In("Angle", PinType::Float, PropertyValue::Float(0.0f))
        };
        def.outputs = {Out("Result", PinType::Vector3)};
        Register(std::move(def));
    }
    {
        NodeDefinition def;
        def.type = "AppendVector";
        def.displayName = "AppendVector";
        def.category = "Math";
        def.description = "Appends the components of A and B into a larger vector.";
        def.ueClass = "MaterialExpressionAppendVector";
        def.typeRule = PinTypeRule::AppendInputs;
        def.inputs = {
            In("A", PinType::Wildcard, PropertyValue::Float(0.0f)),
            In("B", PinType::Wildcard, PropertyValue::Float(0.0f))
        };
        def.outputs = {Out("Result", PinType::Wildcard)};
        Register(std::move(def));
    }
    {
        NodeDefinition def;
        def.type = "ComponentMask";
        def.displayName = "ComponentMask";
        def.category = "Math";
        def.description =
            "Selects R/G/B/A channels from the input. The output type follows the "
            "number of enabled channels.";
        def.ueClass = "MaterialExpressionComponentMask";
        def.typeRule = PinTypeRule::ComponentMask;
        def.inputs = {
            In("Input", PinType::Wildcard, PropertyValue::Vec4Value(0.0f, 0.0f, 0.0f, 1.0f))
        };
        def.outputs = {Out("Result", PinType::Wildcard)};
        def.properties = {
            PropBool("r", "R", true, "Enable the red channel."),
            PropBool("g", "G", true, "Enable the green channel."),
            PropBool("b", "B", true, "Enable the blue channel."),
            PropBool("a", "A", true, "Enable the alpha channel.")
        };
        Register(std::move(def));
    }
    auto registerBreakVector = [](const char* type, PinType inputType, bool hasB, bool hasA) {
        NodeDefinition def;
        def.type = type;
        def.displayName = type;
        def.category = "Math";
        def.description = std::string("Splits a ") + PinTypeToString(inputType) +
                          " into its scalar components.";
        def.ueClass = std::string("MaterialExpression") + type;
        PropertyValue defaultValue = PropertyValue::Vec2Value(0.0f, 0.0f);
        if (inputType == PinType::Vector3) {
            defaultValue = PropertyValue::Vec3Value(0.0f, 0.0f, 0.0f);
        } else if (inputType == PinType::Vector4) {
            defaultValue = PropertyValue::Vec4Value(0.0f, 0.0f, 0.0f, 0.0f);
        }
        def.inputs = {In("Input", inputType, defaultValue)};
        def.outputs = {Out("R", PinType::Float), Out("G", PinType::Float)};
        if (hasB) def.outputs.push_back(Out("B", PinType::Float));
        if (hasA) def.outputs.push_back(Out("A", PinType::Float));
        return def;
    };
    Register(registerBreakVector("BreakVector2", PinType::Vector2, false, false));
    Register(registerBreakVector("BreakVector3", PinType::Vector3, true, false));
    Register(registerBreakVector("BreakVector4", PinType::Vector4, true, true));
    {
        NodeDefinition def;
        def.type = "Length";
        def.displayName = "Length";
        def.category = "Math";
        def.description = "Length (magnitude) of a scalar or vector.";
        def.ueClass = "MaterialExpressionLength";
        def.inputs = {In("Input", PinType::Wildcard,
                         PropertyValue::Vec3Value(0.0f, 0.0f, 0.0f))};
        def.outputs = {Out("Result", PinType::Float)};
        Register(std::move(def));
    }
    {
        NodeDefinition def;
        def.type = "Distance";
        def.displayName = "Distance";
        def.category = "Math";
        def.description = "Distance between two scalars or vectors.";
        def.ueClass = "MaterialExpressionDistance";
        def.inputs = {
            In("A", PinType::Wildcard, PropertyValue::Vec3Value(0.0f, 0.0f, 0.0f)),
            In("B", PinType::Wildcard, PropertyValue::Vec3Value(0.0f, 0.0f, 0.0f))
        };
        def.outputs = {Out("Result", PinType::Float)};
        Register(std::move(def));
    }
    {
        NodeDefinition def;
        def.type = "SmoothStep";
        def.displayName = "SmoothStep";
        def.category = "Math";
        def.description = "Smooth Hermite interpolation between Min and Max.";
        def.ueClass = "MaterialExpressionSmoothStep";
        def.typeRule = PinTypeRule::PromoteInputs;
        def.inputs = {
            In("Value", PinType::Wildcard, PropertyValue::Float(0.0f)),
            In("Min", PinType::Wildcard, PropertyValue::Float(0.0f)),
            In("Max", PinType::Wildcard, PropertyValue::Float(1.0f))
        };
        def.outputs = {Out("Result", PinType::Wildcard)};
        Register(std::move(def));
    }
    {
        NodeDefinition def;
        def.type = "Step";
        def.displayName = "Step";
        def.category = "Math";
        def.description = "Returns 0 when Value is below Edge, otherwise 1.";
        def.ueClass = "MaterialExpressionStep";
        def.typeRule = PinTypeRule::PromoteInputs;
        def.inputs = {
            In("Value", PinType::Wildcard, PropertyValue::Float(0.0f)),
            In("Edge", PinType::Wildcard, PropertyValue::Float(0.5f))
        };
        def.outputs = {Out("Result", PinType::Wildcard)};
        Register(std::move(def));
    }
    {
        NodeDefinition def;
        def.type = "ConstantBiasScale";
        def.displayName = "ConstantBiasScale";
        def.category = "Math";
        def.description = "Returns Input * Scale + Bias.";
        def.ueClass = "MaterialExpressionConstantBiasScale";
        def.typeRule = PinTypeRule::PromoteInputs;
        def.inputs = {
            In("Input", PinType::Wildcard, PropertyValue::Float(0.0f)),
            In("Bias", PinType::Wildcard, PropertyValue::Float(0.0f)),
            In("Scale", PinType::Wildcard, PropertyValue::Float(1.0f))
        };
        def.outputs = {Out("Result", PinType::Wildcard)};
        Register(std::move(def));
    }
    {
        NodeDefinition def;
        def.type = "SphereMask";
        def.displayName = "SphereMask";
        def.category = "Math";
        def.description = "Masks based on the distance from A to B inside Radius.";
        def.ueClass = "MaterialExpressionSphereMask";
        def.inputs = {
            In("A", PinType::Vector3, PropertyValue::Vec3Value(0.0f, 0.0f, 0.0f)),
            In("B", PinType::Vector3, PropertyValue::Vec3Value(0.0f, 0.0f, 0.0f)),
            In("Radius", PinType::Float, PropertyValue::Float(0.5f)),
            In("Hardness", PinType::Float, PropertyValue::Float(0.5f))
        };
        def.outputs = {Out("Result", PinType::Float)};
        Register(std::move(def));
    }
    {
        NodeDefinition def;
        def.type = "Noise";
        def.displayName = "Noise";
        def.category = "Math";
        def.description = "Deterministic value noise from a 3D position.";
        def.ueClass = "MaterialExpressionNoise";
        def.inputs = {
            In("Position", PinType::Vector3, PropertyValue::Vec3Value(0.0f, 0.0f, 0.0f))
        };
        def.outputs = {Out("Result", PinType::Float)};
        def.properties = {PropFloat("scale", "Scale", 1.0f, 0.01f, true, 0.001f, 32.0f)};
        Register(std::move(def));
    }

    {
        NodeDefinition def;
        def.type = "Lerp";
        def.displayName = "Lerp";
        def.category = "Math";
        def.description = "Linear interpolation between A and B by Alpha.";
        def.ueClass = "MaterialExpressionLerp";
        def.typeRule = PinTypeRule::PromoteInputs;
        def.inputs = {
            In("A", PinType::Wildcard, PropertyValue::Float(0.0f)),
            In("B", PinType::Wildcard, PropertyValue::Float(1.0f)),
            In("Alpha", PinType::Float, PropertyValue::Float(0.5f))
        };
        def.outputs = {Out("Result", PinType::Wildcard)};
        Register(std::move(def));
    }
    {
        NodeDefinition def;
        def.type = "Clamp";
        def.displayName = "Clamp";
        def.category = "Math";
        def.description = "Clamps Value between Min and Max.";
        def.ueClass = "MaterialExpressionClamp";
        def.typeRule = PinTypeRule::PromoteInputs;
        def.inputs = {
            In("Value", PinType::Wildcard, PropertyValue::Float(0.0f)),
            In("Min", PinType::Wildcard, PropertyValue::Float(0.0f)),
            In("Max", PinType::Wildcard, PropertyValue::Float(1.0f))
        };
        def.outputs = {Out("Result", PinType::Wildcard)};
        Register(std::move(def));
    }
    {
        NodeDefinition def;
        def.type = "DotProduct";
        def.displayName = "DotProduct";
        def.category = "Math";
        def.description = "Dot product of two vectors. Returns a scalar.";
        def.ueClass = "MaterialExpressionDotProduct";
        def.inputs = {
            In("A", PinType::Wildcard, PropertyValue::Float(0.0f)),
            In("B", PinType::Wildcard, PropertyValue::Float(0.0f))
        };
        def.outputs = {Out("Result", PinType::Float)};
        Register(std::move(def));
    }
    {
        NodeDefinition def;
        def.type = "CrossProduct";
        def.displayName = "CrossProduct";
        def.category = "Math";
        def.description = "Cross product of two three-component vectors.";
        def.ueClass = "MaterialExpressionCrossProduct";
        def.inputs = {
            In("A", PinType::Vector3, PropertyValue::Vec3Value(1.0f, 0.0f, 0.0f)),
            In("B", PinType::Vector3, PropertyValue::Vec3Value(0.0f, 1.0f, 0.0f))
        };
        def.outputs = {Out("Result", PinType::Vector3)};
        Register(std::move(def));
    }
    {
        NodeDefinition def;
        def.type = "Normalize";
        def.displayName = "Normalize";
        def.category = "Math";
        def.description = "Returns a unit-length vector. Useful for normals and directions.";
        def.ueClass = "MaterialExpressionNormalize";
        def.typeRule = PinTypeRule::MatchFirstInput;
        def.inputs = {In("Input", PinType::Wildcard,
                         PropertyValue::Vec3Value(0.0f, 0.0f, 1.0f))};
        def.outputs = {Out("Result", PinType::Wildcard)};
        Register(std::move(def));
    }
    {
        NodeDefinition def;
        def.type = "Desaturation";
        def.displayName = "Desaturation";
        def.category = "Utility";
        def.description = "Blends a color toward its luminance using Fraction.";
        def.ueClass = "MaterialExpressionDesaturation";
        def.inputs = {
            In("Color", PinType::Vector3, PropertyValue::Color3Value(0.5f, 0.5f, 0.5f)),
            In("Fraction", PinType::Float, PropertyValue::Float(1.0f))
        };
        def.outputs = {Out("Result", PinType::Vector3)};
        Register(std::move(def));
    }
    {
        NodeDefinition def;
        def.type = "Fresnel";
        def.displayName = "Fresnel";
        def.category = "Utility";
        def.description = "View-angle fresnel term. This preview uses the geometric normal.";
        def.ueClass = "MaterialExpressionFresnel";
        def.outputs = {Out("Result", PinType::Float)};
        def.properties = {
            PropFloat("exponent", "Exponent", 5.0f, 0.05f, true, 0.0f, 16.0f,
                      "Higher values tighten the fresnel falloff."),
            PropFloat("baseReflectFraction", "Base Reflect Fraction", 0.04f, 0.001f,
                      true, 0.0f, 1.0f)
        };
        Register(std::move(def));
    }

    // ------------------------------------------------------------------
    // Coordinates
    // ------------------------------------------------------------------
    {
        NodeDefinition def;
        def.type = "TextureCoordinate";
        def.displayName = "TextureCoordinate";
        def.category = "Coordinates";
        def.description = "Mesh UV coordinates scaled by U/V tiling.";
        def.ueClass = "MaterialExpressionTextureCoordinate";
        def.outputs = {Out("UV", PinType::Vector2)};
        def.properties = {
            PropFloat("uTiling", "U Tiling", 1.0f, 0.01f, true, 0.001f, 64.0f),
            PropFloat("vTiling", "V Tiling", 1.0f, 0.01f, true, 0.001f, 64.0f)
        };
        Register(std::move(def));
    }
    {
        NodeDefinition def;
        def.type = "Panner";
        def.displayName = "Panner";
        def.category = "Coordinates";
        def.description = "Scrolls UV coordinates over time. Time defaults to the material time.";
        def.ueClass = "MaterialExpressionPanner";
        def.inputs = {
            In("Coordinate", PinType::Vector2, PropertyValue::Vec2Value(0.0f, 0.0f)),
            In("Time", PinType::Float, PropertyValue::Float(0.0f),
               "Leave unconnected to use the global material time uniform.")
        };
        def.outputs = {Out("UV", PinType::Vector2)};
        def.properties = {PropVec2("speed", "Speed", Vec2(0.05f, 0.02f), 0.001f)};
        Register(std::move(def));
    }
    {
        NodeDefinition def;
        def.type = "Rotator";
        def.displayName = "Rotator";
        def.category = "Coordinates";
        def.description = "Rotates UV coordinates around a center point over time.";
        def.ueClass = "MaterialExpressionRotator";
        def.inputs = {
            In("Coordinate", PinType::Vector2, PropertyValue::Vec2Value(0.0f, 0.0f),
               "UVs to rotate. Defaults to mesh UVs.")
        };
        def.outputs = {Out("UV", PinType::Vector2)};
        def.properties = {
            PropVec2("center", "Center", Vec2(0.5f, 0.5f), 0.001f),
            PropFloat("speed", "Speed", 0.1f, 0.001f, true, -10.0f, 10.0f,
                      "Rotation speed in radians per second.")
        };
        Register(std::move(def));
    }
    {
        NodeDefinition def;
        def.type = "BumpOffset";
        def.displayName = "BumpOffset";
        def.category = "Coordinates";
        def.description =
            "Offsets UV coordinates using a height value. The preview uses a simple "
            "parallax approximation without view-dependent depth.";
        def.ueClass = "MaterialExpressionBumpOffset";
        def.inputs = {
            In("Coordinate", PinType::Vector2, PropertyValue::Vec2Value(0.0f, 0.0f)),
            In("Height", PinType::Float, PropertyValue::Float(0.0f)),
            In("HeightRatio", PinType::Float, PropertyValue::Float(0.05f)),
            In("ReferencePlane", PinType::Float, PropertyValue::Float(0.0f))
        };
        def.outputs = {Out("UV", PinType::Vector2)};
        Register(std::move(def));
    }
    {
        NodeDefinition def;
        def.type = "Time";
        def.displayName = "Time";
        def.category = "Coordinates";
        def.description = "Global material time in seconds.";
        def.ueClass = "MaterialExpressionTime";
        def.outputs = {Out("Time", PinType::Float)};
        Register(std::move(def));
    }
    {
        NodeDefinition def;
        def.type = "WorldPosition";
        def.displayName = "WorldPosition";
        def.category = "Coordinates";
        def.description = "World-space position of the shaded pixel.";
        def.ueClass = "MaterialExpressionWorldPosition";
        def.outputs = {Out("World Position", PinType::Vector3)};
        Register(std::move(def));
    }
    {
        NodeDefinition def;
        def.type = "VertexNormal";
        def.displayName = "VertexNormal";
        def.category = "Coordinates";
        def.description = "Interpolated world-space vertex normal.";
        def.ueClass = "MaterialExpressionVertexNormal";
        def.outputs = {Out("Vertex Normal", PinType::Vector3)};
        Register(std::move(def));
    }
    {
        NodeDefinition def;
        def.type = "CameraVector";
        def.displayName = "CameraVector";
        def.category = "Coordinates";
        def.description = "Normalized world-space vector from the shaded pixel to the camera.";
        def.ueClass = "MaterialExpressionCameraVector";
        def.outputs = {Out("Camera Vector", PinType::Vector3)};
        Register(std::move(def));
    }
    {
        NodeDefinition def;
        def.type = "PixelDepth";
        def.displayName = "PixelDepth";
        def.category = "Coordinates";
        def.description = "Distance from the preview camera to the shaded pixel.";
        def.ueClass = "MaterialExpressionPixelDepth";
        def.outputs = {Out("Depth", PinType::Float)};
        Register(std::move(def));
    }
    {
        NodeDefinition def;
        def.type = "SceneDepth";
        def.displayName = "SceneDepth";
        def.category = "Coordinates";
        def.description = "Scene depth at the shaded pixel. The preview approximates it with camera distance.";
        def.ueClass = "MaterialExpressionSceneDepth";
        def.outputs = {Out("Depth", PinType::Float)};
        Register(std::move(def));
    }
    {
        NodeDefinition def;
        def.type = "CameraPosition";
        def.displayName = "CameraPosition";
        def.category = "Coordinates";
        def.description = "World-space position of the preview camera.";
        def.ueClass = "MaterialExpressionCameraPositionWS";
        def.outputs = {Out("Camera Position", PinType::Vector3)};
        Register(std::move(def));
    }
    {
        NodeDefinition def;
        def.type = "ObjectPosition";
        def.displayName = "ObjectPosition";
        def.category = "Coordinates";
        def.description = "World-space position of the preview object (origin).";
        def.ueClass = "MaterialExpressionObjectPositionWS";
        def.outputs = {Out("Object Position", PinType::Vector3)};
        Register(std::move(def));
    }
    {
        NodeDefinition def;
        def.type = "ActorPosition";
        def.displayName = "ActorPosition";
        def.category = "Coordinates";
        def.description = "World-space position of the owning actor (origin in the preview).";
        def.ueClass = "MaterialExpressionActorPositionWS";
        def.outputs = {Out("Actor Position", PinType::Vector3)};
        Register(std::move(def));
    }
    {
        NodeDefinition def;
        def.type = "LightVector";
        def.displayName = "LightVector";
        def.category = "Coordinates";
        def.description = "Direction toward the key studio light.";
        def.ueClass = "MaterialExpressionLightVector";
        def.outputs = {Out("Light Vector", PinType::Vector3)};
        Register(std::move(def));
    }
    {
        NodeDefinition def;
        def.type = "ReflectionVector";
        def.displayName = "ReflectionVector";
        def.category = "Coordinates";
        def.description = "Camera vector reflected around the surface normal.";
        def.ueClass = "MaterialExpressionReflectionVectorWS";
        def.outputs = {Out("Reflection Vector", PinType::Vector3)};
        Register(std::move(def));
    }
    {
        NodeDefinition def;
        def.type = "ScreenPosition";
        def.displayName = "ScreenPosition";
        def.category = "Coordinates";
        def.description = "Normalized screen-space position (viewport UV).";
        def.ueClass = "MaterialExpressionScreenPosition";
        def.outputs = {Out("Screen Position", PinType::Vector2)};
        Register(std::move(def));
    }
    {
        NodeDefinition def;
        def.type = "ViewSize";
        def.displayName = "ViewSize";
        def.category = "Coordinates";
        def.description = "Current preview viewport size in pixels.";
        def.ueClass = "MaterialExpressionViewSize";
        def.outputs = {Out("View Size", PinType::Vector2)};
        Register(std::move(def));
    }
    {
        NodeDefinition def;
        def.type = "VertexColor";
        def.displayName = "VertexColor";
        def.category = "Coordinates";
        def.description = "Vertex color. The preview mesh has no vertex colors, so this is white.";
        def.ueClass = "MaterialExpressionVertexColor";
        def.outputs = {Out("Vertex Color", PinType::Vector4)};
        Register(std::move(def));
    }
    {
        NodeDefinition def;
        def.type = "TwoSidedSign";
        def.displayName = "TwoSidedSign";
        def.category = "Utility";
        def.description = "Front/back facing sign. The preview is single-sided, so this is 1.";
        def.ueClass = "MaterialExpressionTwoSidedSign";
        def.outputs = {Out("Sign", PinType::Float)};
        Register(std::move(def));
    }

    // ------------------------------------------------------------------
    // Textures
    // ------------------------------------------------------------------
    {
        NodeDefinition def;
        def.type = "Sampler";
        def.displayName = "Sampler";
        def.category = "Textures";
        def.description =
            "Shared texture sampler settings. Connect to a TextureSample Sampler input.";
        def.ueClass = "MaterialExpressionSampler";
        def.outputs = {Out("Sampler", PinType::Sampler)};
        def.properties = SamplerProperties();
        Register(std::move(def));
    }
    {
        NodeDefinition def;
        def.type = "TextureObject";
        def.displayName = "TextureObject";
        def.category = "Textures";
        def.description = "A texture asset reference. Connect it to a TextureSample's Texture input.";
        def.ueClass = "MaterialExpressionTextureObject";
        def.outputs = {Out("Texture", PinType::Texture)};
        def.properties = WithSamplerProperties({
            PropAsset("texture", "Texture", "assets/textures/T_Checker.png",
                      "Path to a PNG/JPG/TGA texture. Relative paths resolve next to the executable.")
        });
        Register(std::move(def));
    }
    {
        NodeDefinition def;
        def.type = "TextureObjectParameter";
        def.displayName = "TextureObjectParameter";
        def.category = "Textures";
        def.description = "A named texture parameter that can be overridden per material instance.";
        def.ueClass = "MaterialExpressionTextureObjectParameter";
        def.outputs = {Out("Texture", PinType::Texture)};
        def.properties = WithSamplerProperties({
            PropString("parameterName", "Parameter Name", "Texture",
                       "Name exposed to material instances in UE5."),
            PropAsset("texture", "Texture", "assets/textures/T_Checker.png",
                      "Fallback texture path.")
        });
        Register(std::move(def));
    }
    {
        NodeDefinition def;
        def.type = "TextureSample";
        def.displayName = "TextureSample";
        def.category = "Textures";
        def.description = "Samples a texture. UVs default to the mesh UV0 channel.";
        def.ueClass = "MaterialExpressionTextureSample";
        def.inputs = {
            In("UVs", PinType::Vector2, PropertyValue::Vec2Value(0.0f, 0.0f)),
            In("Texture", PinType::Texture),
            In("Sampler", PinType::Sampler)
        };
        def.outputs = {
            Out("RGBA", PinType::Vector4),
            Out("RGB", PinType::Vector3),
            Out("R", PinType::Float),
            Out("G", PinType::Float),
            Out("B", PinType::Float),
            Out("A", PinType::Float)
        };
        Register(std::move(def));
    }

    {
        NodeDefinition def;
        def.type = "TextureSampleParameter2D";
        def.displayName = "TextureSampleParameter2D";
        def.category = "Textures";
        def.description = "Texture sample exposed as a named texture parameter.";
        def.ueClass = "MaterialExpressionTextureSampleParameter2D";
        def.inputs = {
            In("UVs", PinType::Vector2, PropertyValue::Vec2Value(0.0f, 0.0f)),
            In("Texture", PinType::Texture),
            In("Sampler", PinType::Sampler)
        };
        def.outputs = {
            Out("RGBA", PinType::Vector4),
            Out("RGB", PinType::Vector3),
            Out("R", PinType::Float),
            Out("G", PinType::Float),
            Out("B", PinType::Float),
            Out("A", PinType::Float)
        };
        def.properties = WithSamplerProperties({
            PropString("parameterName", "Parameter Name", "Texture",
                       "Name exposed to material instances in UE5."),
            PropAsset("texture", "Texture", "assets/textures/T_Checker.png",
                      "Fallback texture when the Texture input is not connected.")
        });
        Register(std::move(def));
    }

    {
        NodeDefinition def;
        def.type = "SceneTexture";
        def.displayName = "SceneTexture";
        def.category = "Textures";
        def.description =
            "Scene texture sampling node. Preview provides Color/Size/InvSize but uses a "
            "neutral placeholder color because scene capture is not linked in this version.";
        def.ueClass = "MaterialExpressionSceneTexture";
        def.inputs = {
            In("UVs", PinType::Vector2, PropertyValue::Vec2Value(0.0f, 0.0f))
        };
        def.outputs = {
            Out("Color", PinType::Vector4),
            Out("Size", PinType::Vector2),
            Out("InvSize", PinType::Vector2)
        };
        def.properties = {
            PropString("sceneTextureId", "Scene Texture Id", "Scene Color",
                       "Scene texture slot used by UE5. Preview does not capture the scene.")
        };
        Register(std::move(def));
    }

    // ------------------------------------------------------------------
    // Material function calls (pins are built from the referenced function)
    // ------------------------------------------------------------------
    {
        NodeDefinition def;
        def.type = "MaterialFunctionCall";
        def.displayName = "Material Function Call";
        def.category = "Functions";
        def.description =
            "Calls a reusable blueprint function asset. Drag a function asset from "
            "the Content Browser or the Material Functions section into the graph.";
        def.ueClass = "MaterialExpressionMaterialFunctionCall";
        def.properties = {
            PropString("functionPath", "Function Asset", "",
                       "Path to the .mffunction asset, relative to the project or absolute."),
            PropString("functionName", "Function Name", "Material Function",
                       "Display name of the referenced function.")
        };
        def.hiddenInLibrary = true;
        def.isFunctionCall = true;
        Register(std::move(def));
    }

    // ------------------------------------------------------------------
    // Blueprint function graph nodes
    // ------------------------------------------------------------------
    {
        NodeDefinition def;
        def.type = "FunctionInput";
        def.displayName = "Function Input";
        def.category = "Function";
        def.description = "Exposes a named typed input on a blueprint function.";
        def.ueClass = "MaterialExpressionFunctionInput";
        def.outputs = {Out("Value", PinType::Float)};
        def.properties = {
            PropString("inputName", "Input Name", "Input",
                       "Name of the input when the function is used."),
            PropString("pinType", "Type", "Float",
                       "Strongly typed value type of this function input.")};
        def.isFunctionOnly = true;
        Register(std::move(def));
    }
    {
        NodeDefinition def;
        def.type = "FunctionOutput";
        def.displayName = "Function Output";
        def.category = "Function";
        def.description = "Collects a named typed result from a blueprint function.";
        def.ueClass = "MaterialExpressionFunctionOutput";
        def.inputs = {In("Value", PinType::Float)};
        def.properties = {
            PropString("outputName", "Output Name", "Output",
                       "Name of the output when the function is used."),
            PropString("pinType", "Type", "Float",
                       "Strongly typed value type of this function output.")};
        def.isFunctionOnly = true;
        Register(std::move(def));
    }
}

void NodeRegistry::Register(NodeDefinition definition) {
    m_Index[definition.type] = m_Definitions.size();
    m_Definitions.push_back(std::move(definition));
}

const NodeDefinition* NodeRegistry::Find(const std::string& type) const {
    const auto it = m_Index.find(type);
    return it == m_Index.end() ? nullptr : &m_Definitions[it->second];
}

std::vector<std::string> NodeRegistry::Categories() const {
    static const std::vector<std::string> preferred = {
        "Constants", "Parameters", "Math", "Coordinates", "Textures", "Utility",
        "Function", "Output"
    };
    std::vector<std::string> categories;
    for (const std::string& category : preferred) {
        for (const NodeDefinition& definition : m_Definitions) {
            if (definition.hiddenInLibrary) continue;
            if (definition.category == category) {
                categories.push_back(category);
                break;
            }
        }
    }
    return categories;
}

} // namespace mf
