#include "Notation.h"
#include "../engine/I18n.h"
#include "BinaryData.h"

#include "toolkit.h"
#include "toolkitdef.h"
#include "vrv.h"
#include <juce_graphics/juce_graphics.h>

namespace orgel
{
static std::mutex& globalVerovioMutex()
{
    static std::mutex m;
    return m;
}

Notation::Notation() = default;
Notation::~Notation() = default;

juce::File Notation::resourceDirectory()
{
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
#if JUCE_MAC
    base = base.getChildFile ("Application Support");
#endif
    auto dir = base.getChildFile ("Orgelwerk").getChildFile ("verovio-6.4.1");
    auto stamp = dir.getChildFile (".complete");
    if (stamp.existsAsFile())
        return dir;

    static std::mutex extractMutex;
    std::lock_guard<std::mutex> g (extractMutex);
    if (stamp.existsAsFile())
        return dir;
    dir.deleteRecursively();
    dir.createDirectory();
    juce::MemoryInputStream zipStream (BinaryData::veroviodata_zip, (size_t) BinaryData::veroviodata_zipSize, false);
    juce::ZipFile zip (zipStream);
    if (zip.uncompressTo (dir, true).wasOk())
        stamp.replaceWithText ("ok");
    return dir;
}

juce::StringArray Notation::verovioExtensions()
{
    return { "musicxml", "xml", "mxl", "mei", "abc", "krn", "hum", "hmd", "pae", "gabc", "md", "msd", "esac", "cmme" };
}

vrv::Toolkit& Notation::toolkit()
{
    if (tk == nullptr)
    {
        vrv::EnableLog (vrv::LOG_ERROR);
        tk = std::make_unique<vrv::Toolkit> (false);
        tk->SetResourcePath (resourceDirectory().getFullPathName().toStdString());
    }
    return *tk;
}

void Notation::setLayout (int w, int h, int s)
{
    std::lock_guard<std::recursive_mutex> g (mutex);
    pageWidth = w;
    pageHeight = h;
    scale = s;
}

static juce::String formatForExtension (const juce::String& ext)
{
    auto e = ext.toLowerCase().trimCharactersAtStart (".");
    if (e == "musicxml" || e == "xml" || e == "mxl") return "musicxml";
    if (e == "mei") return "mei";
    if (e == "abc") return "abc";
    if (e == "krn" || e == "hum" || e == "hmd") return "humdrum";
    if (e == "pae") return "pae";
    if (e == "gabc") return "gabc";
    if (e == "md" || e == "msd") return "musedata";
    if (e == "esac") return "esac";
    if (e == "cmme") return "cmme.xml";
    return "auto";
}

bool Notation::loadFile (const juce::File& file, juce::String& error)
{
    juce::MemoryBlock mb;
    if (! file.loadFileAsData (mb))
    {
        error = tr ("Datei kann nicht gelesen werden: ") + file.getFullPathName();
        return false;
    }
    auto* bytes = static_cast<const unsigned char*> (mb.getData());
    if (mb.getSize() > 4 && bytes[0] == 'P' && bytes[1] == 'K')
    {
        std::lock_guard<std::recursive_mutex> g (mutex);
        std::lock_guard<std::mutex> gg (globalVerovioMutex());
        auto& t = toolkit();
        applyOptions();
        t.SetInputFrom ("musicxml");
        if (! t.LoadZipDataBuffer (bytes, (int) mb.getSize()))
        {
            error = tr ("Komprimierte MusicXML-Datei konnte nicht gelesen werden.");
            return false;
        }
        lastData = {};
        lastFormat = "mxl";
        title = file.getFileNameWithoutExtension();
        renderAll (false);
        return true;
    }
    if (! loadData (file.loadFileAsString(), formatForExtension (file.getFileExtension()), error))
        return false;
    if (title.isEmpty())
        title = file.getFileNameWithoutExtension();
    return true;
}

bool Notation::loadData (const juce::String& data, const juce::String& format, juce::String& error)
{
    std::lock_guard<std::recursive_mutex> g (mutex);
    {
        std::lock_guard<std::mutex> gg (globalVerovioMutex());
        auto& t = toolkit();
        applyOptions();
        t.SetInputFrom (format.isEmpty() ? "auto" : format.toStdString());
        if (! t.LoadData (data.toStdString()))
        {
            error = tr ("Die Noten konnten nicht gelesen werden") + " (" + format + ").";
            pages.clear();
            return false;
        }
        lastData = data;
        lastFormat = format;
        title = {};
        renderAll (false);
    }
    return ! pages.empty();
}

void Notation::applyOptions()
{
    auto& t = toolkit();
    juce::DynamicObject::Ptr opts = new juce::DynamicObject();
    opts->setProperty ("pageWidth", pageWidth);
    opts->setProperty ("pageHeight", pageHeight);
    opts->setProperty ("scale", scale);
    opts->setProperty ("adjustPageHeight", false);
    opts->setProperty ("footer", "none");
    opts->setProperty ("header", "auto");
    opts->setProperty ("breaks", "smart");
    opts->setProperty ("justifyVertically", true);
    opts->setProperty ("spacingStaff", 10);
    opts->setProperty ("spacingSystem", 14);
    opts->setProperty ("svgViewBox", true);
    if (hideEmptyStaves >= 0)
    {
        opts->setProperty ("condense", hideEmptyStaves != 0 ? "auto" : "none");
        opts->setProperty ("condenseFirstPage", hideEmptyStaves != 0);
        // (despite its name, "true" would keep every staff in bars with a tempo, and MIDI
        // files change tempo in almost every bar)
        opts->setProperty ("condenseTempoPages", false);
    }
    opts->setProperty ("pageMarginLeft", 80);
    opts->setProperty ("pageMarginRight", 80);
    opts->setProperty ("pageMarginTop", 80);
    opts->setProperty ("pageMarginBottom", 80);
    t.SetOptions (juce::JSON::toString (juce::var (opts.get()), true).toStdString());
}

// The options are set before loading, so Verovio lays the pages out only once.
void Notation::renderAll (bool relayout)
{
    auto& t = toolkit();
    if (relayout)
    {
        applyOptions();
        t.RedoLayout();
    }

    pages.clear();
    notePage.clear();
    systemIds.clear();
    const int count = t.GetPageCount();
    for (int p = 1; p <= count; ++p)
    {
        const std::string raw = t.RenderToSVG (p, false);
        // remember which page each note is on (to turn pages while playing);
        // searched in the plain bytes, which stays linear on big pages
        static const std::string marker = "class=\"note\"";
        for (size_t idx = raw.find (marker); idx != std::string::npos; idx = raw.find (marker, idx + marker.size()))
        {
            const size_t tagStart = raw.rfind ('<', idx);
            const size_t tagEnd = raw.find ('>', idx);
            if (tagStart == std::string::npos || tagEnd == std::string::npos)
                continue;
            const size_t idPos = raw.find ("id=\"", tagStart);
            if (idPos == std::string::npos || idPos > tagEnd)
                continue;
            const size_t idEnd = raw.find ('"', idPos + 4);
            if (idEnd != std::string::npos && idEnd > idPos + 4 && idEnd < tagEnd)
                notePage[juce::String (juce::CharPointer_UTF8 (raw.c_str() + idPos + 4), juce::CharPointer_UTF8 (raw.c_str() + idEnd))] = p - 1;
        }
        // the systems: <g id="..." class="system">
        std::vector<juce::String> systems;
        static const std::string systemMarker = "class=\"system\"";
        for (size_t idx = raw.find (systemMarker); idx != std::string::npos; idx = raw.find (systemMarker, idx + systemMarker.size()))
        {
            const size_t tagStart = raw.rfind ('<', idx);
            const size_t idPos = tagStart == std::string::npos ? std::string::npos : raw.find ("id=\"", tagStart);
            if (idPos == std::string::npos || idPos > idx)
                continue;
            const size_t idEnd = raw.find ('"', idPos + 4);
            if (idEnd != std::string::npos && idEnd < idx)
                systems.push_back (juce::String (juce::CharPointer_UTF8 (raw.c_str() + idPos + 4), juce::CharPointer_UTF8 (raw.c_str() + idEnd)));
        }
        systemIds.push_back (std::move (systems));
        juce::String svg (juce::CharPointer_UTF8 (raw.c_str()));
        pages.push_back (svg);
    }

    // timing of notes in quarter notes, to highlight what is playing
    t.RenderToMIDI();
    timemap.clear();
    auto json = juce::JSON::parse (juce::String (juce::CharPointer_UTF8 (t.RenderToTimemap ("{\"includeRests\": false}").c_str())));
    if (auto* arr = json.getArray())
        for (auto& e : *arr)
        {
            TimeEntry te;
            te.qstamp = (double) e.getProperty ("qstamp", 0.0);
            if (auto* on = e.getProperty ("on", {}).getArray())
                for (auto& id : *on)
                    te.on.push_back (id.toString());
            if (auto* off = e.getProperty ("off", {}).getArray())
                for (auto& id : *off)
                    te.off.push_back (id.toString());
            timemap.push_back (std::move (te));
        }
}

juce::String Notation::getRawPageSVG (int page) const
{
    std::lock_guard<std::recursive_mutex> g (mutex);
    return juce::isPositiveAndBelow (page, (int) pages.size()) ? pages[(size_t) page] : juce::String();
}

juce::String Notation::getPageSVG (int page) const
{
    return inlineSymbols (getRawPageSVG (page));
}

std::vector<juce::String> Notation::getSystemIds (int page) const
{
    std::lock_guard<std::recursive_mutex> g (mutex);
    return juce::isPositiveAndBelow (page, (int) systemIds.size()) ? systemIds[(size_t) page] : std::vector<juce::String>();
}

int Notation::pageOfElement (const juce::String& id) const
{
    std::lock_guard<std::recursive_mutex> g (mutex);
    auto it = notePage.find (id);
    return it != notePage.end() ? it->second : -1;
}

juce::MemoryBlock Notation::renderMidi()
{
    std::lock_guard<std::recursive_mutex> g (mutex);
    std::lock_guard<std::mutex> gg (globalVerovioMutex());
    juce::MemoryOutputStream out;
    juce::Base64::convertFromBase64 (out, juce::String (toolkit().RenderToMIDI()));
    return out.getMemoryBlock();
}

juce::String Notation::getMEI()
{
    std::lock_guard<std::recursive_mutex> g (mutex);
    std::lock_guard<std::mutex> gg (globalVerovioMutex());
    return juce::String (juce::CharPointer_UTF8 (toolkit().GetMEI ("{\"pageNo\": 0}").c_str()));
}

juce::String Notation::getHumdrum()
{
    std::lock_guard<std::recursive_mutex> g (mutex);
    std::lock_guard<std::mutex> gg (globalVerovioMutex());
    auto& t = toolkit();
    auto mei = t.GetMEI ("{\"pageNo\": 0}");
    return juce::String (juce::CharPointer_UTF8 (t.ConvertMEIToHumdrum (mei).c_str()));
}

juce::String Notation::musicXmlToHumdrum (const juce::String& musicXml)
{
    std::lock_guard<std::mutex> gg (globalVerovioMutex());
    vrv::Toolkit t (false);
    t.SetResourcePath (resourceDirectory().getFullPathName().toStdString());
    t.SetInputFrom ("musicxml-hum");
    if (! t.LoadData (musicXml.toStdString()))
        return {};
    return juce::String (juce::CharPointer_UTF8 (t.GetHumdrum().c_str()));
}

//==============================================================================
namespace
{
    // SMuFL glyph outlines from Verovio's font data, for music symbols inside text.
    struct Glyph
    {
        juce::String pathD;
        double advance = 0;
    };

    const Glyph* findGlyph (const juce::String& font, juce::juce_wchar code)
    {
        static std::mutex m;
        static std::map<juce::String, Glyph> cache;
        static std::map<juce::String, std::map<juce::String, double>> advances;
        std::lock_guard<std::mutex> g (m);

        auto fontName = font.containsIgnoreCase ("Bravura") ? juce::String ("Bravura") : juce::String ("Leipzig");
        auto hex = juce::String::toHexString ((int) code).toUpperCase().paddedLeft ('0', 4);
        auto key = fontName + "/" + hex;
        if (auto it = cache.find (key); it != cache.end())
            return it->second.pathD.isEmpty() ? nullptr : &it->second;

        auto dir = Notation::resourceDirectory();
        if (! advances.count (fontName))
        {
            auto& a = advances[fontName];
            if (auto xml = juce::parseXML (dir.getChildFile (fontName + ".xml")))
                for (auto* e : xml->getChildIterator())
                    a[e->getStringAttribute ("c")] = e->getDoubleAttribute ("h-a-x");
        }
        Glyph gl;
        if (auto xml = juce::parseXML (dir.getChildFile (fontName).getChildFile (hex + ".xml")))
            if (auto* path = xml->getChildByName ("path"))
                gl.pathD = path->getStringAttribute ("d");
        gl.advance = advances[fontName][hex];
        cache[key] = gl;
        return gl.pathD.isEmpty() ? nullptr : &cache[key];
    }

    double pxValue (const juce::String& s) { return s.trimCharactersAtEnd ("px").getDoubleValue(); }

    // Replaces a nested Verovio <text> by plain text runs and glyph paths that JUCE can draw.
    juce::XmlElement* flattenText (const juce::XmlElement& text, const juce::String& inheritedColour)
    {
        struct State
        {
            double x = 0, y = 0, size = 0;
            bool hasX = false;
            juce::String anchor = "start", family, style, weight, fill;
        };
        auto* group = new juce::XmlElement ("g");
        State root;
        root.fill = inheritedColour;
        root.x = pxValue (text.getStringAttribute ("x"));
        root.y = pxValue (text.getStringAttribute ("y"));
        root.hasX = text.hasAttribute ("x");
        double cursor = root.x;
        juce::XmlElement* lastText = nullptr;
        double lastY = 0;

        std::function<void (const juce::XmlElement&, State)> walk = [&] (const juce::XmlElement& e, State st) {
            if (e.hasTagName ("title") || e.hasTagName ("desc"))
                return;
            if (e.hasAttribute ("x")) { st.x = pxValue (e.getStringAttribute ("x")); cursor = st.x; }
            if (e.hasAttribute ("y")) st.y = pxValue (e.getStringAttribute ("y"));
            if (e.hasAttribute ("font-size")) st.size = pxValue (e.getStringAttribute ("font-size"));
            if (e.hasAttribute ("text-anchor")) st.anchor = e.getStringAttribute ("text-anchor");
            if (e.hasAttribute ("font-family")) st.family = e.getStringAttribute ("font-family");
            if (e.hasAttribute ("font-style")) st.style = e.getStringAttribute ("font-style");
            if (e.hasAttribute ("font-weight")) st.weight = e.getStringAttribute ("font-weight");
            if (e.hasAttribute ("fill") && e.getStringAttribute ("fill") != "currentColor") st.fill = e.getStringAttribute ("fill");

            for (auto* c : e.getChildIterator())
            {
                if (! c->isTextElement())
                {
                    walk (*c, st);
                    continue;
                }
                auto content = c->getText();
                if (content.trim().isEmpty() || st.size <= 0)
                    continue;
                const bool musicFont = st.family.containsIgnoreCase ("Leipzig") || st.family.containsIgnoreCase ("Bravura")
                                       || st.family.containsIgnoreCase ("Leland") || st.family.containsIgnoreCase ("Petaluma");
                if (musicFont)
                {
                    for (auto p = content.getCharPointer(); ! p.isEmpty();)
                    {
                        auto ch = p.getAndAdvance();
                        if (auto* gl = findGlyph (st.family, ch))
                        {
                            auto* g = group->createNewChildElement ("path");
                            g->setAttribute ("d", gl->pathD);
                            g->setAttribute ("transform", "translate(" + juce::String (cursor) + "," + juce::String (st.y) + ") scale("
                                                              + juce::String (st.size / 1000.0) + "," + juce::String (-st.size / 1000.0) + ")");
                            g->setAttribute ("fill", st.fill);
                            cursor += gl->advance * st.size / 1000.0;
                        }
                    }
                    continue;
                }
                // centred headers like "- 14 -" come in several runs: keep them in one text
                if (st.anchor != "start" && lastText != nullptr && std::abs (lastY - st.y) < 1.0)
                {
                    auto* tn = lastText->getFirstChildElement();
                    tn->setText (tn->getText() + content);
                    continue;
                }
                auto* t = group->createNewChildElement ("text");
                lastText = st.anchor != "start" ? t : nullptr;
                lastY = st.y;
                t->setAttribute ("x", cursor);
                t->setAttribute ("y", st.y);
                t->setAttribute ("font-size", juce::String (st.size) + "px");
                t->setAttribute ("font-family", "Times New Roman, Times, serif");
                t->setAttribute ("fill", st.fill);
                if (st.anchor != "start") t->setAttribute ("text-anchor", st.anchor);
                if (st.style.isNotEmpty()) t->setAttribute ("font-style", st.style);
                if (st.weight.isNotEmpty()) t->setAttribute ("font-weight", st.weight);
                t->addTextElement (st.anchor != "start" ? content : content.trim());
                if (st.anchor == "start")
                    cursor += juce::Font (juce::FontOptions ((float) st.size)).getStringWidthFloat (content) * 1.0;
            }
        };
        walk (text, root);
        return group;
    }
} // namespace

juce::String Notation::inlineSymbols (const juce::String& svg)
{
    auto xml = juce::XmlDocument::parse (svg);
    if (xml == nullptr)
        return svg;

    // glyph definitions: <symbol> (older Verovio) or <g> inside <defs> (Verovio 6)
    std::map<juce::String, juce::XmlElement*> symbols;
    std::function<void (juce::XmlElement&, bool)> collect = [&] (juce::XmlElement& e, bool inDefs) {
        for (auto* c : e.getChildIterator())
        {
            if ((c->hasTagName ("symbol") || inDefs) && c->hasAttribute ("id"))
                symbols[c->getStringAttribute ("id")] = c;
            collect (*c, c->hasTagName ("defs"));
        }
    };
    collect (*xml, false);

    auto num = [] (const juce::String& s) { return s.trimCharactersAtEnd ("px").getDoubleValue(); };

    std::function<void (juce::XmlElement&, juce::String)> replace = [&] (juce::XmlElement& parent, juce::String colour) {
        for (int i = 0; i < parent.getNumChildElements(); ++i)
        {
            auto* c = parent.getChildElement (i);
            if (c->hasTagName ("style"))
            {
                // JUCE does not apply Verovio's CSS; strokes are set explicitly below
                parent.removeChildElement (c, true);
                --i;
                continue;
            }
            if (c->getStringAttribute ("class").contains ("placeholder"))
            {
                parent.removeChildElement (c, true); // hidden by Verovio's CSS
                --i;
                continue;
            }
            // CSS-free colour inheritance: "color" on a group colours everything inside it
            juce::String here = colour;
            if (c->hasAttribute ("color") && c->getStringAttribute ("color") != "currentColor")
                here = c->getStringAttribute ("color");
            if (c->hasTagName ("text"))
            {
                parent.replaceChildElement (c, flattenText (*c, here));
                continue;
            }
            if (c->hasTagName ("use"))
            {
                auto href = c->getStringAttribute ("xlink:href", c->getStringAttribute ("href"));
                auto it = symbols.find (href.trimCharactersAtStart ("#"));
                if (it != symbols.end())
                {
                    auto* sym = it->second;
                    juce::String transform;
                    if (c->hasAttribute ("transform"))
                        transform << c->getStringAttribute ("transform") << " ";
                    if (c->hasAttribute ("x") || c->hasAttribute ("y"))
                        transform << "translate(" << num (c->getStringAttribute ("x")) << "," << num (c->getStringAttribute ("y")) << ") ";
                    if (sym->hasTagName ("symbol"))
                    {
                        auto vb = juce::StringArray::fromTokens (sym->getStringAttribute ("viewBox", "0 0 1000 1000"), " ,", "");
                        const double vx = vb[0].getDoubleValue(), vy = vb[1].getDoubleValue();
                        const double vw = juce::jmax (1.0, vb[2].getDoubleValue()), vh = juce::jmax (1.0, vb[3].getDoubleValue());
                        const double w = c->hasAttribute ("width") ? num (c->getStringAttribute ("width")) : vw;
                        const double h = c->hasAttribute ("height") ? num (c->getStringAttribute ("height")) : vh;
                        transform << "scale(" << (w / vw) << "," << (h / vh) << ") translate(" << -vx << "," << -vy << ")";
                    }
                    else if (sym->hasAttribute ("transform"))
                        transform << sym->getStringAttribute ("transform");

                    auto* g = new juce::XmlElement ("g");
                    if (transform.trim().isNotEmpty())
                        g->setAttribute ("transform", transform.trim());
                    for (auto* attr : { "fill", "stroke", "class", "fill-opacity" })
                        if (c->hasAttribute (attr))
                            g->setAttribute (attr, c->getStringAttribute (attr));
                    if (sym->hasTagName ("symbol"))
                        for (auto* sc : sym->getChildIterator())
                            g->addChildElement (new juce::XmlElement (*sc));
                    else
                    {
                        auto* copy = new juce::XmlElement (*sym);
                        copy->removeAttribute ("id");
                        copy->removeAttribute ("transform");
                        g->addChildElement (copy);
                    }
                    parent.replaceChildElement (c, g);
                    replace (*g, here);
                    continue;
                }
            }
            if (c->hasTagName ("path") || c->hasTagName ("polygon") || c->hasTagName ("polyline")
                || c->hasTagName ("rect") || c->hasTagName ("ellipse") || c->hasTagName ("line"))
            {
                if (! c->hasAttribute ("stroke") || c->getStringAttribute ("stroke") == "currentColor")
                    c->setAttribute ("stroke", here);
            }
            for (auto* attr : { "fill", "stroke", "color" })
                if (c->getStringAttribute (attr) == "currentColor")
                    c->setAttribute (attr, here);
            if (! c->hasTagName ("symbol") && ! c->hasTagName ("defs"))
                replace (*c, here);
        }
    };
    replace (*xml, "#000000");
    return xml->toString (juce::XmlElement::TextFormat().withoutHeader().singleLine());
}
} // namespace orgel
