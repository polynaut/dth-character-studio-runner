#pragma once

#include <dzaction.h>

// Registered DzAction with two jobs:
//  1. Startup hook: Daz Studio instantiates every registered DzAction at
//     startup to populate menus/Customize — the constructor starts the
//     JobPoller (which defers its first poll internally).
//  2. Manual trigger: an immediate "check for jobs now" menu entry.
class DzJobRunnerAction : public DzAction
{
    Q_OBJECT
public:
    DzJobRunnerAction();

protected:
    virtual QString getDefaultMenuPath() const;
    virtual void executeAction();
};
