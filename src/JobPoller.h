#pragma once

// Main-thread watcher + batch runner for the DTH exporter job file.
//
// Every Daz API call (DzScript included) must happen on the main thread, so
// the whole thing is a main-thread QObject driven by Qt signals — no worker
// threads, no marshalling. Batch steps hop through the event loop as queued
// slots so the UI stays alive and Daz's own progress dialogs can paint.
//
// Pickup is REAL FILE WATCHING first (v1.3.0): a QFileSystemWatcher on every
// content dir's Scripts/DTH-Character-Studio/ turns the studio's handoff
// write into a near-instant pickup instead of a poll-interval wait. The
// QTimer stays underneath as the safety net — change notification is
// best-effort on network shares (a Daz library on a NAS is a real
// deployment), and a scripts dir that doesn't exist yet can't be watched at
// all — slowed way down while the watches cover every existing scripts dir,
// back to the classic few-seconds poll while they don't. The fallback tick
// doubles as the watch-list re-sync, so a dir created after startup (the
// studio's first runtime install) gets its watch there.
//
// Contract v2 (JSON, dth_exporter_jobs.json): on pickup the file is RENAMED
// to running_dth_exporter_jobs.json — the "started" signal (the studio can
// only abort an un-renamed file). While the batch runs, the renamed file is
// rewritten after every row with per-row statuses + the whole-batch progress
// (0-100). At the end progress hits 100 and the file is LEFT for the studio,
// which deletes it and reports the outcome. The legacy CSV contract
// (dth_exporter_jobs.csv: parse → delete-as-ack → run, no progress) stays
// supported for older studios.
//
// Contract v3 adds `type: "open-scene"`: the same envelope carrying ONE
// script-less row — load that scene into THIS (running) Daz instance and
// raise the main window, because the studio outside the process can do
// neither once a scene is loaded. Same rename/progress bookkeeping; the one
// deliberate difference is the ending — the scene STAYS loaded (no new empty
// scene: nothing ran, there are no throwaway ROM keyframes to discard).
//
// Qt note: string-based connect/invokeMethod/singleShot only — this file
// compiles against both Qt 4.8 (DS4 SDK) and Qt 6 (DS6 SDK).

#include "JsonJobFile.h"

#include <QtCore/QDateTime>
#include <QtCore/QFileSystemWatcher>
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QTimer>

class JobPoller : public QObject
{
    Q_OBJECT
public:
    static JobPoller &instance();

    // Idempotent. Defers the first poll a few seconds so Daz finishes
    // starting up and the content-directory mapping is ready.
    void start();

public slots:
    // Manual trigger ("check for jobs now"). No-op while a batch runs.
    void checkNow();

private slots:
    void beginPolling();
    void onPollTick();
    // A watched scripts dir changed. Debounced into onPollTick: the studio
    // writes temp-file-then-rename, so one handoff is a burst of events.
    void onWatchedDirChanged(const QString &path);
    void stepOpenScene();
    void stepExecute();
    void stepOpenSceneOnly(); // the whole open-scene (contract v3) batch

private:
    JobPoller();

    struct Job {
        QString scenePath;  // empty = new empty scene
        QString scriptPath;
        int steps = 0;      // per-scene step count for the progress log (0 = unknown)
    };

    // Job-file discovery per contract: JSON (v2) first, legacy CSV second.
    bool pickUpJsonJobFile(const QString &path);
    bool pickUpLegacyCsvJobFile(const QString &path);

    void beginBatch(const QList<Job> &jobs);
    void advanceRow();
    void finishBatch();
    void newEmptyScene();
    // The user's-scene guard: prompt (Daz-style Save Changes) when the OPEN
    // scene has unsaved changes before the batch replaces it. False = cancel.
    bool ensureSceneSafeToReplace();
    // Mark every not-yet-processed row failed with `reason`, write progress
    // 100 and return to polling — the batch-wide cancel.
    void cancelBatch(const QString &reason);
    void rememberIgnored(const QString &path);
    static void log(const QString &message);

    // v2 progress writing: update the current row's status/error in the model
    // and rewrite the running_ file (no-op for legacy CSV batches).
    void markRow(dthjr::JobStatus status, const QString &error = QString());
    void writeRunningFile();

    // v1.2.0 verbose progress log: append "[<percent>] <message>" to the
    // job's progressLogPath (no-op when the job carries none). The percent is
    // PER-SCENE (see JsonJobFile.h); the studio-generated export script
    // appends the interior steps to the same file.
    void progressLine(int percent, const QString &message);
    // The display stem of the current row's scene ("new scene" for an empty
    // row) — how the progress log names the scene it is working.
    QString currentSceneStem() const;

    // Bring m_watcher in line with the CURRENT content-dir mapping: add a
    // watch for every existing Scripts/DTH-Character-Studio dir not yet
    // watched (QFileSystemWatcher forgets deleted dirs on its own). Returns
    // whether the watches cover every candidate that exists right now — what
    // decides the fallback timer's pace. Watching a dir that later leaves the
    // mapping is harmless: its events find no file and change nothing.
    bool syncWatches();
    // The batch is over — back to watching, plus one QUEUED immediate check:
    // a job file written while the batch ran (watch events are dropped then)
    // is picked up now, not a poll interval later.
    void resumeWatching();

    enum State { Stopped, Polling, RunningBatch };

    State m_state;
    bool m_started;
    QTimer m_pollTimer;
    QFileSystemWatcher m_watcher;
    // Single-shot collapse of an event burst into ONE check.
    QTimer m_watchDebounce;
    QList<Job> m_queue;
    int m_index;

    // Contract-v2 batch state: the parsed model the running file is rewritten
    // from, and where it lives. Empty path = legacy CSV batch (no progress).
    dthjr::JobFileModel m_model;
    QString m_runningPath;
    // The verbose progress log (v1.2.0). Empty = the job carried none.
    QString m_progressPath;

    // Signature of a file we refused to parse (foreign/corrupt) or could not
    // delete/rename — skip it silently until it changes, so it doesn't spam
    // the log.
    QString m_ignoredPath;
    qint64 m_ignoredSize;
    QDateTime m_ignoredMtime;
};
