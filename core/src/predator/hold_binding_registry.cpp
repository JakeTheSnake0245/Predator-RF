// Predator RF — Hold-binding registry implementation.
//
// See hold_binding_registry.h for the design contract.

#include "hold_binding_registry.h"

#include <mutex>
#include <unordered_map>

namespace predator {
namespace hold {

namespace {

std::mutex& registryMutex() {
    static std::mutex m;
    return m;
}

std::unordered_map<std::string, std::string>& bindings() {
    static std::unordered_map<std::string, std::string> m;
    return m;
}

}  // namespace

void setBoundVfoFor(const std::string& instanceName,
                    const std::string& vfoName) {
    if (instanceName.empty()) return;
    std::lock_guard<std::mutex> lk(registryMutex());
    bindings()[instanceName] = vfoName;
}

std::string getBoundVfoFor(const std::string& instanceName) {
    if (instanceName.empty()) return std::string();
    std::lock_guard<std::mutex> lk(registryMutex());
    auto it = bindings().find(instanceName);
    if (it == bindings().end()) return std::string();
    return it->second;
}

void clearBoundVfoFor(const std::string& instanceName) {
    if (instanceName.empty()) return;
    std::lock_guard<std::mutex> lk(registryMutex());
    bindings().erase(instanceName);
}

std::size_t boundInstanceCount() {
    std::lock_guard<std::mutex> lk(registryMutex());
    return bindings().size();
}

}  // namespace hold
}  // namespace predator
