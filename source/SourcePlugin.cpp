#include "Honeydew.h"

/**
    The source: the dish on its lightbox.

    Listed directly in the HoneydewSource target, not in honeydew_core: both
    plugins share the class and not the `CFFGLPluginInfo` below, and putting
    either registration in the shared library would register both plugins
    into both bundles. The core is an OBJECT library because this registers
    itself from a file-scope constructor nothing references (CMakeLists.txt).

    `SW Honeydew` is eleven characters; the FFGL name field is char[ 16 ] and
    not null-terminated. `oxbow probe` reads it back the way a host does.
*/
namespace
{
class HoneydewSource : public honeydew::HoneydewPlugin
{
public:
	HoneydewSource() :
		HoneydewPlugin( false )
	{
	}
};
} // namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< HoneydewSource >,// Create method
	"HD01",                         // Plugin unique ID of maximum length 4
	"SW Honeydew",                  // Plugin name
	2,                              // API major version number
	1,                              // API minor version number
	0,                              // Plugin major version number
	1,                              // Plugin minor version number
	FF_SOURCE,                      // Plugin type
	"A thin layer of real reagents in a dish on a lightbox: the Belousov-Zhabotinsky reaction's waves and spirals, the "
	"Briggs-Rauscher and iodine-clock colour changes, CDIMA Turing patterns, the blue-bottle family and the chemical "
	"chameleon, each from its published mechanism, coloured by Beer-Lambert through each species' spectrum.",
	"Honeydew FFGL source"// About
);

extern "C" const char* HoneydewSourceBuildStamp()
{
	return "honeydew " HONEYDEW_VERSION " source, built " __DATE__ " " __TIME__;
}
