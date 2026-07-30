#pragma once

#define PLUGIN_MAJOR 1
#define PLUGIN_MINOR 1
#define PLUGIN_REV 3
#define PLUGIN_BUILD 10

// Dotted string for the Windows VERSIONINFO resource (version.rc) — derived
// from the numbers above so version.h stays the single source of truth.
#define DTH_STR2(x) #x
#define DTH_STR(x) DTH_STR2(x)
#define PLUGIN_VERSION_STRING \
	DTH_STR(PLUGIN_MAJOR) "." DTH_STR(PLUGIN_MINOR) "." DTH_STR(PLUGIN_REV) "." DTH_STR(PLUGIN_BUILD)
