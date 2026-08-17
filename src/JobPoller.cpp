#include "JobPoller.h"
#include "CsvJobFile.h"

#include <dzaction.h>
#include <dzactionmgr.h>
#include <dzapp.h>
#include <dzcontentmgr.h>
#include <dzmainwindow.h>
#include <dzscene.h>
#include <dzscript.h>

#if DAZ_SDK_MAJOR_VERSION >= 6
#include <QtWidgets/QMessageBox>
#else
#include <QtGui/QMessageBox>
#endif

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMetaObject>
#include <QtCore/QVariant>

namespace {

const int kStartupDelayMs = 3000; // content-dir mapping may not be ready at plugin init
// The fallback poll's two paces. FAST while the file watches don't cover
// every existing scripts dir (none may exist yet — the studio creates them —
// and a failed addPath must not cost pickup latency): the poll then IS the
// pickup, exactly like pre-1.3.0. SLOW once they do: the watch is the pickup
// and the poll only papers over events a network share may swallow.
const int kPollIntervalMs = 5000;
const int kWatchedPollIntervalMs = 15000;
// One handoff is a BURST of directory events (the studio writes a temp file
// and renames it) — collapse them into one check.
const int kWatchDebounceMs = 250;
const int kSettleMs = 500;        // event-loop drain between scene open and script run

// Contract v2 (JSON, renamed on pickup) + the legacy v1 CSV.
const char *const kJobFileRelPath = "/Scripts/DTH-Character-Studio/dth_exporter_jobs.json";
const char *const kRunningJobFileName = "running_dth_exporter_jobs.json";
const char *const kLegacyJobFileRelPath = "/Scripts/DTH-Character-Studio/dth_exporter_jobs.csv";
const char *const kLogPrefix = "[DTH Character Studio Runner] ";

void disposeScript(DzScript *script)
{
#if DAZ_SDK_MAJOR_VERSION >= 6
    script->unref(); // SDK6: DzScript is ref-counted, destructor is protected
#else
    delete script;   // SDK4: plain public virtual destructor
#endif
}

// Bring the Daz Studio main window to the front — the open-scene batch's
// second half. Only possible from INSIDE the process: Windows refuses
// SetForegroundWindow (what activateWindow maps to) from a background
// process, which is exactly why the studio delegates the whole open here.
void raiseMainWindow()
{
    DzMainWindow *win = dzApp ? dzApp->getInterface() : NULL;
    if (!win)
        return;
    if (win->isMinimized())
        win->showNormal();
    win->raise();
    win->activateWindow();
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
    // Real file watching (v1.3.0): a changed scripts dir funnels through the
    // single-shot debounce into the same onPollTick every pickup path shares.
    m_watchDebounce.setSingleShot(true);
    m_watchDebounce.setInterval(kWatchDebounceMs);
    connect(&m_watchDebounce, SIGNAL(timeout()), this, SLOT(onPollTick()));
    connect(&m_watcher, SIGNAL(directoryChanged(QString)), this, SLOT(onWatchedDirChanged(QString)));
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
    log(QString("watching content directories for %1 (file watching + fallback poll)")
            .arg(QString(kJobFileRelPath)));
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

void JobPoller::resumeWatching()
{
    m_state = Polling;
    m_pollTimer.start();
    // A handoff written while the batch ran was invisible (watch events are
    // dropped mid-batch): check NOW, queued so the current call stack — and
    // Daz's own deferred post-batch work — unwinds first. Pre-1.3.0 this
    // waited for the next poll tick.
    QMetaObject::invokeMethod(this, "onPollTick", Qt::QueuedConnection);
}

void JobPoller::rememberIgnored(const QString &path)
{
    const QFileInfo info(path);
    m_ignoredPath = path;
    m_ignoredSize = info.size();
    m_ignoredMtime = info.lastModified();
}

void JobPoller::onWatchedDirChanged(const QString &path)
{
    Q_UNUSED(path)
    // Events during a batch are dropped on purpose (one batch at a time; the
    // batch's own rewrites fire this constantly) — resumeWatching()'s queued
    // check picks up anything that landed meanwhile.
    if (m_state != Polling)
        return;
    // (Re)start: a burst keeps pushing the shot out; the last event wins.
    m_watchDebounce.start();
}

bool JobPoller::syncWatches()
{
    DzContentMgr *mgr = dzApp ? dzApp->getContentMgr() : NULL;
    if (!mgr)
        return false;
    bool allCovered = false;
    for (int i = 0; i < mgr->getNumContentDirectories(); ++i) {
        const QString dir = mgr->getContentDirectoryPath(i)
            + "/Scripts/DTH-Character-Studio";
        if (!QFileInfo(dir).isDir())
            continue; // nothing to watch yet — the fallback poll covers it
        if (m_watcher.directories().contains(dir)) {
            allCovered = true;
            continue;
        }
        // Qt 4.8's addPath returns void (Qt 5+ returns bool) — verify via the
        // list instead, the one spelling both SDK builds share.
        m_watcher.addPath(dir);
        if (m_watcher.directories().contains(dir)) {
            allCovered = true;
            log(QString("file watch armed on %1").arg(dir));
        } else {
            log(QString("could not watch %1 — the fallback poll covers it").arg(dir));
            return false;
        }
    }
    return allCovered;
}

void JobPoller::onPollTick()
{
    if (m_state != Polling)
        return;

    DzContentMgr *mgr = dzApp ? dzApp->getContentMgr() : NULL;
    if (!mgr)
        return;

    // Keep the watches in step with the mapping (dirs appear after installs,
    // watchers forget deleted dirs), and pace the fallback poll by whether
    // they cover everything that exists.
    const int wanted = syncWatches() ? kWatchedPollIntervalMs : kPollIntervalMs;
    if (m_pollTimer.interval() != wanted)
        m_pollTimer.setInterval(wanted);

    // Contract v2 (JSON) first, the legacy CSV as fallback — first mapped
    // content directory wins for each.
    for (int i = 0; i < mgr->getNumContentDirectories(); ++i) {
        const QString candidate = mgr->getContentDirectoryPath(i) + kJobFileRelPath;
        if (QFile::exists(candidate)) {
            pickUpJsonJobFile(candidate);
            return;
        }
    }
    for (int i = 0; i < mgr->getNumContentDirectories(); ++i) {
        const QString candidate = mgr->getContentDirectoryPath(i) + kLegacyJobFileRelPath;
        if (QFile::exists(candidate)) {
            pickUpLegacyCsvJobFile(candidate);
            return;
        }
    }
}

bool JobPoller::pickUpJsonJobFile(const QString &path)
{
    const QFileInfo info(path);
    if (path == m_ignoredPath && info.size() == m_ignoredSize && info.lastModified() == m_ignoredMtime)
        return false; // previously rejected and unchanged — stay silent

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false; // writer may still hold it — retry next tick
    const QByteArray bytes = file.readAll();
    file.close();

    const dthjr::JsonParseResult parsed =
        dthjr::parseJobJson(std::string_view(bytes.constData(), static_cast<size_t>(bytes.size())));

    for (size_t i = 0; i < parsed.warnings.size(); ++i)
        log(QString("job file: %1").arg(QString::fromStdString(parsed.warnings[i])));

    if (!parsed.ok) {
        log(QString("ignoring foreign/corrupt job file %1 (%2) — leaving it in place")
                .arg(path, QString::fromStdString(parsed.error)));
        rememberIgnored(path);
        return false;
    }

    // The RENAME is the "started" signal — the studio can abort (delete) an
    // un-renamed file, never a renamed one. A stale running_ file (an old
    // finished batch nobody cleaned up) would block the rename: remove it.
    const QString runningPath = info.absolutePath() + "/" + kRunningJobFileName;
    if (QFile::exists(runningPath) && !QFile::remove(runningPath)) {
        log(QString("could not clear the stale running job file %1 — retrying next poll").arg(runningPath));
        return false;
    }
    if (!QFile::rename(path, runningPath)) {
        // The studio may have deleted (aborted) or be rewriting it right now.
        log(QString("could not rename job file %1 — retrying next poll").arg(path));
        return false;
    }

    if (parsed.file.jobs.empty()) {
        // Nothing runnable: complete the batch immediately so the studio sees
        // 100 and cleans the file up.
        m_model = parsed.file;
        m_runningPath = runningPath;
        m_model.progress = 100;
        writeRunningFile();
        m_runningPath.clear();
        log("job file contained no runnable rows");
        return true;
    }

    m_model = parsed.file;
    m_runningPath = runningPath;
    // v1.2.0: arm the verbose progress log. The plugin OWNS the file for the
    // batch — truncate whatever a previous run left, so the studio's watcher
    // only ever sees this batch's lines.
    m_progressPath = QString::fromStdString(parsed.file.progressLogPath);
    if (!m_progressPath.isEmpty()) {
        QFile fresh(m_progressPath);
        if (fresh.open(QIODevice::WriteOnly | QIODevice::Truncate))
            fresh.close();
        else
            log(QString("could not truncate the progress log %1 — appending anyway").arg(m_progressPath));
    }

    if (parsed.file.type == "open-scene") {
        // Contract v3: one script-less row — load the scene into THIS Daz and
        // come to the front. Runs as its own single-step batch (same
        // rename/progress bookkeeping, different ending: the scene stays).
        m_pollTimer.stop();
        m_state = RunningBatch;
        m_queue.clear();
        Job job;
        job.scenePath = QString::fromStdString(parsed.file.jobs[0].scenePath);
        m_queue.append(job);
        m_index = 0;
        log(QString("open-scene handoff: %1").arg(job.scenePath));
        QMetaObject::invokeMethod(this, "stepOpenSceneOnly", Qt::QueuedConnection);
        return true;
    }

    QList<Job> jobs;
    for (size_t i = 0; i < parsed.file.jobs.size(); ++i) {
        Job job;
        job.scenePath = QString::fromStdString(parsed.file.jobs[i].scenePath);
        job.scriptPath = QString::fromStdString(parsed.file.jobs[i].scriptPath);
        job.steps = parsed.file.jobs[i].steps;
        jobs.append(job);
    }
    beginBatch(jobs);
    return true;
}

void JobPoller::progressLine(int percent, const QString &message)
{
    if (m_progressPath.isEmpty())
        return;
    QFile file(m_progressPath);
    // Open-append-close per line: the log is low-volume (a handful of lines
    // per scene) and the close is the flush the studio's watcher relies on.
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append))
        return;
    const std::string line =
        dthjr::formatProgressLine(percent, std::string(message.toUtf8().constData()));
    file.write(line.data(), static_cast<qint64>(line.size()));
    file.close();
}

QString JobPoller::currentSceneStem() const
{
    if (m_index < 0 || m_index >= m_queue.size())
        return QString("scene");
    const QString &path = m_queue.at(m_index).scenePath;
    if (path.isEmpty())
        return QString("new scene");
    return QFileInfo(path).completeBaseName();
}

bool JobPoller::ensureSceneSafeToReplace()
{
    if (!dzScene || !dzScene->assetNeedSave())
        return true;
    // The user is likely looking at the studio, not Daz — raise the window so
    // the prompt (and the choice it asks for) is actually seen.
    raiseMainWindow();
    QMessageBox box(dzApp ? dzApp->getInterface() : NULL);
    box.setWindowTitle("Save Changes");
    // Daz's own wording for the manual-open case — this IS that moment, just
    // triggered by a DTH Character Studio job instead of File > Open.
    box.setText("Would you like to save changes to the current scene before closing it?");
    box.setStandardButtons(QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Yes);
    const int choice = box.exec();
    if (choice == QMessageBox::Cancel)
        return false;
    if (choice == QMessageBox::No)
        return true; // discard, exactly like answering Daz's own prompt with No
    // Yes: run Daz's own File > Save action (interactive — an unsaved scene
    // gets the regular Save As dialog). Still dirty afterwards = the user
    // cancelled that dialog, which cancels the batch too.
    DzMainWindow *win = dzApp ? dzApp->getInterface() : NULL;
    DzActionMgr *actions = win ? win->getActionMgr() : NULL;
    DzAction *save = actions ? actions->findAction("DzSaveFileAction") : NULL;
    if (!save && actions)
        save = actions->findAction("DzFileSaveAction");
    if (!save) {
        log("could not find Daz's File > Save action — treating Save Changes as cancelled");
        return false;
    }
    save->trigger();
    return !dzScene->assetNeedSave();
}

void JobPoller::cancelBatch(const QString &reason)
{
    log(QString("batch cancelled: %1").arg(reason));
    progressLine(100, QString("batch cancelled — %1").arg(reason));
    m_progressPath.clear();
    // A user cancel is not an outcome to report — DELETE the job file so
    // nothing lingers and nothing re-runs. (The studio treats a vanished
    // watched file as a dead watch; un-watched handoffs simply disappear.)
    if (!m_runningPath.isEmpty()) {
        if (!QFile::remove(m_runningPath))
            log(QString("could not delete the cancelled job file %1").arg(m_runningPath));
        m_runningPath.clear();
    }
    m_queue.clear();
    m_index = 0;
    resumeWatching();
}

void JobPoller::stepOpenSceneOnly()
{
    // The user's open scene is about to be REPLACED — with THEIR work in it,
    // not a batch row's throwaway keyframes. Same guard as the export batch.
    if (!ensureSceneSafeToReplace()) {
        cancelBatch("cancelled — unsaved changes in the open scene");
        return;
    }
    const Job &job = m_queue.at(0);
    QString error;
    if (!QFile::exists(job.scenePath)) {
        error = "scene not found";
        log(QString("open-scene failed: scene not found: %1").arg(job.scenePath));
    } else if (!dzApp->getContentMgr()->openFile(job.scenePath, false)) {
        // merge=false replaces the current scene without a save prompt.
        error = "failed to open scene";
        log(QString("open-scene failed: could not open %1").arg(job.scenePath));
    } else {
        // The scene was just READ from disk and the user has not touched it, but
        // Daz still flags it as needing a save (post-load fixups mark the asset
        // modified) — so closing Daz asked to save changes nobody made, and the
        // next handoff would hit our own Save Changes guard for nothing. Tell
        // Daz the asset matches its file again (DzScene::assetSaved(), public in
        // both SDKs). Only on the open-scene path: after a BUILD the in-memory
        // scene genuinely differs from the file, and that one is discarded by
        // newEmptyScene() anyway.
        if (dzScene)
            dzScene->assetSaved();
        // The studio can't do this half from outside the process.
        raiseMainWindow();
        log("open-scene done — window raised, scene left loaded (marked unmodified)");
    }
    // One row → this write is also progress 100; the studio deletes the file.
    markRow(error.isEmpty() ? dthjr::JobStatus::Done : dthjr::JobStatus::Failed, error);
    m_runningPath.clear();
    m_progressPath.clear(); // open-scene handoffs write no progress lines
    // Deliberately NO newEmptyScene(): the loaded scene is the whole point.
    m_queue.clear();
    m_index = 0;
    resumeWatching();
}

bool JobPoller::pickUpLegacyCsvJobFile(const QString &path)
{
    const QFileInfo info(path);
    if (path == m_ignoredPath && info.size() == m_ignoredSize && info.lastModified() == m_ignoredMtime)
        return false; // previously rejected and unchanged — stay silent

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false; // writer may still hold it — retry next tick
    const QByteArray bytes = file.readAll();
    file.close();

    const dthjr::ParseResult parsed =
        dthjr::parseJobCsv(std::string_view(bytes.constData(), static_cast<size_t>(bytes.size())));

    for (size_t i = 0; i < parsed.warnings.size(); ++i)
        log(QString("job file: %1").arg(QString::fromStdString(parsed.warnings[i])));

    if (!parsed.ok) {
        log(QString("ignoring foreign/corrupt job file %1 (%2) — leaving it in place")
                .arg(path, QString::fromStdString(parsed.error)));
        rememberIgnored(path);
        return false;
    }

    // Legacy lifecycle: deleting the file is the "transfer succeeded" ack —
    // do it before running anything so a crash mid-batch never re-runs the
    // batch on the next start.
    if (!QFile::remove(path)) {
        log(QString("could not delete job file %1 — NOT running it (a re-poll would double-run)").arg(path));
        rememberIgnored(path);
        return false;
    }

    if (parsed.rows.empty()) {
        log("job file contained no runnable rows");
        return true;
    }

    m_model = dthjr::JobFileModel();
    m_runningPath.clear();  // legacy batches carry no progress file
    m_progressPath.clear(); // …and no verbose progress log either
    QList<Job> jobs;
    for (size_t i = 0; i < parsed.rows.size(); ++i) {
        Job job;
        job.scenePath = QString::fromStdString(parsed.rows[i].scenePath);
        job.scriptPath = QString::fromStdString(parsed.rows[i].scriptPath);
        jobs.append(job);
    }
    beginBatch(jobs);
    return true;
}

void JobPoller::markRow(dthjr::JobStatus status, const QString &error)
{
    if (m_runningPath.isEmpty())
        return; // legacy CSV batch — no progress file
    if (m_index < 0 || static_cast<size_t>(m_index) >= m_model.jobs.size())
        return;
    dthjr::JsonJob &job = m_model.jobs[static_cast<size_t>(m_index)];
    job.status = status;
    job.error = error.isEmpty() ? std::string() : std::string(error.toUtf8().constData());
    // Whole-batch progress = processed rows / total (done + failed count).
    int processed = 0;
    for (size_t i = 0; i < m_model.jobs.size(); ++i) {
        if (m_model.jobs[i].status == dthjr::JobStatus::Done
            || m_model.jobs[i].status == dthjr::JobStatus::Failed)
            ++processed;
    }
    m_model.progress = static_cast<int>(
        (processed * 100) / static_cast<int>(m_model.jobs.size() ? m_model.jobs.size() : 1));
    writeRunningFile();
}

void JobPoller::writeRunningFile()
{
    if (m_runningPath.isEmpty())
        return;
    const std::string text = dthjr::writeJobJson(m_model);
    QFile file(m_runningPath);
    // Plain truncate-rewrite: the studio tolerates a torn read by re-polling.
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        log(QString("could not update the running job file %1").arg(m_runningPath));
        return;
    }
    file.write(text.data(), static_cast<qint64>(text.size()));
    file.close();
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
    // Row 0 replaces the USER'S open scene (later rows replace the previous
    // row's throwaway ROM keyframes — nothing worth prompting for): a dirty
    // scene gets Daz's Save Changes choice first; Cancel cancels the batch.
    if (m_index == 0 && !ensureSceneSafeToReplace()) {
        cancelBatch("cancelled — unsaved changes in the open scene");
        return;
    }
    const Job &job = m_queue.at(m_index);
    const QString stem = currentSceneStem();
    progressLine(0, QString("%1: opening scene").arg(stem));

    if (job.scenePath.isEmpty()) {
        log(QString("row %1/%2: new empty scene").arg(m_index + 1).arg(m_queue.size()));
        newEmptyScene();
    } else if (!QFile::exists(job.scenePath)) {
        log(QString("row %1/%2 skipped: scene not found: %3").arg(m_index + 1).arg(m_queue.size()).arg(job.scenePath));
        progressLine(100, QString("%1: failed — scene not found").arg(stem));
        markRow(dthjr::JobStatus::Failed, "scene not found");
        advanceRow();
        return;
    } else {
        log(QString("row %1/%2: opening %3").arg(m_index + 1).arg(m_queue.size()).arg(job.scenePath));
        // merge=false replaces the current scene without a save prompt
        // (measured behaviour the studio already relies on).
        if (!dzApp->getContentMgr()->openFile(job.scenePath, false)) {
            log(QString("row %1/%2 skipped: failed to open scene: %3")
                    .arg(m_index + 1).arg(m_queue.size()).arg(job.scenePath));
            progressLine(100, QString("%1: failed — could not open the scene").arg(stem));
            markRow(dthjr::JobStatus::Failed, "failed to open scene");
            advanceRow();
            return;
        }
    }
    // Step 1 of the row is done: the scene is open. The export script owns
    // the interior steps and continues this scale in the same file.
    progressLine(job.steps > 0 ? 100 / job.steps : 0,
                 QString("%1: scene opened").arg(stem));

    // Let deferred post-load work settle before the script runs.
    QTimer::singleShot(kSettleMs, this, SLOT(stepExecute()));
}

void JobPoller::stepExecute()
{
    const Job &job = m_queue.at(m_index);
    const QString stem = currentSceneStem();

    if (!QFile::exists(job.scriptPath)) {
        log(QString("row %1/%2 skipped: script not found: %3").arg(m_index + 1).arg(m_queue.size()).arg(job.scriptPath));
        progressLine(100, QString("%1: failed — export script not found").arg(stem));
        markRow(dthjr::JobStatus::Failed, "script not found");
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
        progressLine(100, QString("%1: failed — could not load the export script").arg(stem));
        markRow(dthjr::JobStatus::Failed, "could not load script");
        advanceRow();
        return;
    }

    // Plain execute, no arguments (v1.0.4+): which script the job row names IS
    // the mode — the studio generates a dedicated bulk script. The script
    // appends the interior progress steps (ROM, exports, CSV delivery) to the
    // progress log itself while this call blocks.
    log(QString("row %1/%2: running %3").arg(m_index + 1).arg(m_queue.size()).arg(job.scriptPath));
    markRow(dthjr::JobStatus::Running);
    const bool ok = script->execute(); // synchronous; returns when the ROM + export are done
    disposeScript(script);
    log(QString("row %1/%2: %3").arg(m_index + 1).arg(m_queue.size()).arg(ok ? "done" : "script reported failure"));
    progressLine(100, ok ? QString("%1: done").arg(stem)
                         : QString("%1: failed — the export script reported failure").arg(stem));
    markRow(ok ? dthjr::JobStatus::Done : dthjr::JobStatus::Failed,
            ok ? QString() : QString("script reported failure"));

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
    // Contract v2: progress 100 marks the batch complete; the file is LEFT in
    // place — the STUDIO deletes it once it has read the outcome.
    if (!m_runningPath.isEmpty()) {
        m_model.progress = 100;
        writeRunningFile();
        m_runningPath.clear();
    }
    progressLine(100, "batch finished");
    m_progressPath.clear();
    log("batch finished");
    m_queue.clear();
    m_index = 0;
    resumeWatching();
}

void JobPoller::newEmptyScene()
{
    // DzScene::clear() is the File > New equivalent (dzscene.h, Q_SLOT) — it
    // drops the batch's throwaway ROM keyframes so quitting Daz never prompts
    // to save them (verified by the end-to-end smoke test).
    dzScene->clear();
}
