#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../engine/OrganModel.h"

namespace orgel::theme
{
inline const juce::Colour woodDark { 0xff2a170c };
inline const juce::Colour wood { 0xff4a2a16 };
inline const juce::Colour woodLight { 0xff6e4223 };
inline const juce::Colour gold { 0xffd4af5a };
inline const juce::Colour goldDark { 0xff8c6a2a };
inline const juce::Colour ivory { 0xfff4ecd8 };
inline const juce::Colour ivoryShade { 0xffd9ceb4 };
inline const juce::Colour ebony { 0xff151210 };
inline const juce::Colour activeKey { 0xfff0b44c };
inline const juce::Colour text { 0xfff2e6cc };

inline juce::Colour stopTextColour (StopFamily f)
{
    switch (f)
    {
        case StopFamily::Reed: return juce::Colour (0xffa3161b);
        case StopFamily::Mixture: return juce::Colour (0xff1f5a2c);
        case StopFamily::String: return juce::Colour (0xff253a7a);
        case StopFamily::Flute: return juce::Colour (0xff5a3510);
        case StopFamily::Percussion: return juce::Colour (0xff6a2a7a);
        default: return juce::Colour (0xff111111);
    }
}

inline juce::Font serif (float size, bool bold = false)
{
    return juce::Font (juce::FontOptions ("Georgia", size, bold ? juce::Font::bold : juce::Font::plain));
}

inline juce::Font sans (float size, bool bold = false)
{
    return juce::Font (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain));
}

// brass name plate
inline void drawPlate (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& label, float fontSize)
{
    juce::ColourGradient grad (gold.brighter (0.25f), r.getX(), r.getY(), goldDark, r.getX(), r.getBottom(), false);
    g.setGradientFill (grad);
    g.fillRoundedRectangle (r, 3.0f);
    g.setColour (goldDark.darker (0.6f));
    g.drawRoundedRectangle (r, 3.0f, 1.0f);
    g.setColour (juce::Colour (0xff2a1c08));
    g.setFont (serif (fontSize, true));
    g.drawFittedText (label, r.toNearestInt().reduced (4, 0), juce::Justification::centred, 1);
}

inline void fillWood (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour base, bool vertical = false)
{
    juce::ColourGradient grad (base.brighter (0.12f), r.getX(), r.getY(), base.darker (0.25f),
                               vertical ? r.getRight() : r.getX(), vertical ? r.getY() : r.getBottom(), false);
    g.setGradientFill (grad);
    g.fillRect (r);
    // grain
    juce::Random rnd (12345 + (int) r.getX() * 7 + (int) r.getY());
    for (int i = 0; i < (int) (r.getWidth() * r.getHeight() / 900.0f) && i < 400; ++i)
    {
        const float y = r.getY() + rnd.nextFloat() * r.getHeight();
        const float x = r.getX() + rnd.nextFloat() * r.getWidth();
        const float len = 20.0f + rnd.nextFloat() * 80.0f;
        g.setColour (juce::Colours::black.withAlpha (0.05f + 0.06f * rnd.nextFloat()));
        if (vertical)
            g.drawLine (x, y, x + rnd.nextFloat() * 2.0f - 1.0f, juce::jmin (r.getBottom(), y + len), 1.0f);
        else
            g.drawLine (x, y, juce::jmin (r.getRight(), x + len), y + rnd.nextFloat() * 2.0f - 1.0f, 1.0f);
    }
}
} // namespace orgel::theme
