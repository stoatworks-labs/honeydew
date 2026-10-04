#include "Honeydew.h"

/**
    The effect: the clip is the lightbox under the dish. Each display
    primary's light is filtered by the layer's transmittance, so the dish is a
    real colour filter over the picture; Light Coupling projects the picture
    onto the photosensitive chemistries.

    See SourcePlugin.cpp for why this file is listed in its own target.
    `SW Honeydew Over` is exactly 16 characters, which the unterminated field
    allows.
*/
namespace
{
class HoneydewEffect : public honeydew::HoneydewPlugin
{
public:
	HoneydewEffect() :
		HoneydewPlugin( true )
	{
	}
};
} // namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< HoneydewEffect >,// Create method
	"HD02",                         // Plugin unique ID of maximum length 4
	"SW Honeydew Over",             // Plugin name
	2,                              // API major version number
	1,                              // API minor version number
	0,                              // Plugin major version number
	1,                              // Plugin minor version number
	FF_EFFECT,                      // Plugin type
	"The clip is the lightbox under a dish of real reagents: the layer filters the picture's light species by species "
	"(Beer-Lambert), and Light Coupling projects the picture onto the photosensitive chemistries so waves avoid and "
	"Turing patterns print its bright parts.",
	"Honeydew FFGL effect"// About
);

extern "C" const char* HoneydewEffectBuildStamp()
{
	return "honeydew " HONEYDEW_VERSION " effect, built " __DATE__ " " __TIME__;
}
