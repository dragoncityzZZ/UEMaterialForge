#include "io/Ue5Clipboard.h"

#include "core/NodeRegistry.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mf {
namespace {

// ---------------------------------------------------------------------------
// Small text helpers
// ---------------------------------------------------------------------------

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string Trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
    return ToLower(a) == ToLower(b);
}

std::string Unquote(const std::string& value) {
    const std::string trimmed = Trim(value);
    if (trimmed.size() >= 2) {
        const char front = trimmed.front();
        const char back = trimmed.back();
        if ((front == '"' && back == '"') || (front == '\'' && back == '\'')) {
            return trimmed.substr(1, trimmed.size() - 2);
        }
    }
    return trimmed;
}

// Removes the path prefix and quotes from a UE class name such as
// "/Script/Engine.MaterialExpressionMultiply".
std::string ShortClassName(const std::string& className) {
    std::string value = Unquote(className);
    const auto slash = value.find_last_of('/');
    if (slash != std::string::npos) value = value.substr(slash + 1);
    const auto dot = value.find_last_of('.');
    if (dot != std::string::npos) value = value.substr(dot + 1);
    return value;
}

std::vector<float> ParseNumbers(const std::string& text) {
    std::vector<float> numbers;
    const char* cursor = text.c_str();
    while (*cursor != '\0') {
        if (std::isdigit(static_cast<unsigned char>(*cursor)) || *cursor == '-' ||
            *cursor == '+' || *cursor == '.') {
            char* end = nullptr;
            const double value = std::strtod(cursor, &end);
            if (end != cursor) {
                numbers.push_back(static_cast<float>(value));
                cursor = end;
                continue;
            }
        }
        ++cursor;
    }
    return numbers;
}

bool ParseFloatValue(const std::string& raw, float* value) {
    if (!value) return false;
    const std::string text = Trim(Unquote(raw));
    if (text.empty()) return false;
    char* end = nullptr;
    const double parsed = std::strtod(text.c_str(), &end);
    if (end != text.c_str()) {
        *value = static_cast<float>(parsed);
        return true;
    }
    const std::vector<float> numbers = ParseNumbers(text);
    if (numbers.empty()) return false;
    *value = numbers.front();
    return true;
}

bool ParseBoolValue(const std::string& raw, bool* value) {
    if (!value) return false;
    const std::string text = ToLower(Trim(Unquote(raw)));
    if (text == "true" || text == "yes") {
        *value = true;
        return true;
    }
    if (text == "false" || text == "no") {
        *value = false;
        return true;
    }
    float number = 0.0f;
    if (ParseFloatValue(text, &number)) {
        *value = number != 0.0f;
        return true;
    }
    return false;
}

std::string ExtractAssetPath(const std::string& raw) {
    const std::string text = Trim(raw);
    if (text.empty()) return {};
    size_t start = text.find("/Game/");
    if (start == std::string::npos) start = text.find("/Engine/");
    if (start != std::string::npos) {
        size_t end = start;
        while (end < text.size()) {
            const char ch = text[end];
            if (ch == '\'' || ch == '"' || ch == ',' || ch == ')' ||
                std::isspace(static_cast<unsigned char>(ch))) {
                break;
            }
            ++end;
        }
        std::string path = text.substr(start, end - start);
        // UE object references use /Game/Path/Package.Package; MaterialForge
        // stores the package path without the object suffix.
        const auto slash = path.find_last_of('/');
        const auto dot = path.find_last_of('.');
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
            path.resize(dot);
        }
        return path;
    }
    const std::string unquoted = Unquote(text);
    if (unquoted == "None" || unquoted == "nullptr") return {};
    return unquoted;
}

std::string ExtractDisplayText(const std::string& raw) {
    const std::string text = Trim(raw);
    if (text.rfind("NSLOCTEXT", 0) == 0 || text.find("NSLOCTEXT") != std::string::npos) {
        const auto last = text.find_last_of('"');
        if (last != std::string::npos) {
            const auto first = text.rfind('"', last - 1);
            if (first != std::string::npos && last > first + 1) {
                return text.substr(first + 1, last - first - 1);
            }
            if (first != std::string::npos) return {};
        }
        const auto lastSingle = text.find_last_of('\'');
        if (lastSingle != std::string::npos) {
            const auto firstSingle = text.rfind('\'', lastSingle - 1);
            if (firstSingle != std::string::npos) {
                return text.substr(firstSingle + 1, lastSingle - firstSingle - 1);
            }
        }
    }
    return Unquote(text);
}

const std::string* FindProp(const std::map<std::string, std::string>& props,
                            std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        const auto exact = props.find(key);
        if (exact != props.end()) return &exact->second;
        for (const auto& [name, value] : props) {
            if (EqualsIgnoreCase(name, key)) return &value;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// UE text export scanner
// ---------------------------------------------------------------------------

struct RawPin {
    std::string id;
    std::string name;
    std::string defaultValue;
    bool output = false;
    bool hasDefaultValue = false;
    std::vector<std::pair<std::string, std::string>> links; // (graph node, pin id)
};

struct RawObject {
    std::string className;
    std::string name;
    std::map<std::string, std::string> props;
    std::vector<RawPin> pins;
    std::vector<RawObject> children;
};

class Scanner {
public:
    explicit Scanner(const std::string& text) : m_Text(text) {}

    bool Eof() const { return m_Position >= m_Text.size(); }
    char Peek() const { return Eof() ? '\0' : m_Text[m_Position]; }
    std::size_t Position() const { return m_Position; }
    const std::string& Text() const { return m_Text; }

    void SkipSpace() {
        while (!Eof() && std::isspace(static_cast<unsigned char>(Peek()))) {
            ++m_Position;
        }
    }

    bool StartsWith(std::string_view literal) const {
        if (m_Text.compare(m_Position, literal.size(), literal) != 0) return false;
        const std::size_t end = m_Position + literal.size();
        if (end < m_Text.size()) {
            const unsigned char next = static_cast<unsigned char>(m_Text[end]);
            if (std::isalnum(next) || next == '_') return false;
        }
        return true;
    }

    bool Consume(std::string_view literal) {
        if (!StartsWith(literal)) return false;
        m_Position += literal.size();
        return true;
    }

    void Advance() {
        if (!Eof()) ++m_Position;
    }

    bool ReadQuoted() {
        if (Eof()) return false;
        const char quote = Peek();
        if (quote != '"' && quote != '\'') return false;
        ++m_Position;
        while (!Eof()) {
            const char ch = Peek();
            ++m_Position;
            if (ch == '\\' && !Eof()) {
                ++m_Position;
                continue;
            }
            if (ch == quote) {
                // Unreal doubles the quote character to escape it in FString
                // exports, but some builds use backslashes instead.
                if (!Eof() && Peek() == quote) {
                    ++m_Position;
                    continue;
                }
                return true;
            }
        }
        return false;
    }

    bool ReadBalanced(std::string* out) {
        if (Eof() || Peek() != '(') return false;
        const std::size_t start = m_Position;
        int depth = 0;
        while (!Eof()) {
            const char ch = Peek();
            if (ch == '"' || ch == '\'') {
                if (!ReadQuoted()) return false;
                continue;
            }
            ++m_Position;
            if (ch == '(') {
                ++depth;
            } else if (ch == ')') {
                --depth;
                if (depth == 0) {
                    if (out) *out = m_Text.substr(start, m_Position - start);
                    return true;
                }
            }
        }
        return false;
    }

private:
    const std::string& m_Text;
    std::size_t m_Position = 0;
};

std::string ReadValue(Scanner& scanner) {
    scanner.SkipSpace();
    const std::size_t start = scanner.Position();
    if (!scanner.Eof() && scanner.Peek() == '(') {
        std::string balanced;
        if (scanner.ReadBalanced(&balanced)) return balanced;
        return {};
    }
    while (!scanner.Eof()) {
        const char ch = scanner.Peek();
        if (std::isspace(static_cast<unsigned char>(ch))) break;
        if (ch == '"' || ch == '\'') {
            if (!scanner.ReadQuoted()) break;
            continue;
        }
        if (ch == '(') {
            std::string balanced;
            if (!scanner.ReadBalanced(&balanced)) break;
            continue;
        }
        scanner.Advance();
    }
    if (scanner.Position() == start && !scanner.Eof()) scanner.Advance();
    return scanner.Text().substr(start, scanner.Position() - start);
}

bool ParseProperty(Scanner& scanner, std::string* key, std::string* value) {
    scanner.SkipSpace();
    const std::size_t start = scanner.Position();
    while (!scanner.Eof()) {
        const char ch = scanner.Peek();
        if (ch == '=') break;
        if (std::isspace(static_cast<unsigned char>(ch))) {
            // No assignment before the next token; skip it so the outer loop
            // always makes progress on malformed input.
            while (!scanner.Eof() &&
                   !std::isspace(static_cast<unsigned char>(scanner.Peek()))) {
                scanner.Advance();
            }
            return false;
        }
        scanner.Advance();
    }
    if (scanner.Eof() || scanner.Peek() != '=') return false;
    const std::string parsedKey = Trim(scanner.Text().substr(start, scanner.Position() - start));
    scanner.Advance();
    if (parsedKey.empty()) return false;
    if (key) *key = parsedKey;
    if (value) *value = ReadValue(scanner);
    return true;
}

void SplitTopLevel(const std::string& text, char delimiter, std::vector<std::string>* out) {
    if (!out) return;
    int depth = 0;
    char quote = '\0';
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (quote != '\0') {
            if (ch == '\\') {
                ++i;
            } else if (ch == quote) {
                quote = '\0';
            }
            continue;
        }
        if (ch == '"' || ch == '\'') {
            quote = ch;
            continue;
        }
        if (ch == '(') {
            ++depth;
            continue;
        }
        if (ch == ')') {
            if (depth > 0) --depth;
            continue;
        }
        if (ch == delimiter && depth == 0) {
            out->push_back(Trim(text.substr(start, i - start)));
            start = i + 1;
        }
    }
    out->push_back(Trim(text.substr(start)));
}

void ParseLinkedTo(const std::string& raw,
                   std::vector<std::pair<std::string, std::string>>* links) {
    if (!links) return;
    std::string text = Trim(raw);
    if (text.size() >= 2 && text.front() == '(' && text.back() == ')') {
        text = text.substr(1, text.size() - 2);
    }
    std::vector<std::string> items;
    SplitTopLevel(text, ',', &items);
    for (std::string item : items) {
        item = Trim(item);
        while (!item.empty() && (item.back() == ',' || item.back() == ' ')) {
            item.pop_back();
        }
        if (item.empty()) continue;
        const auto split = item.find_first_of(" \t");
        if (split == std::string::npos) continue;
        const std::string node = Unquote(Trim(item.substr(0, split)));
        std::string pin = Trim(item.substr(split + 1));
        while (!pin.empty() && (pin.back() == ',' || pin.back() == ' ')) {
            pin.pop_back();
        }
        if (node.empty() || pin.empty()) continue;
        links->emplace_back(node, Unquote(pin));
    }
}

bool ParsePin(Scanner& scanner, RawPin* pin) {
    if (!pin) return false;
    scanner.SkipSpace();
    if (!scanner.Consume("Pin")) return false;
    scanner.SkipSpace();
    if (scanner.Eof() || scanner.Peek() != '(') return false;
    std::string body;
    if (!scanner.ReadBalanced(&body)) return false;
    if (body.size() >= 2) body = body.substr(1, body.size() - 2);

    std::vector<std::string> fields;
    SplitTopLevel(body, ',', &fields);
    for (const std::string& field : fields) {
        const auto equals = field.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = Trim(field.substr(0, equals));
        const std::string value = Trim(field.substr(equals + 1));
        if (key == "PinId") {
            pin->id = Unquote(value);
        } else if (key == "PinName") {
            pin->name = Unquote(value);
        } else if (key == "DefaultValue") {
            pin->defaultValue = value;
            pin->hasDefaultValue = true;
        } else if (key == "Direction") {
            pin->output = Unquote(value).find("Output") != std::string::npos;
        } else if (key == "LinkedTo") {
            ParseLinkedTo(value, &pin->links);
        }
    }
    return !pin->id.empty();
}

bool ParseObject(Scanner& scanner, RawObject* object, std::string* error);

bool ParseObjectBody(Scanner& scanner, RawObject* object, std::string* error) {
    while (!scanner.Eof()) {
        scanner.SkipSpace();
        if (scanner.Consume("End Object")) return true;
        if (scanner.StartsWith("Begin Object")) {
            RawObject child;
            if (!ParseObject(scanner, &child, error)) return false;
            object->children.push_back(std::move(child));
            continue;
        }
        if (scanner.Consume("CustomProperties")) {
            RawPin pin;
            if (ParsePin(scanner, &pin)) {
                object->pins.push_back(std::move(pin));
            }
            continue;
        }
        std::string key;
        std::string value;
        if (ParseProperty(scanner, &key, &value)) {
            object->props[key] = value;
        }
    }
    if (error) *error = "Unexpected end of UE5 material clipboard text.";
    return false;
}

bool ParseObject(Scanner& scanner, RawObject* object, std::string* error) {
    if (!scanner.Consume("Begin Object")) {
        if (error) *error = "Expected 'Begin Object' in UE5 material clipboard text.";
        return false;
    }
    scanner.SkipSpace();
    if (!ParseObjectBody(scanner, object, error)) return false;
    const auto classIt = object->props.find("Class");
    if (classIt != object->props.end()) object->className = Unquote(classIt->second);
    const auto nameIt = object->props.find("Name");
    if (nameIt != object->props.end()) object->name = Unquote(nameIt->second);
    return true;
}

bool ParseTopLevelObjects(const std::string& text, std::vector<RawObject>* objects,
                          std::string* error) {
    Scanner scanner(text);
    while (!scanner.Eof()) {
        scanner.SkipSpace();
        if (scanner.Eof()) break;
        if (scanner.StartsWith("Begin Object")) {
            RawObject object;
            if (!ParseObject(scanner, &object, error)) return false;
            if (objects) objects->push_back(std::move(object));
            continue;
        }
        const std::size_t next = scanner.Text().find("Begin Object", scanner.Position());
        if (next == std::string::npos) break;
        while (scanner.Position() < next) scanner.Advance();
        if (scanner.Position() == next) scanner.Advance();
    }
    return true;
}

// ---------------------------------------------------------------------------
// Registry lookup and property mapping
// ---------------------------------------------------------------------------

const NodeDefinition* LookupDefinition(const std::string& ueClass, std::string* type) {
    struct Lookup {
        std::unordered_map<std::string, const NodeDefinition*> byClass;
        std::unordered_map<std::string, const NodeDefinition*> byType;
    };
    static const Lookup lookup = [] {
        Lookup result;
        for (const NodeDefinition& definition : NodeRegistry::Get().All()) {
            if (!definition.ueClass.empty()) {
                result.byClass.emplace(ToLower(definition.ueClass), &definition);
            }
            result.byType.emplace(ToLower(definition.type), &definition);
        }
        return result;
    }();

    static const std::unordered_map<std::string, std::string> aliases = {
        {"materialexpressionlinearinterpolate", "Lerp"},
        {"materialexpressionvertexnormalws", "VertexNormal"},
        {"materialexpressioncameravectorws", "CameraVector"},
    };

    const std::string lowered = ToLower(ShortClassName(ueClass));
    if (const auto it = lookup.byClass.find(lowered); it != lookup.byClass.end()) {
        if (type) *type = it->second->type;
        return it->second;
    }
    if (const auto alias = aliases.find(lowered); alias != aliases.end()) {
        if (const auto it = lookup.byType.find(ToLower(alias->second));
            it != lookup.byType.end()) {
            if (type) *type = it->second->type;
            return it->second;
        }
    }

    std::string suffix = lowered;
    const std::string prefix = "materialexpression";
    if (suffix.rfind(prefix, 0) == 0) suffix = suffix.substr(prefix.size());
    if (const auto it = lookup.byType.find(suffix); it != lookup.byType.end()) {
        if (type) *type = it->second->type;
        return it->second;
    }
    return nullptr;
}

void SetFloatProperty(Node& node, const char* key, float value) {
    const auto it = node.properties.find(key);
    if (it == node.properties.end()) return;
    switch (it->second.kind) {
    case PropertyKind::Float:
        it->second.number = value;
        break;
    case PropertyKind::Bool:
        it->second.boolean = value != 0.0f;
        break;
    default:
        break;
    }
}

void SetVectorProperty(Node& node, const char* key, const std::vector<float>& values) {
    if (values.empty()) return;
    const auto it = node.properties.find(key);
    if (it == node.properties.end()) return;
    PropertyValue& property = it->second;
    const auto component = [&](std::size_t index, float fallback) {
        return index < values.size() ? values[index] : fallback;
    };
    switch (property.kind) {
    case PropertyKind::Float:
        property.number = values.front();
        break;
    case PropertyKind::Vector2:
        property.vec2 = Vec2(component(0, 0.0f), component(1, 0.0f));
        break;
    case PropertyKind::Vector3:
        property.vec3 = Vec3(component(0, 0.0f), component(1, 0.0f), component(2, 0.0f));
        break;
    case PropertyKind::Vector4:
        property.vec4 = Vec4(component(0, 0.0f), component(1, 0.0f),
                             component(2, 0.0f), component(3, 1.0f));
        break;
    case PropertyKind::Color3:
        property.vec3 = Vec3(component(0, 0.0f), component(1, 0.0f), component(2, 0.0f));
        break;
    case PropertyKind::Color4:
        property.vec4 = Vec4(component(0, 0.0f), component(1, 0.0f),
                             component(2, 0.0f), component(3, 1.0f));
        break;
    case PropertyKind::Bool:
        property.boolean = values.front() != 0.0f;
        break;
    default:
        break;
    }
}

void SetStringProperty(Node& node, const char* key, const std::string& value) {
    const auto it = node.properties.find(key);
    if (it == node.properties.end()) return;
    if (it->second.kind == PropertyKind::String || it->second.kind == PropertyKind::AssetPath) {
        it->second.text = value;
    } else if (it->second.kind == PropertyKind::Bool) {
        bool parsed = false;
        if (ParseBoolValue(value, &parsed)) it->second.boolean = parsed;
    }
}

void SetTextureProperty(Node& node, const char* key, const std::string& raw) {
    const std::string path = ExtractAssetPath(raw);
    if (!path.empty()) SetStringProperty(node, key, path);
}

void ApplyProperties(Node& node, const std::map<std::string, std::string>& props) {
    const std::string type = node.type;
    const auto numeric = [](const std::string* raw, std::vector<float>* out) {
        if (!raw) return false;
        *out = ParseNumbers(*raw);
        return !out->empty();
    };
    const auto applyFloat = [&](const char* key, std::initializer_list<const char*> names,
                                float fallback = 0.0f) {
        const std::string* raw = FindProp(props, names);
        if (!raw) return;
        float value = fallback;
        if (ParseFloatValue(*raw, &value)) SetFloatProperty(node, key, value);
    };
    const auto applyString = [&](const char* key, std::initializer_list<const char*> names) {
        const std::string* raw = FindProp(props, names);
        if (!raw) return;
        SetStringProperty(node, key, Unquote(*raw));
    };
    const auto applyBool = [&](const char* key, std::initializer_list<const char*> names) {
        const std::string* raw = FindProp(props, names);
        if (!raw) return;
        bool value = false;
        if (ParseBoolValue(*raw, &value)) {
            const auto it = node.properties.find(key);
            if (it != node.properties.end() && it->second.kind == PropertyKind::Bool) {
                it->second.boolean = value;
            }
        }
    };

    if (type == "Constant") {
        applyFloat("value", {"R"});
    } else if (type == "Constant2Vector") {
        const std::string* r = FindProp(props, {"R"});
        const std::string* g = FindProp(props, {"G"});
        if (r || g) {
            float red = 0.0f;
            float green = 0.0f;
            if (r) ParseFloatValue(*r, &red);
            if (g) ParseFloatValue(*g, &green);
            SetVectorProperty(node, "value", {red, green});
        } else {
            std::vector<float> values;
            if (numeric(FindProp(props, {"Constant"}), &values)) {
                SetVectorProperty(node, "value", values);
            }
        }
    } else if (type == "Constant3Vector" || type == "Constant4Vector") {
        std::vector<float> values;
        if (numeric(FindProp(props, {"Constant", "DefaultValue"}), &values)) {
            SetVectorProperty(node, "value", values);
        } else {
            for (const char* component : {"R", "G", "B", "A"}) {
                const std::string* raw = FindProp(props, {component});
                if (!raw) {
                    values.push_back(type == "Constant4Vector" && component[0] == 'A' ? 1.0f : 0.0f);
                    continue;
                }
                float value = 0.0f;
                ParseFloatValue(*raw, &value);
                values.push_back(value);
            }
            SetVectorProperty(node, "value", values);
        }
    } else if (type == "ScalarParameter") {
        applyString("parameterName", {"ParameterName"});
        applyFloat("value", {"DefaultValue"});
        applyString("parameterGroup", {"Group", "ParameterGroup"});
        applyFloat("sortPriority", {"SortPriority"});
        applyString("description", {"Desc", "Description"});
        applyFloat("sliderMin", {"SliderMin"});
        applyFloat("sliderMax", {"SliderMax"});
    } else if (type == "VectorParameter") {
        applyString("parameterName", {"ParameterName"});
        std::vector<float> values;
        if (numeric(FindProp(props, {"DefaultValue"}), &values)) {
            SetVectorProperty(node, "value", values);
        }
        applyString("parameterGroup", {"Group", "ParameterGroup"});
        applyFloat("sortPriority", {"SortPriority"});
        applyString("description", {"Desc", "Description"});
        applyFloat("sliderMin", {"SliderMin"});
        applyFloat("sliderMax", {"SliderMax"});
    } else if (type == "StaticSwitchParameter") {
        applyString("parameterName", {"ParameterName"});
        applyBool("value", {"DefaultValue"});
    } else if (type == "ComponentMask") {
        applyBool("r", {"R"});
        applyBool("g", {"G"});
        applyBool("b", {"B"});
        applyBool("a", {"A"});
    } else if (type == "Noise") {
        applyFloat("scale", {"Scale"});
    } else if (type == "Fresnel") {
        applyFloat("exponent", {"Exponent"});
        applyFloat("baseReflectFraction", {"BaseReflectFraction"});
    } else if (type == "TextureCoordinate") {
        applyFloat("uTiling", {"UTiling"});
        applyFloat("vTiling", {"VTiling"});
    } else if (type == "Panner") {
        {
            const std::string* speedX = FindProp(props, {"SpeedX"});
            const std::string* speedY = FindProp(props, {"SpeedY"});
            if (speedX || speedY) {
                std::vector<float> values(2, 0.0f);
                float value = 0.0f;
                if (speedX && ParseFloatValue(*speedX, &value)) values[0] = value;
                if (speedY && ParseFloatValue(*speedY, &value)) values[1] = value;
                SetVectorProperty(node, "speed", values);
            } else {
                std::vector<float> values;
                if (numeric(FindProp(props, {"Speed", "SpeedXY"}), &values)) {
                    SetVectorProperty(node, "speed", values);
                }
            }
        }
    } else if (type == "Rotator") {
        const std::string* centerX = FindProp(props, {"CenterX"});
        const std::string* centerY = FindProp(props, {"CenterY"});
        if (centerX || centerY) {
            std::vector<float> values(2, 0.0f);
            float value = 0.0f;
            if (centerX && ParseFloatValue(*centerX, &value)) values[0] = value;
            if (centerY && ParseFloatValue(*centerY, &value)) values[1] = value;
            SetVectorProperty(node, "center", values);
        }
        applyFloat("speed", {"Speed"});
    } else if (type == "TextureObject" || type == "TextureObjectParameter") {
        if (const std::string* texture = FindProp(props, {"Texture", "Texture2D"})) {
            SetTextureProperty(node, "texture", *texture);
        }
        applyString("parameterName", {"ParameterName"});
    } else if (type == "TextureSampleParameter2D") {
        if (const std::string* texture = FindProp(props, {"Texture", "Texture2D"})) {
            SetTextureProperty(node, "texture", *texture);
        }
        applyString("parameterName", {"ParameterName"});
    } else if (type == "SceneTexture") {
        if (const std::string* raw = FindProp(props, {"SceneTextureId"})) {
            std::string id = Unquote(*raw);
            const std::string prefix = "PPI_";
            if (id.rfind(prefix, 0) == 0) id = id.substr(prefix.size());
            if (!id.empty()) SetStringProperty(node, "sceneTextureId", id);
        }
    } else if (type == "Comment") {
        if (const std::string* raw = FindProp(props, {"Text"})) {
            SetStringProperty(node, "text", ExtractDisplayText(*raw));
        }
        if (const std::string* raw = FindProp(props, {"CommentColor", "Color"})) {
            const std::vector<float> values = ParseNumbers(*raw);
            SetVectorProperty(node, "color", values);
        }
    }
}

void ApplyPinDefault(Pin& pin, const std::string& raw) {
    if (raw.empty()) return;
    std::vector<float> values = ParseNumbers(raw);
    if (values.empty()) {
        float number = 0.0f;
        if (ParseFloatValue(raw, &number)) values.push_back(number);
    }
    if (values.empty()) return;
    const auto component = [&](std::size_t index, float fallback) {
        return index < values.size() ? values[index] : fallback;
    };
    switch (pin.type) {
    case PinType::Float:
        pin.defaultValue = PropertyValue::Float(values.front());
        break;
    case PinType::Vector2:
        pin.defaultValue = PropertyValue::Vec2Value(component(0, 0.0f), component(1, 0.0f));
        break;
    case PinType::Vector3:
        pin.defaultValue = PropertyValue::Vec3Value(component(0, 0.0f), component(1, 0.0f),
                                                    component(2, 0.0f));
        break;
    case PinType::Vector4:
        pin.defaultValue = PropertyValue::Vec4Value(component(0, 0.0f), component(1, 0.0f),
                                                    component(2, 0.0f), component(3, 1.0f));
        break;
    default:
        break;
    }
}

std::string NormalizePinName(const std::string& name) {
    std::string normalized;
    normalized.reserve(name.size());
    for (unsigned char ch : ToLower(name)) {
        if (std::isalnum(ch)) normalized.push_back(static_cast<char>(ch));
    }
    // Material Output input pins from disabled material properties include a
    // "(Disabled)" suffix in newer engine versions.
    const std::string disabled = "disabled";
    if (normalized.size() > disabled.size() &&
        normalized.compare(normalized.size() - disabled.size(), disabled.size(), disabled) == 0) {
        normalized.resize(normalized.size() - disabled.size());
    }
    return normalized;
}

Pin* FindInputPin(Node& node, const std::string& name) {
    const std::string target = NormalizePinName(name);
    for (Pin& pin : node.inputs) {
        if (NormalizePinName(pin.name) == target) return &pin;
    }
    if (node.type == "Power") {
        if (target == "base") {
            if (Pin* pin = node.FindInput("A")) return pin;
        }
        if (target == "exp" || target == "exponent") {
            if (Pin* pin = node.FindInput("B")) return pin;
        }
    }
    if (target == "coordinates" || target == "coordinate" || target == "uv" ||
        target == "uvs") {
        for (const char* candidate : {"UVs", "UV", "Coordinate"}) {
            if (Pin* pin = node.FindInput(candidate)) return pin;
        }
    }
    if (target == "textureobject" || target == "tex" || target == "texture2d") {
        if (Pin* pin = node.FindInput("Texture")) return pin;
    }
    if ((target == "input" || target == "value") && node.inputs.size() == 1) {
        return &node.inputs.front();
    }
    if (node.inputs.size() == 1) return &node.inputs.front();
    return nullptr;
}

Pin* FindOutputPin(Node& node, const std::string& name) {
    const std::string target = NormalizePinName(name);
    for (Pin& pin : node.outputs) {
        if (NormalizePinName(pin.name) == target) return &pin;
    }
    if (target.empty()) {
        if (node.outputs.size() == 1) return &node.outputs.front();
        for (const char* candidate : {"Value", "Result", "Output"}) {
            if (Pin* pin = node.FindOutput(candidate)) return pin;
        }
    }
    if (target == "output" || target == "result" || target == "value" || target.empty()) {
        for (const char* candidate : {"Value", "Result", "Output"}) {
            if (Pin* pin = node.FindOutput(candidate)) return pin;
        }
    }
    if (node.outputs.size() == 1) return &node.outputs.front();
    for (Pin& pin : node.outputs) {
        if (!pin.IsSplitComponent() && !pin.hidden) return &pin;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Import implementation
// ---------------------------------------------------------------------------

struct ParsedNode {
    const RawObject* raw = nullptr;
    std::string graphName;
    std::string expressionClass;
    std::string type;
    const NodeDefinition* definition = nullptr;
    std::map<std::string, std::string> props;
    Vec2 position{};
    Vec2 size{320.0f, 180.0f};
    bool isRoot = false;
    bool isComment = false;
    bool hasPosition = false;
    Node* node = nullptr;
};

std::string GraphNodeName(const RawObject& object) {
    const auto name = object.props.find("Name");
    if (name != object.props.end()) return Unquote(name->second);
    return {};
}

bool TryGetPosition(const std::map<std::string, std::string>& props, float* x, float* y) {
    const std::string* px = FindProp(props, {"NodePosX"});
    const std::string* py = FindProp(props, {"NodePosY"});
    if (!px && !py) {
        px = FindProp(props, {"MaterialExpressionEditorX"});
        py = FindProp(props, {"MaterialExpressionEditorY"});
    }
    if (!px && !py) return false;
    float parsedX = 0.0f;
    float parsedY = 0.0f;
    if (px) ParseFloatValue(*px, &parsedX);
    if (py) ParseFloatValue(*py, &parsedY);
    if (x) *x = parsedX;
    if (y) *y = parsedY;
    return true;
}

} // namespace

bool LooksLikeUe5MaterialClipboard(const std::string& text) {
    if (text.size() < 16) return false;
    const bool hasObjects = text.find("Begin Object") != std::string::npos;
    const bool hasMaterialGraph = text.find("MaterialGraphNode") != std::string::npos ||
                                  text.find("MaterialExpression") != std::string::npos;
    return hasObjects && hasMaterialGraph;
}

bool ImportUe5MaterialClipboard(const std::string& text, MaterialGraph& graph,
                                const Vec2& origin, Ue5ClipboardImportResult* result) {
    Ue5ClipboardImportResult local;
    Ue5ClipboardImportResult& output = result ? *result : local;
    output = Ue5ClipboardImportResult{};

    if (!LooksLikeUe5MaterialClipboard(text)) {
        output.error = "The clipboard does not contain a UE4/UE5 material node export.";
        return false;
    }

    std::vector<RawObject> objects;
    std::string parseError;
    if (!ParseTopLevelObjects(text, &objects, &parseError)) {
        output.error = parseError.empty() ? "Could not parse the UE5 material clipboard text."
                                          : parseError;
        return false;
    }
    if (objects.empty()) {
        output.error = "The clipboard contains no material graph objects.";
        return false;
    }

    std::vector<ParsedNode> parsed;
    std::vector<std::string> skippedClasses;
    int skippedCount = 0;
    const auto noteSkipped = [&](const std::string& className) {
        ++skippedCount;
        const std::string shortName = ShortClassName(className);
        if (std::find(skippedClasses.begin(), skippedClasses.end(), shortName) ==
            skippedClasses.end()) {
            skippedClasses.push_back(shortName);
        }
    };

    for (const RawObject& object : objects) {
        const auto classIt = object.props.find("Class");
        const std::string outerClass = classIt == object.props.end()
                                           ? object.className
                                           : Unquote(classIt->second);
        if (outerClass.find("MaterialGraphNode") == std::string::npos &&
            object.className.find("MaterialGraphNode") == std::string::npos) {
            continue;
        }

        ParsedNode node;
        node.raw = &object;
        node.graphName = GraphNodeName(object);
        if (node.graphName.empty()) continue;
        node.isRoot = outerClass.find("MaterialGraphNode_Root") != std::string::npos;

        // Copy the node properties first, then merge the expression object's
        // properties. The expression object lives in a nested Begin Object
        // block in UE4 exports and directly inside the graph node in UE5.
        node.props = object.props;
        for (const RawObject& child : object.children) {
            for (const auto& [key, value] : child.props) {
                if (key == "Class" || key == "Name" || key == "ExportPath") continue;
                node.props[key] = value;
            }
            if (child.className.find("MaterialExpression") != std::string::npos) {
                node.expressionClass = child.className;
            }
        }
        if (node.expressionClass.empty() && object.className.find("MaterialExpression") !=
                                               std::string::npos) {
            node.expressionClass = object.className;
        }
        if (node.expressionClass.empty() && outerClass.find("MaterialGraphNode_Comment") !=
                                               std::string::npos) {
            node.expressionClass = "MaterialExpressionComment";
        }
        if (!node.isRoot && node.expressionClass.empty()) {
            noteSkipped(outerClass);
            continue;
        }

        if (!node.isRoot) {
            node.isComment = node.expressionClass.find("MaterialExpressionComment") !=
                             std::string::npos;
            if (node.isComment) {
                node.type = "Comment";
                node.definition = NodeRegistry::Get().Find("Comment");
                if (const std::string* width = FindProp(node.props, {"NodeWidth"})) {
                    float parsedWidth = node.size.x;
                    if (ParseFloatValue(*width, &parsedWidth) && parsedWidth > 8.0f) {
                        node.size.x = parsedWidth;
                    }
                }
                if (const std::string* height = FindProp(node.props, {"NodeHeight"})) {
                    float parsedHeight = node.size.y;
                    if (ParseFloatValue(*height, &parsedHeight) && parsedHeight > 8.0f) {
                        node.size.y = parsedHeight;
                    }
                }
            } else {
                node.definition = LookupDefinition(node.expressionClass, &node.type);
            }
            if (!node.definition || node.definition->isFunctionOnly) {
                noteSkipped(node.expressionClass);
                continue;
            }
            if (node.type == "MaterialFunctionCall") {
                // Material function calls need a local .mffunction asset and
                // dynamically generated pins; importing the bare UE node would
                // produce a disconnected placeholder, so report it instead.
                noteSkipped(node.expressionClass);
                continue;
            }
            float x = 0.0f;
            float y = 0.0f;
            node.hasPosition = TryGetPosition(node.props, &x, &y);
            node.position = Vec2(x, y);
            if (node.definition->isMaterialOutput) {
                // The material root is handled separately and is never copied
                // as a regular node.
                noteSkipped(node.expressionClass);
                continue;
            }
        }
        parsed.push_back(std::move(node));
    }

    if (parsed.empty()) {
        output.error = "No supported UE5 material expressions were found in the clipboard.";
        output.skippedNodeCount = skippedCount;
        output.skippedClasses = std::move(skippedClasses);
        return false;
    }

    // Keep the UE layout but place the imported block at the requested origin.
    float minX = 0.0f;
    float minY = 0.0f;
    bool haveBounds = false;
    int fallbackIndex = 0;
    for (ParsedNode& node : parsed) {
        if (node.isRoot) continue;
        if (!node.hasPosition) {
            node.position = Vec2(static_cast<float>(fallbackIndex % 4) * 60.0f,
                                 static_cast<float>(fallbackIndex / 4) * 140.0f);
        }
        ++fallbackIndex;
        if (!haveBounds) {
            minX = node.position.x;
            minY = node.position.y;
            haveBounds = true;
        } else {
            minX = std::min(minX, node.position.x);
            minY = std::min(minY, node.position.y);
        }
    }

    Node* materialOutput = graph.FindMaterialOutput();
    bool hasRoot = false;
    for (const ParsedNode& node : parsed) {
        if (node.isRoot) {
            hasRoot = true;
            break;
        }
    }
    if (hasRoot && !materialOutput) {
        if (const NodeDefinition* outputDefinition = NodeRegistry::Get().Find("MaterialOutput")) {
            materialOutput = &graph.AddNode(*outputDefinition, Vec2(origin.x, origin.y));
        }
    }

    std::unordered_map<std::string, ParsedNode*> parsedByGraphName;
    for (ParsedNode& node : parsed) {
        parsedByGraphName[node.graphName] = &node;
    }

    int addedNodes = 0;
    for (ParsedNode& node : parsed) {
        if (node.isRoot) {
            node.node = materialOutput;
            continue;
        }
        const Vec2 placement(origin.x + (node.position.x - minX),
                             origin.y + (node.position.y - minY));
        node.node = &graph.AddNode(*node.definition, placement);
        ApplyProperties(*node.node, node.props);
        if (node.isComment) node.node->size = node.size;
        output.createdNodes.push_back(node.node->id);
        ++addedNodes;

        // UE's TextureSample has an internal texture asset while
        // MaterialForge represents it as TextureObject -> TextureSample. The
        // TextureObject is created after links have been imported so an
        // explicit upstream TextureObject wins.
    }

    // Pin lookup for both ends of a link. Pins carry their owning graph node
    // name and a GUID; LinkedTo refers to that pair.
    struct PinRef {
        ParsedNode* node = nullptr;
        std::string name;
    };
    std::map<std::string, PinRef> pinsById;
    for (ParsedNode& node : parsed) {
        for (const RawPin& pin : node.raw->pins) {
            if (pin.id.empty()) continue;
            PinRef ref;
            ref.node = &node;
            ref.name = pin.name;
            pinsById[node.graphName + "|" + ToLower(pin.id)] = std::move(ref);
        }
    }

    const auto connectPins = [&](ParsedNode* from, const std::string& fromPinName,
                                 ParsedNode* to, const std::string& toPinName) {
        if (!from || !to || !from->node || !to->node) return;
        Node* targetNode = to->node;
        if (to->isRoot) targetNode = materialOutput;
        if (!targetNode) return;
        Pin* outputPin = FindOutputPin(*from->node, fromPinName);
        Pin* inputPin = FindInputPin(*targetNode, toPinName);
        if (!outputPin || !inputPin) {
            return;
        }
        graph.Connect(outputPin->id, inputPin->id);
    };

    int addedLinks = 0;
    std::set<std::string> visitedLinks;
    std::set<std::string> droppedRootLinks;
    for (ParsedNode& node : parsed) {
        for (const RawPin& pin : node.raw->pins) {
            if (pin.links.empty()) continue;
            if (pin.output) {
                for (const auto& [targetNodeName, targetPinId] : pin.links) {
                    const auto target = pinsById.find(targetNodeName + "|" +
                                                      ToLower(targetPinId));
                    if (target == pinsById.end()) {
                        if (targetNodeName.find("MaterialGraphNode_Root") !=
                            std::string::npos) {
                            droppedRootLinks.insert(node.graphName + "|" + pin.name +
                                                    "->" + targetNodeName + "|" +
                                                    targetPinId);
                        }
                        continue;
                    }
                    ParsedNode* targetNode = target->second.node;
                    if (!targetNode) continue;
                    const std::string key = node.graphName + "|" + pin.name + "->" +
                                            targetNodeName + "|" + target->second.name;
                    if (!visitedLinks.insert(key).second) continue;
                    const std::size_t before = graph.Links().size();
                    connectPins(&node, pin.name, targetNode, target->second.name);
                    if (graph.Links().size() > before) ++addedLinks;
                }
            } else if (node.isRoot) {
                for (const auto& [sourceNodeName, sourcePinId] : pin.links) {
                    const auto source = pinsById.find(sourceNodeName + "|" +
                                                      ToLower(sourcePinId));
                    if (source == pinsById.end()) continue;
                    ParsedNode* sourceNode = source->second.node;
                    if (!sourceNode || sourceNode->isRoot) continue;
                    const std::string key = sourceNodeName + "|" + source->second.name +
                                            "->" + node.graphName + "|" + pin.name;
                    if (!visitedLinks.insert(key).second) continue;
                    const std::size_t before = graph.Links().size();
                    connectPins(sourceNode, source->second.name, &node, pin.name);
                    if (graph.Links().size() > before) ++addedLinks;
                }
            } else {
                // Also consume the input-side LinkedTo entries. Depending on
                // the engine version the exporter may only fill one end.
                for (const auto& [sourceNodeName, sourcePinId] : pin.links) {
                    const auto source = pinsById.find(sourceNodeName + "|" +
                                                      ToLower(sourcePinId));
                    if (source == pinsById.end()) continue;
                    ParsedNode* sourceNode = source->second.node;
                    if (!sourceNode || sourceNode->isRoot) continue;
                    const std::string key = sourceNodeName + "|" + source->second.name +
                                            "->" + node.graphName + "|" + pin.name;
                    if (!visitedLinks.insert(key).second) continue;
                    const std::size_t before = graph.Links().size();
                    connectPins(sourceNode, source->second.name, &node, pin.name);
                    if (graph.Links().size() > before) ++addedLinks;
                }
            }
        }
    }

    // Apply unconnected input defaults after connecting so their values match
    // the UE node state. The material root's pin defaults are applied too.
    for (ParsedNode& node : parsed) {
        for (const RawPin& rawPin : node.raw->pins) {
            if (rawPin.output || !rawPin.hasDefaultValue) continue;
            if (node.isRoot) {
                if (!materialOutput) continue;
                if (Pin* inputPin = FindInputPin(*materialOutput, rawPin.name)) {
                    ApplyPinDefault(*inputPin, rawPin.defaultValue);
                }
            } else if (node.node) {
                if (Pin* inputPin = FindInputPin(*node.node, rawPin.name)) {
                    ApplyPinDefault(*inputPin, rawPin.defaultValue);
                }
            }
        }
    }

    // TextureSample nodes in UE reference a Texture2D asset directly. Add the
    // matching TextureObject node if the copy did not include one.
    for (ParsedNode& node : parsed) {
        if (node.isRoot || !node.node || node.type != "TextureSample") continue;
        const std::string* textureRaw = FindProp(node.props, {"Texture", "Texture2D"});
        if (!textureRaw) continue;
        const std::string texturePath = ExtractAssetPath(*textureRaw);
        if (texturePath.empty()) continue;
        Pin* textureInput = node.node->FindInput("Texture");
        if (textureInput && graph.FindInputLink(textureInput->id)) continue;
        const NodeDefinition* textureObjectDefinition =
            NodeRegistry::Get().Find("TextureObject");
        if (!textureObjectDefinition) continue;
        Node& textureObject = graph.AddNode(
            *textureObjectDefinition,
            Vec2(node.node->position.x - 280.0f, node.node->position.y + 32.0f));
        SetStringProperty(textureObject, "texture", texturePath);
        Pin* sourcePin = textureObject.FindOutput("Texture");
        if (sourcePin && textureInput) {
            const std::size_t before = graph.Links().size();
            graph.Connect(sourcePin->id, textureInput->id);
            if (graph.Links().size() > before) ++addedLinks;
        }
        output.createdNodes.push_back(textureObject.id);
        ++addedNodes;
    }

    graph.MarkContentChanged();
    output.success = addedNodes > 0;
    output.nodeCount = addedNodes;
    output.linkCount = addedLinks;
    output.skippedNodeCount = skippedCount;
    output.droppedMaterialOutputLinks = static_cast<int>(droppedRootLinks.size());
    output.skippedClasses = std::move(skippedClasses);
    if (!output.success) {
        output.error = "No supported UE5 material expressions were found in the clipboard.";
        return false;
    }
    return true;
}

} // namespace mf
