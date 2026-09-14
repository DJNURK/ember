#include "dsp/styles/SaturationStyle.h"
#include "dsp/styles/TubeStyles.h"
#include "dsp/styles/TapeStyles.h"
#include "dsp/styles/AmpStyles.h"
#include "dsp/styles/RectifyStyles.h"
#include "dsp/styles/DestroyStyles.h"
#include "dsp/styles/FxStyles.h"

namespace ember
{
std::unique_ptr<SaturationStyle> createSaturationStyle(StyleID id)
{
    switch (id)
    {
        case StyleID::CleanTube:   return std::make_unique<CleanTubeStyle>();
        case StyleID::WarmTube:    return std::make_unique<WarmTubeStyle>();
        case StyleID::SubtleTube:  return std::make_unique<SubtleTubeStyle>();
        case StyleID::BrokenTube:  return std::make_unique<BrokenTubeStyle>();

        case StyleID::CleanTape:   return std::make_unique<CleanTapeStyle>();
        case StyleID::WarmTape:    return std::make_unique<WarmTapeStyle>();
        case StyleID::BrightTape:  return std::make_unique<BrightTapeStyle>();
        case StyleID::Transformer: return std::make_unique<TransformerStyle>();

        case StyleID::CleanAmp:    return std::make_unique<CleanAmpStyle>();
        case StyleID::Crunch:      return std::make_unique<CrunchStyle>();
        case StyleID::Lead:        return std::make_unique<LeadStyle>();

        case StyleID::Smudge:      return std::make_unique<SmudgeStyle>();
        case StyleID::Rectify:     return std::make_unique<RectifyStyle>();

        case StyleID::Foldback:    return std::make_unique<FoldbackStyle>();
        case StyleID::HardClip:    return std::make_unique<HardClipStyle>();
        case StyleID::Decimate:    return std::make_unique<DecimateStyle>();
        case StyleID::Bitcrush:    return std::make_unique<BitcrushStyle>();

        case StyleID::Shimmer:     return std::make_unique<ShimmerStyle>();
        case StyleID::Breathe:     return std::make_unique<BreatheStyle>();

        case StyleID::Count:
        default:                   return std::make_unique<CleanTubeStyle>();
    }
}

const char* getStyleName(StyleID id) noexcept
{
    switch (id)
    {
        case StyleID::CleanTube:   return "Clean Tube";
        case StyleID::WarmTube:    return "Warm Tube";
        case StyleID::SubtleTube:  return "Subtle Tube";
        case StyleID::BrokenTube:  return "Broken Tube";
        case StyleID::CleanTape:   return "Clean Tape";
        case StyleID::WarmTape:    return "Warm Tape";
        case StyleID::BrightTape:  return "Bright Tape";
        case StyleID::Transformer: return "Transformer";
        case StyleID::CleanAmp:    return "Clean Amp";
        case StyleID::Crunch:      return "Crunch";
        case StyleID::Lead:        return "Lead";
        case StyleID::Smudge:      return "Smudge";
        case StyleID::Rectify:     return "Rectify";
        case StyleID::Foldback:    return "Foldback";
        case StyleID::HardClip:    return "Hard Clip";
        case StyleID::Decimate:    return "Decimate";
        case StyleID::Bitcrush:    return "Bitcrush";
        case StyleID::Shimmer:     return "Shimmer";
        case StyleID::Breathe:     return "Breathe";
        case StyleID::Count:
        default:                   return "Clean Tube";
    }
}

StyleCategory getStyleCategory(StyleID id) noexcept
{
    switch (id)
    {
        case StyleID::CleanTube: case StyleID::WarmTube:
        case StyleID::SubtleTube: case StyleID::BrokenTube:  return StyleCategory::Tube;
        case StyleID::CleanTape: case StyleID::WarmTape:
        case StyleID::BrightTape:                            return StyleCategory::Tape;
        case StyleID::Transformer:                           return StyleCategory::Transformer;
        case StyleID::CleanAmp: case StyleID::Crunch:
        case StyleID::Lead:                                  return StyleCategory::Amp;
        case StyleID::Smudge: case StyleID::Rectify:         return StyleCategory::Rectify;
        case StyleID::Foldback: case StyleID::HardClip:
        case StyleID::Decimate:                              return StyleCategory::Destroy;
        case StyleID::Bitcrush:                              return StyleCategory::Crush;
        case StyleID::Shimmer: case StyleID::Breathe:        return StyleCategory::FX;
        case StyleID::Count:
        default:                                             return StyleCategory::Tube;
    }
}

const char* getCategoryName(StyleCategory c) noexcept
{
    switch (c)
    {
        case StyleCategory::Tube:        return "Tube";
        case StyleCategory::Tape:        return "Tape";
        case StyleCategory::Transformer: return "Transformer";
        case StyleCategory::Amp:         return "Amp";
        case StyleCategory::Rectify:     return "Rectify";
        case StyleCategory::Destroy:     return "Destroy";
        case StyleCategory::Crush:       return "Crush";
        case StyleCategory::FX:          return "FX";
        case StyleCategory::Count:
        default:                         return "Tube";
    }
}
} // namespace ember
