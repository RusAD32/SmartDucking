// This translation unit wraps JUCE's stock VST3 client in a small composite
// factory. Both exported audio-module classes share the JUCE controller and
// the SmartDuckingAudioProcessor implementation.

#if defined(__linux__) || defined(__FreeBSD__)
 #define JUCE_GUI_BASICS_INCLUDE_XHEADERS 1
#endif

#include "PluginProcessor.h"

#define GetPluginFactory JuceDefaultGetPluginFactory
#include <juce_audio_plugin_client/juce_audio_plugin_client_VST3.cpp>
#undef GetPluginFactory

namespace juce
{
namespace
{
constexpr Steinberg::int32 surroundPluginCode = 0x536d5372; // 'SmSr'

const Steinberg::FUID& getSurroundComponentId()
{
    static const auto id = toSteinbergUID (VST3Interface::jucePluginId (
        JucePlugin_ManufacturerCode,
        surroundPluginCode,
        VST3Interface::Type::component));
    return id;
}

class ScopedPluginVariant final
{
public:
    explicit ScopedPluginVariant (smart_ducking::PluginVariant requested) noexcept
        : previous (smart_ducking::exchangeRequestedPluginVariant (requested))
    {
    }

    ~ScopedPluginVariant()
    {
        smart_ducking::exchangeRequestedPluginVariant (previous);
    }

private:
    smart_ducking::PluginVariant previous;
};

template <typename ClassInfo>
void setClassIdentity (ClassInfo& info,
                       const Steinberg::FUID& cid,
                       const char* name)
{
    cid.toTUID (info.cid);
    Steinberg::strncpy8 (info.name, name, Steinberg::PClassInfo::kNameSize);
}
} // namespace

class SmartDuckingPluginFactory final : public Steinberg::IPluginFactory3
{
public:
    SmartDuckingPluginFactory()
    {
        if (auto* baseFactory = JuceDefaultGetPluginFactory())
        {
            void* queried = nullptr;

            if (baseFactory->queryInterface (Steinberg::IPluginFactory3::iid, &queried)
                    == Steinberg::kResultOk)
                factory = static_cast<Steinberg::IPluginFactory3*> (queried);

            baseFactory->release();
        }
    }

    ~SmartDuckingPluginFactory()
    {
        if (factory != nullptr)
            factory->release();
    }

    JUCE_DECLARE_VST3_COM_REF_METHODS

    Steinberg::tresult PLUGIN_API queryInterface (const Steinberg::TUID targetIID,
                                                   void** object) override
    {
        const auto result = testForMultiple (*this,
                                             targetIID,
                                             UniqueBase<Steinberg::IPluginFactory3> {},
                                             UniqueBase<Steinberg::IPluginFactory2> {},
                                             UniqueBase<Steinberg::IPluginFactory> {},
                                             UniqueBase<Steinberg::FUnknown> {});

        if (result.isOk())
            return result.extract (object);

        if (object != nullptr)
            *object = nullptr;

        return Steinberg::kNoInterface;
    }

    Steinberg::int32 PLUGIN_API countClasses() override
    {
        return factory != nullptr ? factory->countClasses() + 1 : 0;
    }

    Steinberg::tresult PLUGIN_API getFactoryInfo (Steinberg::PFactoryInfo* info) override
    {
        return factory != nullptr ? factory->getFactoryInfo (info) : Steinberg::kNotInitialized;
    }

    Steinberg::tresult PLUGIN_API getClassInfo (Steinberg::int32 index,
                                                Steinberg::PClassInfo* info) override
    {
        if (factory == nullptr || info == nullptr || ! isPositiveAndBelow (index, countClasses()))
            return Steinberg::kInvalidArgument;

        const auto result = factory->getClassInfo (mapToStockIndex (index), info);

        if (result != Steinberg::kResultOk)
            return result;

        rewriteComponentIdentity (index, *info);
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API getClassInfo2 (Steinberg::int32 index,
                                                 Steinberg::PClassInfo2* info) override
    {
        if (factory == nullptr || info == nullptr || ! isPositiveAndBelow (index, countClasses()))
            return Steinberg::kInvalidArgument;

        const auto result = factory->getClassInfo2 (mapToStockIndex (index), info);

        if (result != Steinberg::kResultOk)
            return result;

        rewriteComponentIdentity (index, *info);
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API getClassInfoUnicode (Steinberg::int32 index,
                                                       Steinberg::PClassInfoW* info) override
    {
        if (info == nullptr)
            return Steinberg::kInvalidArgument;

        Steinberg::PClassInfo2 asciiInfo;
        const auto result = getClassInfo2 (index, &asciiInfo);

        if (result == Steinberg::kResultOk)
            info->fromAscii (asciiInfo);

        return result;
    }

    Steinberg::tresult PLUGIN_API setHostContext (Steinberg::FUnknown* context) override
    {
        return factory != nullptr ? factory->setHostContext (context) : Steinberg::kNotInitialized;
    }

    Steinberg::tresult PLUGIN_API createInstance (Steinberg::FIDString cid,
                                                  Steinberg::FIDString sourceIid,
                                                  void** object) override
    {
        if (factory == nullptr || cid == nullptr || sourceIid == nullptr || object == nullptr)
            return Steinberg::kInvalidArgument;

        Steinberg::TUID stockComponentCid;
        JuceVST3Component::iid.toTUID (stockComponentCid);

        if (doUIDsMatch (cid, getSurroundComponentId()))
        {
            const ScopedPluginVariant scope { smart_ducking::PluginVariant::surround };
            return factory->createInstance (stockComponentCid, sourceIid, object);
        }

        if (doUIDsMatch (cid, JuceVST3Component::iid))
        {
            const ScopedPluginVariant scope { smart_ducking::PluginVariant::stereo };
            return factory->createInstance (stockComponentCid, sourceIid, object);
        }

        return factory->createInstance (cid, sourceIid, object);
    }

private:
    static Steinberg::int32 mapToStockIndex (Steinberg::int32 compositeIndex) noexcept
    {
        return compositeIndex <= 1 ? 0 : compositeIndex - 1;
    }

    template <typename ClassInfo>
    static void rewriteComponentIdentity (Steinberg::int32 index, ClassInfo& info)
    {
        if (index == 0)
            setClassIdentity (info, JuceVST3Component::iid, "Smart Ducking (Stereo)");
        else if (index == 1)
            setClassIdentity (info, getSurroundComponentId(), "Smart Ducking (Surround)");
    }

    std::atomic<int> refCount { 1 };
    Steinberg::IPluginFactory3* factory = nullptr;
};
} // namespace juce

extern "C" SMTG_EXPORT_SYMBOL Steinberg::IPluginFactory* PLUGIN_API GetPluginFactory();

extern "C" SMTG_EXPORT_SYMBOL Steinberg::IPluginFactory* PLUGIN_API GetPluginFactory()
{
    return new juce::SmartDuckingPluginFactory();
}
