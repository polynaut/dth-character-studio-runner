#pragma once

// Main-thread poller + batch runner for the DTH exporter job file.
//
// Every Daz API call (DzScript included) must happen on the main thread, so
// the whole thing is a main-thread QObject driven by a QTimer — no worker
// threads, no marshalling. Batch steps hop through the event loop as queued
// slots so the UI stays alive and Daz's own progress dialogs can paint.
//
// Qt note: string-based connect/invokeMethod/singleShot only — this file
// compiles against both Qt 4.8 (DS4 SDK) and Qt 6 (DS6 SDK).

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

    void beginBatch(const QList<Job> &jobs);
    void advanceRow();
    void finishBatch();
    void newEmptyScene();
    void rememberIgnored(const QString &path);
    static void log(const QString &message);

    enum State { Stopped, Polling, RunningBatch };

    State m_state;
    bool m_started;
    QTimer m_pollTimer;
    QList<Job> m_queue;
    int m_index;

    // Signature of a file we refused to parse (foreign/corrupt) or could not
    // delete — skip it silently until it changes, so it doesn't spam the log.
    QString m_ignoredPath;
    qint64 m_ignoredSize;
    QDateTime m_ignoredMtime;
};
