#pragma once

#include <vector>

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

namespace smart_ducking
{
struct UserPreset
{
    juce::String name;
    juce::ValueTree state;
    juce::File file;
};

class UserPresetStore
{
public:
    explicit UserPresetStore (juce::File directory = defaultDirectory());

    [[nodiscard]] std::vector<UserPreset> loadAll() const;
    [[nodiscard]] juce::Result save (const juce::String& name,
                                     const juce::ValueTree& presetState) const;
    [[nodiscard]] bool remove (const UserPreset& preset) const;

    [[nodiscard]] const juce::File& getDirectory() const noexcept { return presetDirectory; }
    [[nodiscard]] static juce::File defaultDirectory();

private:
    [[nodiscard]] juce::File fileForName (const juce::String& name) const;

    juce::File presetDirectory;
};
} // namespace smart_ducking
