#include "JobPoller.h"
#include "CsvJobFile.h"

#include <dzapp.h>
#include <dzcontentmgr.h>
#include <dzscene.h>
#include <dzscript.h>

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMetaObject>
#include <QtCore/QVariant>

namespace {

const int kStartupDelayMs = 3000; // content-dir mapping may not be ready at plugin init
const int kPollIntervalMs = 5000;
const int kSettleMs = 500;        // event-loop drain between scene open and script run

const char *const kJobFileRelPath = "/Scripts/DTH-Character-Studio/dth_exporter_jobs.csv";
const char *const kLogPrefix = "[DTH JobRunner] ";

void disposeScript(DzScript *script)
{
#if DAZ_SDK_MAJOR_VERSION >= 6
    script->unref(); // SDK6: DzScript is ref-counted, destructor is protected
#else
    delete script;   // SDK4: plain public virtual destructor
#endif
}

} // namespace

JobPoller &JobPoller::instance()
{
    static JobPoller poller;
    return poller;
}

JobPoller::JobPoller()
    : m_state(Stopped)
    , m_started(false)
    , m_index(0)
    , m_ignoredSize(-1)
{
    m_pollTimer.setInterval(kPollIntervalMs);
    connect(&m_pollTimer, SIGNAL(timeout()), this, SLOT(onPollTick()));
}

void JobPoller::log(const QString &message)
{
    if (dzApp)
        dzApp->log(QString(kLogPrefix) + message);
}

void JobPoller::start()
{
    if (m_started)
        return;
    m_started = true;
    QTimer::singleShot(kStartupDelayMs, this, SLOT(beginPolling()));
}

void JobPoller::beginPolling()
{
    m_state = Polling;
    m_pollTimer.start();
    log(QString("watching content directories for %1 (every %2 s)")
            .arg(QString(kJobFileRelPath))
            .arg(kPollIntervalMs / 1000));
    onPollTick();
}

void JobPoller::checkNow()
{
    if (!m_started) {
        start();
        return;
    }
    if (m_state == RunningBatch) {
        log("a batch is already running — new job files are picked up when it finishes");
        return;
    }
    log("manual check triggered");
    m_ignoredPath.clear(); // a manual check re-reports even a previously ignored file
    onPollTick();
}

void JobPoller::rememberIgnored(const QString &path)
{
    const QFileInfo info(path);
    m_ignoredPath = path;
    m_ignoredSize = info.size();
    m_ignoredMtime = info.lastModified();
}

void JobPoller::onPollTick()
{
    if (m_state != Polling)
        return;

    DzContentMgr *mgr = dzApp ? dzApp->getContentMgr() : NULL;
    if (!mgr)
        return;

    QString found;
    for (int i = 0; i < mgr->getNumContentDirectories(); ++i) {
        const QString candidate = mgr->getContentDirectoryPath(i) + kJobFileRelPath;
        if (QFile::exists(candidate)) {
            found = candidate;
            break; // first mapped directory wins
        }
    }
    if (found.isEmpty())
        return;

    const QFileInfo info(found);
    if (found == m_ignoredPath && info.size() == m_ignoredSize && info.lastModified() == m_ignoredMtime)
        return; // previously rejected and unchanged — stay silent

    QFile file(found);
    if (!file.open(QIODevice::ReadOnly))
        return; // writer may still hold it — retry next tick
    const QByteArray bytes = file.readAll();
    file.close();

    const dthjr::ParseResult parsed =
        dthjr::parseJobCsv(std::string_view(bytes.constData(), static_cast<size_t>(bytes.size())));

    for (size_t i = 0; i < parsed.warnings.size(); ++i)
        log(QString("job file: %1").arg(QString::fromStdString(parsed.warnings[i])));

    if (!parsed.ok) {
        log(QString("ignoring foreign/corrupt job file %1 (%2) — leaving it in place")
                .arg(found, QString::fromStdString(parsed.error)));
        rememberIgnored(found);
        return;
    }

    // Deleting the file is the "transfer succeeded" ack — do it before running
    // anything so a crash mid-batch never re-runs the batch on the next start.
    if (!QFile::remove(found)) {
        log(QString("could not delete job file %1 — NOT running it (a re-poll would double-run)").arg(found));
        rememberIgnored(found);
        return;
    }

    if (parsed.rows.empty()) {
        log("job file contained no runnable rows");
        return;
    }

    QList<Job> jobs;
    for (size_t i = 0; i < parsed.rows.size(); ++i) {
        Job job;
        job.scenePath = QString::fromStdString(parsed.rows[i].scenePath);
        job.scriptPath = QString::fromStdString(parsed.rows[i].scriptPath);
        jobs.append(job);
    }
    beginBatch(jobs);
}

void JobPoller::beginBatch(const QList<Job> &jobs)
{
    m_pollTimer.stop();
    m_state = RunningBatch;
    m_queue = jobs;
    m_index = 0;
    log(QString("starting batch of %1 job(s)").arg(m_queue.size()));
    QMetaObject::invokeMethod(this, "stepOpenScene", Qt::QueuedConnection);
}

void JobPoller::stepOpenScene()
{
    const Job &job = m_queue.at(m_index);

    if (job.scenePath.isEmpty()) {
        log(QString("row %1/%2: new empty scene").arg(m_index + 1).arg(m_queue.size()));
        newEmptyScene();
    } else if (!QFile::exists(job.scenePath)) {
        log(QString("row %1/%2 skipped: scene not found: %3").arg(m_index + 1).arg(m_queue.size()).arg(job.scenePath));
        advanceRow();
        return;
    } else {
        log(QString("row %1/%2: opening %3").arg(m_index + 1).arg(m_queue.size()).arg(job.scenePath));
        // merge=false replaces the current scene without a save prompt
        // (measured behaviour the studio already relies on).
        if (!dzApp->getContentMgr()->openFile(job.scenePath, false)) {
            log(QString("row %1/%2 skipped: failed to open scene: %3")
                    .arg(m_index + 1).arg(m_queue.size()).arg(job.scenePath));
            advanceRow();
            return;
        }
    }

    // Let deferred post-load work settle before the script runs.
    QTimer::singleShot(kSettleMs, this, SLOT(stepExecute()));
}

void JobPoller::stepExecute()
{
    const Job &job = m_queue.at(m_index);

    if (!QFile::exists(job.scriptPath)) {
        log(QString("row %1/%2 skipped: script not found: %3").arg(m_index + 1).arg(m_queue.size()).arg(job.scriptPath));
        advanceRow();
        return;
    }

    DzScript *script = new DzScript;
#if DAZ_SDK_MAJOR_VERSION >= 6
    script->ref();
#endif
    if (!script->loadFromFile(job.scriptPath)) {
        log(QString("row %1/%2 skipped: could not load script: %3")
                .arg(m_index + 1).arg(m_queue.size()).arg(job.scriptPath));
        disposeScript(script);
        advanceRow();
        return;
    }

    QVariantList args;
    args << QString("bulk-export");
    log(QString("row %1/%2: running %3").arg(m_index + 1).arg(m_queue.size()).arg(job.scriptPath));
    const bool ok = script->execute(args); // synchronous; returns when the ROM + export are done
    disposeScript(script);
    log(QString("row %1/%2: %3").arg(m_index + 1).arg(m_queue.size()).arg(ok ? "done" : "script reported failure"));

    advanceRow();
}

void JobPoller::advanceRow()
{
    ++m_index;
    if (m_index < m_queue.size())
        QMetaObject::invokeMethod(this, "stepOpenScene", Qt::QueuedConnection);
    else
        finishBatch();
}

void JobPoller::finishBatch()
{
    // The last row's scene is full of throwaway ROM keyframes — discard them
    // so quitting Daz later never prompts to save.
    newEmptyScene();
    log("batch finished");
    m_queue.clear();
    m_index = 0;
    m_state = Polling;
    m_pollTimer.start();
}

void JobPoller::newEmptyScene()
{
    // DzScene::clear() is the File > New equivalent (dzscene.h, Q_SLOT).
    // The SDK exposes no scene dirty-flag API, so clear() is also our only
    // means of ensuring quitting Daz never prompts to save — verified by the
    // end-to-end smoke test.
    dzScene->clear();
}
