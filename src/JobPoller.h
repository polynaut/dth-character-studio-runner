#pragma once

// Main-thread poller + batch runner for the DTH exporter job file.
//
// Every Daz API call (DzScript included) must happen on the main thread, so
// the whole thing is a main-thread QObject driven by a QTimer — no worker
// threads, no marshalling. Batch steps hop through the event loop as queued
// slots so the UI stays alive and Daz's own progress dialogs can paint.
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
// Qt note: string-based connect/invokeMethod/singleShot only — this file
// compiles against both Qt 4.8 (DS4 SDK) and Qt 6 (DS6 SDK).

#include "JsonJobFile.h"

#include <QtCore/QDateTime>
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
    void stepOpenScene();
    void stepExecute();

private:
    JobPoller();

    struct Job {
        QString scenePath;  // empty = new empty scene
        QString scriptPath;
    };

    // Job-file discovery per contract: JSON (v2) first, legacy CSV second.
    bool pickUpJsonJobFile(const QString &path);
    bool pickUpLegacyCsvJobFile(const QString &path);

    void beginBatch(const QList<Job> &jobs);
    void advanceRow();
    void finishBatch();
    void newEmptyScene();
    void rememberIgnored(const QString &path);
    static void log(const QString &message);

    // v2 progress writing: update the current row's status/error in the model
    // and rewrite the running_ file (no-op for legacy CSV batches).
    void markRow(dthjr::JobStatus status, const QString &error = QString());
    void writeRunningFile();

    enum State { Stopped, Polling, RunningBatch };

    State m_state;
    bool m_started;
    QTimer m_pollTimer;
    QList<Job> m_queue;
    int m_index;

    // Contract-v2 batch state: the parsed model the running file is rewritten
    // from, and where it lives. Empty path = legacy CSV batch (no progress).
    dthjr::JobFileModel m_model;
    QString m_runningPath;

    // Signature of a file we refused to parse (foreign/corrupt) or could not
    // delete/rename — skip it silently until it changes, so it doesn't spam
    // the log.
    QString m_ignoredPath;
    qint64 m_ignoredSize;
    QDateTime m_ignoredMtime;
};
