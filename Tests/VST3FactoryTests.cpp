#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstmessage.h>

#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
 #include <windows.h>
#elif defined(__APPLE__)
 #include <CoreFoundation/CoreFoundation.h>
#elif defined(__linux__) || defined(__FreeBSD__)
 #include <dlfcn.h>
#endif

namespace
{
using GetPluginFactoryFn = Steinberg::IPluginFactory* (PLUGIN_API*)();

class Vst3Module final
{
public:
    bool open (const std::string& modulePath)
    {
       #if defined(_WIN32)
        handle = LoadLibraryW (std::filesystem::path (modulePath).wstring().c_str());

        if (handle == nullptr)
        {
            std::cerr << "LoadLibrary failed for " << modulePath << '\n';
            return false;
        }

        entry = reinterpret_cast<ModuleEntryFn> (GetProcAddress (handle, "InitDll"));
        exit = reinterpret_cast<ModuleExitFn> (GetProcAddress (handle, "ExitDll"));
        getFactory = reinterpret_cast<GetPluginFactoryFn> (
            GetProcAddress (handle, "GetPluginFactory"));

        if (entry == nullptr || exit == nullptr || getFactory == nullptr || ! entry())
        {
            std::cerr << "The VST3 entry points could not be initialised\n";
            close();
            return false;
        }

       #elif defined(__APPLE__)
        const auto bundleDirectory = std::filesystem::path (modulePath)
                                         .parent_path()
                                         .parent_path()
                                         .parent_path();
        const auto bundlePath = bundleDirectory.string();
        auto* url = CFURLCreateFromFileSystemRepresentation (
            kCFAllocatorDefault,
            reinterpret_cast<const UInt8*> (bundlePath.c_str()),
            static_cast<CFIndex> (bundlePath.size()),
            true);

        if (url != nullptr)
        {
            handle = CFBundleCreate (kCFAllocatorDefault, url);
            CFRelease (url);
        }

        if (handle == nullptr || ! CFBundleLoadExecutable (handle))
        {
            std::cerr << "CFBundle could not load " << bundleDirectory << '\n';
            close();
            return false;
        }

        entry = reinterpret_cast<ModuleEntryFn> (
            CFBundleGetFunctionPointerForName (handle, CFSTR ("bundleEntry")));
        exit = reinterpret_cast<ModuleExitFn> (
            CFBundleGetFunctionPointerForName (handle, CFSTR ("bundleExit")));
        getFactory = reinterpret_cast<GetPluginFactoryFn> (
            CFBundleGetFunctionPointerForName (handle, CFSTR ("GetPluginFactory")));

        if (entry == nullptr || exit == nullptr || getFactory == nullptr || ! entry (handle))
        {
            std::cerr << "The VST3 bundle entry points could not be initialised\n";
            close();
            return false;
        }
       #elif defined(__linux__) || defined(__FreeBSD__)
        handle = dlopen (modulePath.c_str(), RTLD_NOW | RTLD_LOCAL);

        if (handle == nullptr)
        {
            std::cerr << "dlopen failed for " << modulePath << ": " << dlerror() << '\n';
            return false;
        }

        entry = reinterpret_cast<ModuleEntryFn> (dlsym (handle, "ModuleEntry"));
        exit = reinterpret_cast<ModuleExitFn> (dlsym (handle, "ModuleExit"));
        getFactory = reinterpret_cast<GetPluginFactoryFn> (
            dlsym (handle, "GetPluginFactory"));

        if (entry == nullptr || exit == nullptr || getFactory == nullptr || ! entry (handle))
        {
            std::cerr << "The Linux VST3 entry points could not be initialised\n";
            close();
            return false;
        }
       #else
        (void) modulePath;
        return false;
       #endif

        initialised = true;
        return true;
    }

    Steinberg::IPluginFactory* createFactory() const
    {
        return getFactory != nullptr ? getFactory() : nullptr;
    }

    bool close()
    {
        auto ok = true;

        if (initialised && exit != nullptr)
            ok = exit();

        initialised = false;
        entry = nullptr;
        exit = nullptr;
        getFactory = nullptr;

       #if defined(_WIN32)
        if (handle != nullptr)
            FreeLibrary (std::exchange (handle, nullptr));
       #elif defined(__APPLE__)
        if (handle != nullptr)
        {
            CFBundleUnloadExecutable (handle);
            CFRelease (handle);
            handle = nullptr;
        }
       #elif defined(__linux__) || defined(__FreeBSD__)
        if (handle != nullptr)
        {
            dlclose (handle);
            handle = nullptr;
        }
       #endif

        return ok;
    }

    ~Vst3Module()
    {
        close();
    }

private:
   #if defined(_WIN32)
    using ModuleEntryFn = bool (*)();
    using ModuleExitFn = bool (*)();
    HMODULE handle = nullptr;
   #elif defined(__APPLE__)
    using ModuleEntryFn = bool (*) (CFBundleRef);
    using ModuleExitFn = bool (*)();
    CFBundleRef handle = nullptr;
   #elif defined(__linux__) || defined(__FreeBSD__)
    using ModuleEntryFn = bool (*) (void*);
    using ModuleExitFn = bool (*)();
    void* handle = nullptr;
   #endif

    ModuleEntryFn entry = nullptr;
    ModuleExitFn exit = nullptr;
    GetPluginFactoryFn getFactory = nullptr;
    bool initialised = false;
};

struct ExpectedPlugin
{
    std::string name;
    Steinberg::int32 mainChannels;
    Steinberg::int32 voiceChannels;
    Steinberg::int32 outputChannels;
};

bool checkComponentBuses (Steinberg::IPluginFactory& factory,
                          const Steinberg::PClassInfo2& classInfo,
                          const ExpectedPlugin& expected)
{
    void* object = nullptr;

    if (factory.createInstance (classInfo.cid, Steinberg::Vst::IComponent::iid, &object)
            != Steinberg::kResultOk
        || object == nullptr)
    {
        std::cerr << "Could not instantiate " << expected.name << '\n';
        return false;
    }

    auto* component = static_cast<Steinberg::Vst::IComponent*> (object);
    bool ok = true;

    if (component->initialize (nullptr) != Steinberg::kResultOk)
    {
        std::cerr << expected.name << " could not be initialized\n";
        component->release();
        return false;
    }

    Steinberg::TUID controllerCid {};
    void* controllerObject = nullptr;

    if (component->getControllerClassId (controllerCid) != Steinberg::kResultOk
        || factory.createInstance (controllerCid,
                                   Steinberg::Vst::IEditController::iid,
                                   &controllerObject) != Steinberg::kResultOk
        || controllerObject == nullptr)
    {
        std::cerr << expected.name << " does not expose an edit controller\n";
        ok = false;
    }
    else
    {
        auto* controller = static_cast<Steinberg::Vst::IEditController*> (controllerObject);

        if (controller->initialize (nullptr) != Steinberg::kResultOk)
        {
            std::cerr << expected.name << " edit controller could not be initialized\n";
            ok = false;
        }
        else
        {
            void* componentConnectionObject = nullptr;
            void* controllerConnectionObject = nullptr;
            const auto haveConnections = component->queryInterface (
                    Steinberg::Vst::IConnectionPoint::iid,
                    &componentConnectionObject) == Steinberg::kResultOk
                && componentConnectionObject != nullptr
                && controller->queryInterface (
                    Steinberg::Vst::IConnectionPoint::iid,
                    &controllerConnectionObject) == Steinberg::kResultOk
                && controllerConnectionObject != nullptr;
            auto* componentConnection = static_cast<Steinberg::Vst::IConnectionPoint*> (
                componentConnectionObject);
            auto* controllerConnection = static_cast<Steinberg::Vst::IConnectionPoint*> (
                controllerConnectionObject);

            if (! haveConnections
                || componentConnection->connect (controllerConnection) != Steinberg::kResultOk
                || controllerConnection->connect (componentConnection) != Steinberg::kResultOk)
            {
                std::cerr << expected.name << " component/controller connection failed\n";
                ok = false;
            }

            auto bypassParameterCount = 0;

            for (Steinberg::int32 parameterIndex = 0;
                 parameterIndex < controller->getParameterCount();
                 ++parameterIndex)
            {
                Steinberg::Vst::ParameterInfo parameterInfo {};

                if (controller->getParameterInfo (parameterIndex, parameterInfo)
                        == Steinberg::kResultOk
                    && (parameterInfo.flags & Steinberg::Vst::ParameterInfo::kIsBypass) != 0)
                    ++bypassParameterCount;
            }

            if (bypassParameterCount != 1)
            {
                std::cerr << expected.name << " exposes " << bypassParameterCount
                          << " standard VST3 bypass parameters instead of one\n";
                ok = false;
            }

            if (componentConnection != nullptr && controllerConnection != nullptr)
            {
                controllerConnection->disconnect (componentConnection);
                componentConnection->disconnect (controllerConnection);
            }

            if (controllerConnection != nullptr)
                controllerConnection->release();

            if (componentConnection != nullptr)
                componentConnection->release();

            controller->terminate();
        }

        controller->release();
    }

    if (component->getBusCount (Steinberg::Vst::kAudio, Steinberg::Vst::kInput) != 2
        || component->getBusCount (Steinberg::Vst::kAudio, Steinberg::Vst::kOutput) != 1)
    {
        std::cerr << expected.name << " has an unexpected bus count\n";
        ok = false;
    }

    Steinberg::Vst::BusInfo mainInfo {};
    Steinberg::Vst::BusInfo voiceInfo {};
    Steinberg::Vst::BusInfo outputInfo {};

    const auto gotBusInfo = component->getBusInfo (Steinberg::Vst::kAudio,
                                                   Steinberg::Vst::kInput,
                                                   0,
                                                   mainInfo) == Steinberg::kResultOk
                         && component->getBusInfo (Steinberg::Vst::kAudio,
                                                   Steinberg::Vst::kInput,
                                                   1,
                                                   voiceInfo) == Steinberg::kResultOk
                         && component->getBusInfo (Steinberg::Vst::kAudio,
                                                   Steinberg::Vst::kOutput,
                                                   0,
                                                   outputInfo) == Steinberg::kResultOk;

    if (! gotBusInfo
        || mainInfo.channelCount != expected.mainChannels
        || voiceInfo.channelCount != expected.voiceChannels
        || outputInfo.channelCount != expected.outputChannels)
    {
        std::cerr << expected.name << " bus layout mismatch: got "
                  << mainInfo.channelCount << '+' << voiceInfo.channelCount << '+'
                  << outputInfo.channelCount << '\n';
        ok = false;
    }

    void* processorObject = nullptr;
    if (component->queryInterface (Steinberg::Vst::IAudioProcessor::iid,
                                   &processorObject) != Steinberg::kResultOk
        || processorObject == nullptr)
    {
        std::cerr << expected.name << " does not expose IAudioProcessor\n";
        ok = false;
    }
    else
    {
        auto* audioProcessor = static_cast<Steinberg::Vst::IAudioProcessor*> (processorObject);
        Steinberg::Vst::ProcessSetup setup {};
        setup.processMode = Steinberg::Vst::kRealtime;
        setup.symbolicSampleSize = Steinberg::Vst::kSample32;
        setup.maxSamplesPerBlock = 512;
        setup.sampleRate = 48000.0;

        if (audioProcessor->setupProcessing (setup) != Steinberg::kResultOk
            || component->setActive (true) != Steinberg::kResultOk)
        {
            std::cerr << expected.name << " could not enter active 48 kHz processing\n";
            ok = false;
        }
        else
        {
            constexpr Steinberg::uint32 expectedDefaultLatency = 2048 + 2400;
            if (audioProcessor->getLatencySamples() != expectedDefaultLatency)
            {
                std::cerr << expected.name << " reports unexpected default latency: "
                          << audioProcessor->getLatencySamples() << '\n';
                ok = false;
            }
            constexpr Steinberg::uint32 expectedTailSamples = 2 * 48000;
            if (audioProcessor->getTailSamples() != expectedTailSamples)
            {
                std::cerr << expected.name << " reports unexpected tail: "
                          << audioProcessor->getTailSamples() << '\n';
                ok = false;
            }
            component->setActive (false);
        }

        audioProcessor->release();
    }

    component->terminate();
    component->release();
    return ok;
}
} // namespace

int main (int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "Expected the VST3 module binary path\n";
        return 1;
    }

    const std::string modulePath (argv[1]);
    Vst3Module module;

    if (! module.open (modulePath))
        return 1;

    auto* factory = module.createFactory();
    bool ok = factory != nullptr;
    void* factory2Object = nullptr;

    if (factory == nullptr
        || factory->queryInterface (Steinberg::IPluginFactory2::iid, &factory2Object)
               != Steinberg::kResultOk
        || factory2Object == nullptr)
    {
        std::cerr << "The composite IPluginFactory2 is unavailable\n";
        ok = false;
    }

    auto* factory2 = static_cast<Steinberg::IPluginFactory2*> (factory2Object);
    const std::vector<ExpectedPlugin> expected {
        { "Smart Ducking (Stereo)", 2, 6, 2 },
        { "Smart Ducking (Surround)", 6, 6, 6 }
    };

    if (factory2 != nullptr)
    {
        std::vector<Steinberg::PClassInfo2> audioClasses;

        for (Steinberg::int32 index = 0; index < factory2->countClasses(); ++index)
        {
            Steinberg::PClassInfo2 info {};

            if (factory2->getClassInfo2 (index, &info) == Steinberg::kResultOk
                && std::string (info.category) == kVstAudioEffectClass)
                audioClasses.push_back (info);
        }

        if (audioClasses.size() != expected.size())
        {
            std::cerr << "Expected two audio-module classes, got " << audioClasses.size() << '\n';
            ok = false;
        }
        else
        {
            for (size_t pluginIndex = 0; pluginIndex < expected.size(); ++pluginIndex)
            {
                if (std::string (audioClasses[pluginIndex].name) != expected[pluginIndex].name)
                {
                    std::cerr << "Unexpected class name: " << audioClasses[pluginIndex].name << '\n';
                    ok = false;
                    continue;
                }

                if (std::string (audioClasses[pluginIndex].vendor) != "OnWave"
                    || std::string (audioClasses[pluginIndex].version) != "1.0.0"
                    || std::string (audioClasses[pluginIndex].subCategories)
                           .find ("Dynamics") == std::string::npos)
                {
                    std::cerr << expected[pluginIndex].name
                              << " has incorrect release metadata\n";
                    ok = false;
                }

                ok = checkComponentBuses (*factory2,
                                          audioClasses[pluginIndex],
                                          expected[pluginIndex]) && ok;
            }

            if (std::memcmp (audioClasses[0].cid,
                             audioClasses[1].cid,
                             sizeof (audioClasses[0].cid)) == 0)
            {
                std::cerr << "Stereo and Surround component IDs must be distinct\n";
                ok = false;
            }
        }
    }

    if (factory2 != nullptr)
        factory2->release();

    if (factory != nullptr)
        factory->release();

    if (! module.close())
        ok = false;

    if (ok)
        std::cout << "Composite VST3 factory exposes Stereo 2+6+2 and Surround 6+6+6\n";

    return ok ? 0 : 1;
}
