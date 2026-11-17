#pragma once

#include "app/Localization.h"
#include "app/TextSearch.h"
#include "core/NodeRegistry.h"

#include <string>

namespace mf {
namespace detail {

inline const char* NodeSearchAliases(const std::string& type) {
    if (type == "Add") return u8"加法 相加";
    if (type == "Subtract") return u8"减法 相减";
    if (type == "Multiply") return u8"乘法 相乘";
    if (type == "Divide") return u8"除法 相除";
    if (type == "Lerp") return u8"插值 混合";
    if (type == "Clamp") return u8"钳制 限制";
    if (type == "DotProduct") return u8"点积";
    if (type == "CrossProduct") return u8"叉积";
    if (type == "Normalize") return u8"归一化 单位向量";
    if (type == "Desaturation") return u8"去饱和 灰度";
    if (type == "Fresnel") return u8"菲涅尔 边缘";
    if (type == "TextureCoordinate") return u8"纹理坐标 UV";
    if (type == "Panner") return u8"平移 滚动 UV";
    if (type == "Rotator") return u8"旋转 UV";
    if (type == "Time") return u8"时间";
    if (type == "WorldPosition") return u8"世界坐标 位置";
    if (type == "VertexNormal") return u8"顶点法线";
    if (type == "CameraVector") return u8"相机方向 视角";
    if (type == "TextureObject") return u8"纹理对象";
    if (type == "TextureSample") return u8"纹理采样 贴图";
    if (type == "Constant") return u8"常量 标量";
    if (type == "Constant2Vector") return u8"常量 二维向量";
    if (type == "Constant3Vector") return u8"常量 三维向量 颜色";
    if (type == "Constant4Vector") return u8"常量 四维向量 颜色";
    if (type == "ScalarParameter") return u8"标量参数";
    if (type == "VectorParameter") return u8"向量参数 颜色参数";
    if (type == "MaterialOutput") return u8"材质输出 根节点";
    if (type == "FunctionInput") return u8"函数输入 蓝图函数";
    if (type == "FunctionOutput") return u8"函数输出 蓝图函数";
    return "";
}

} // namespace detail

inline std::string NodeSearchText(const NodeDefinition& definition) {
    std::string haystack;
    haystack.reserve(512);
    haystack += definition.displayName;
    haystack += ' ';
    haystack += definition.type;
    haystack += ' ';
    haystack += definition.ueClass;
    haystack += ' ';
    haystack += Tr(definition.category.c_str());
    haystack += ' ';
    haystack += definition.description;
    haystack += ' ';
    haystack += Tr(definition.description.c_str());
    haystack += ' ';
    haystack += detail::NodeSearchAliases(definition.type);
    for (const PropertySpec& property : definition.properties) {
        haystack += ' ';
        haystack += property.key;
        haystack += ' ';
        haystack += property.label;
        haystack += ' ';
        haystack += Tr(property.label.c_str());
        haystack += ' ';
        haystack += property.tooltip;
        haystack += ' ';
        haystack += Tr(property.tooltip.c_str());
    }
    for (const PinDefinition& pin : definition.inputs) {
        haystack += ' ';
        haystack += pin.name;
        haystack += ' ';
        haystack += pin.tooltip;
        haystack += ' ';
        haystack += Tr(pin.tooltip.c_str());
    }
    for (const PinDefinition& pin : definition.outputs) {
        haystack += ' ';
        haystack += pin.name;
        haystack += ' ';
        haystack += pin.tooltip;
        haystack += ' ';
        haystack += Tr(pin.tooltip.c_str());
    }
    return haystack;
}

inline bool NodeMatchesSearch(const NodeDefinition& definition, const std::string& query) {
    return MatchesSearch(NodeSearchText(definition), query);
}

} // namespace mf
