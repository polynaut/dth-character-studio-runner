#include <dzplugin.h>
#include <dzapp.h>
#include <dzversion.h> // DZ_SDK_VERSION, referenced by DZ_PLUGIN_DEFINITION

#include "JobRunnerAction.h"
#include "version.h"

DZ_PLUGIN_DEFINITION("DTH Job Runner");

DZ_PLUGIN_AUTHOR("polynaut");

DZ_PLUGIN_VERSION(PLUGIN_MAJOR, PLUGIN_MINOR, PLUGIN_REV, PLUGIN_BUILD);

DZ_PLUGIN_DESCRIPTION(QString(
    "Watches every mapped content directory for Scripts/DTH-Character-Studio/"
    "dth_exporter_jobs.csv written by DTH Character Studio, and runs each row "
    "(open scene or new empty scene, then execute the row's script with the "
    "'bulk-export' argument). The job file is deleted as the transfer ack."));

DZ_PLUGIN_CLASS_GUID(DzJobRunnerAction, 36629525-4ea0-4bc0-96d3-30d147f12384);
