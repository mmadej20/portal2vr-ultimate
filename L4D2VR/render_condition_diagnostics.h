#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

enum class RenderCondition : std::size_t
{
    Bridge, BackBufferCapture, BackBufferImage, BackBufferDimensions, BackBufferDevice,
    RenderContext, Allocation, RetryBudget, MenuSubmit, StereoSubmit,
    HudSubmit, PresentOwnership, LeftShare, RightShare, HudShare, BlankShare,
    ResourceDrain, MenuDetach, HudDetach, Count
};

enum class RenderConditionChange { None, Failure, DetailsChanged, Recovery, Ready };

// Confined to the existing render/device serialization. Checks use fixed-size
// values and do not allocate, format strings, or acquire diagnostic locks.
class RenderConditionDiagnostics
{
public:
    using Details = std::array<std::uint64_t, 8>;
    void SetVerbose(bool verbose, std::uint64_t nowMilliseconds)
    {
        if (verbose && !m_Verbose)
            for (auto& state : m_Conditions) state.nextVerbose = 0;
        m_Verbose = verbose;
        m_NowMilliseconds = nowMilliseconds;
    }
    RenderConditionChange Observe(RenderCondition condition, bool failed,
                                  Details details = {}, bool reportReady = false)
    {
        auto& previous = m_Conditions[static_cast<std::size_t>(condition)];
        RenderConditionChange result = RenderConditionChange::None;
        if (!previous.seen)
            result = failed ? RenderConditionChange::Failure :
                reportReady || m_Verbose ? RenderConditionChange::Ready : RenderConditionChange::None;
        else if (previous.failed != failed)
            result = failed ? RenderConditionChange::Failure : RenderConditionChange::Recovery;
        else if (previous.details != details && (failed || reportReady || m_Verbose))
            result = RenderConditionChange::DetailsChanged;
        else if (m_Verbose && m_NowMilliseconds >= previous.nextVerbose)
            result = RenderConditionChange::DetailsChanged;
        auto nextVerbose = previous.nextVerbose;
        if (result != RenderConditionChange::None) {
            constexpr std::uint64_t interval = 5000;
            constexpr auto maximum = (std::numeric_limits<std::uint64_t>::max)();
            nextVerbose = m_NowMilliseconds > maximum - interval ? maximum : m_NowMilliseconds + interval;
        }
        previous = {true, failed, details, nextVerbose};
        return result;
    }
    void NewGeneration() { m_Conditions = {}; }

private:
    struct State { bool seen = false; bool failed = false; Details details{}; std::uint64_t nextVerbose = 0; };
    std::array<State, static_cast<std::size_t>(RenderCondition::Count)> m_Conditions{};
    bool m_Verbose = false;
    std::uint64_t m_NowMilliseconds = 0;
};
