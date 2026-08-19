#include "UserPresetStore.h"

#include <algorithm>
#include <utility>

namespace smart_ducking
{
namespace
{
constexpr auto userPresetRootType = "SmartDuckingUserPreset";
constexpr auto presetStateType = "SmartDuckingPreset";
constexpr auto presetExtension = ".smartduckingpreset";
}

UserPresetStore::UserPresetStore (juce::File directory)
    : presetDirectory (std::move (directory))
{
}

juce::File UserPresetStore::defaultDirectory()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("RusAD")
        .getChildFile ("Smart Ducking")
        .getChildFile ("Presets");
}

juce::File UserPresetStore::fileForName (const juce::String& name) const
{
    auto legalName = juce::File::createLegalFileName (name.trim()).substring (0, 48);
    if (legalName.isEmpty())
        legalName = "Preset";

    const auto nameHash = juce::String::toHexString (name.trim().hashCode64());
    return presetDirectory.getChildFile (legalName + "-" + nameHash + presetExtension);
}

std::vector<UserPreset> UserPresetStore::loadAll() const
{
    std::vector<UserPreset> result;
    if (! presetDirectory.isDirectory())
        return result;

    const auto files = presetDirectory.findChildFiles (
        juce::File::findFiles, false, "*" + juce::String (presetExtension));

    for (const auto& file : files)
    {
        const auto xml = juce::XmlDocument::parse (file);
        if (xml == nullptr || ! xml->hasTagName (userPresetRootType))
            continue;

        const auto root = juce::ValueTree::fromXml (*xml);
        const auto name = root.getProperty ("name").toString().trim();
        const auto state = root.getChildWithName (presetStateType);
        if (name.isEmpty() || ! state.isValid())
            continue;

        result.push_back ({ name, state.createCopy(), file });
    }

    std::sort (result.begin(), result.end(), [] (const auto& left, const auto& right)
    {
        return left.name.compareNatural (right.name) < 0;
    });
    return result;
}

juce::Result UserPresetStore::save (const juce::String& requestedName,
                                    const juce::ValueTree& presetState) const
{
    const auto name = requestedName.trim();
    if (name.isEmpty())
        return juce::Result::fail ("Preset name cannot be empty.");

    if (! presetState.isValid() || ! presetState.hasType (presetStateType))
        return juce::Result::fail ("Preset data is invalid.");

    const auto directoryResult = presetDirectory.createDirectory();
    if (directoryResult.failed())
        return directoryResult;

    juce::ValueTree root (userPresetRootType);
    root.setProperty ("formatVersion", 1, nullptr);
    root.setProperty ("name", name, nullptr);
    root.addChild (presetState.createCopy(), -1, nullptr);

    const auto xml = root.createXml();
    if (xml == nullptr)
        return juce::Result::fail ("Could not serialise the preset.");

    const auto destination = fileForName (name);
    juce::TemporaryFile temporary (destination);
    if (! temporary.getFile().replaceWithText (xml->toString()))
        return juce::Result::fail ("Could not write the preset file.");

    if (! temporary.overwriteTargetFileWithTemporary())
        return juce::Result::fail ("Could not replace the preset file.");

    return juce::Result::ok();
}

bool UserPresetStore::remove (const UserPreset& preset) const
{
    return preset.file.isAChildOf (presetDirectory)
        && preset.file.hasFileExtension (presetExtension)
        && preset.file.deleteFile();
}
} // namespace smart_ducking
