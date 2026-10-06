// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "preset.h"
#include <QObject>
#include <QElapsedTimer>
#include <QDateTime>
#include <QProcess>
#include <QSharedPointer>
#include <QTemporaryDir>
#include <memory>

namespace fui {
enum class State { Pending, Probing, Running, Paused, Done, Failed, Cancelled };
QString stateName(State state);
struct Job {
    QString id, input, output, name, log;
    QJsonObject preset;
    Plan plan;
    State state = State::Pending;
    QProcess *process = nullptr;
    QElapsedTimer elapsed;
    QDateTime sourceCreated, sourceModified, sourceAccessed;
    QByteArray stdoutBuffer, stderrBuffer;
    std::unique_ptr<QTemporaryDir> cache;
    int step = 0;
    double percent = 0, speed = 0, fps = 0, mediaSeconds = 0;
    qint64 size = 0;
    bool explicitPlan = false;
    bool active() const { return process || state == State::Probing || state == State::Running || state == State::Paused; }
};
class JobQueue : public QObject {
    Q_OBJECT
public:
    explicit JobQueue(QObject *parent = nullptr);
    ~JobQueue() override;
    QList<QSharedPointer<Job>> jobs;
    QString ffmpeg, ffprobe;
    int concurrency = 1;
    QSharedPointer<Job> add(const QString &input, const QString &output, const QJsonObject &preset);
    QSharedPointer<Job> addPlan(const QString &name, const QString &input, const QString &output, const Plan &plan);
    void start();
    void pause(const QString &id);
    void resume(const QString &id);
    void cancel(const QString &id);
    void stop();
    void reset(const QString &id);
    void remove(const QString &id);
    void move(const QString &id, int delta);
    bool active() const;
    QSharedPointer<Job> find(const QString &id) const;
    void save(const QString &path) const;
    void restore(const QString &path);
signals:
    void changed();
    void idle();
    void error(const QString &message);
private:
    bool started_ = false, shuttingDown_ = false;
    void schedule();
    void prepare(const QSharedPointer<Job> &job);
    void runStep(const QSharedPointer<Job> &job);
    void finish(const QSharedPointer<Job> &job, State state, const QString &message);
    void consume(const QSharedPointer<Job> &job, bool final = false);
    void append(const QSharedPointer<Job> &job, const QString &message);
};
}
