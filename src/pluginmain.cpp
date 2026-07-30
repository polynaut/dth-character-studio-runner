#include <dzplugin.h>
#include <dzapp.h>
#include <dzversion.h> // DZ_SDK_VERSION, referenced by DZ_PLUGIN_DEFINITION

#include "JobRunnerAction.h"
#include "version.h"

// Daz Studio resolves a plugin's entry points by their PLAIN C names —
// GetProcAddress("getSDKVersion") / ("getPluginDefinition") — on DS4 AND DS6
// (measured: dzcore.dll's lookup strings, and the export tables of every
// working plugin, Daz's own and the DTH Exporter alike). The SDK's Windows
// DZ_PLUGIN_DEFINITION macro, however, defines both WITHOUT extern "C", which
// exports C++-mangled names the loader cannot find — the plugin then fails at
// startup with "load failed - could not locate the getSDKVersion() function".
// Declaring them extern "C" BEFORE the macro makes the macro's definitions
// inherit C linkage (linkage comes from the first declaration), so the DLL
// exports the plain names Daz looks up. (MSVC warns C4190 about the UDT
// return type crossing C linkage; both sides are the same MSVC C++ ABI.)
extern "C" {
	__declspec(dllexport) DzVersion getSDKVersion();
	__declspec(dllexport) DzPlugin* getPluginDefinition();
}

DZ_PLUGIN_DEFINITION("DTH Character Studio Runner");

DZ_PLUGIN_AUTHOR("polynaut");

DZ_PLUGIN_VERSION(PLUGIN_MAJOR, PLUGIN_MINOR, PLUGIN_REV, PLUGIN_BUILD);

DZ_PLUGIN_DESCRIPTION(QString(
    "Watches every mapped content directory for Scripts/DTH-Character-Studio/"
    "dth_exporter_jobs.csv written by DTH Character Studio, and runs each row "
    "(open scene or new empty scene, then execute the row's script). The job "
    "file is deleted as the transfer ack."));

DZ_PLUGIN_CLASS_GUID(DzJobRunnerAction, 36629525-4ea0-4bc0-96d3-30d147f12384);
