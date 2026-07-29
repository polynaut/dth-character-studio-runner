#include "JobRunnerAction.h"
#include "JobPoller.h"

DzJobRunnerAction::DzJobRunnerAction()
    : DzAction(tr("DTH Job Runner: Check for Jobs Now"),
               tr("Immediately check all mapped content directories for a DTH exporter job file."))
{
    JobPoller::instance().start();
}

QString DzJobRunnerAction::getDefaultMenuPath() const
{
    return tr("&Scripts");
}

void DzJobRunnerAction::executeAction()
{
    JobPoller::instance().checkNow();
}
