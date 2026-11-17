#include "core/Node.h"

namespace mf {

Pin* Node::FindInput(const std::string& name) {
    for (Pin& pin : inputs) {
        if (pin.name == name) return &pin;
    }
    return nullptr;
}

const Pin* Node::FindInput(const std::string& name) const {
    for (const Pin& pin : inputs) {
        if (pin.name == name) return &pin;
    }
    return nullptr;
}

Pin* Node::FindOutput(const std::string& name) {
    for (Pin& pin : outputs) {
        if (pin.name == name) return &pin;
    }
    return nullptr;
}

const Pin* Node::FindOutput(const std::string& name) const {
    for (const Pin& pin : outputs) {
        if (pin.name == name) return &pin;
    }
    return nullptr;
}

Pin* Node::FindPin(PinId id) {
    for (Pin& pin : inputs) {
        if (pin.id == id) return &pin;
    }
    for (Pin& pin : outputs) {
        if (pin.id == id) return &pin;
    }
    return nullptr;
}

const Pin* Node::FindPin(PinId id) const {
    for (const Pin& pin : inputs) {
        if (pin.id == id) return &pin;
    }
    for (const Pin& pin : outputs) {
        if (pin.id == id) return &pin;
    }
    return nullptr;
}

} // namespace mf
