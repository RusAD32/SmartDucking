#pragma once

namespace smart_ducking
{
enum class PluginVariant
{
    stereo,
    surround
};

PluginVariant exchangeRequestedPluginVariant (PluginVariant) noexcept;
[[nodiscard]] PluginVariant getRequestedPluginVariant() noexcept;
} // namespace smart_ducking
