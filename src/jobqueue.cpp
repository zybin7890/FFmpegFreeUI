// SPDX-License-Identifier: AGPL-3.0-only
#include "jobqueue.h"
#include "platform.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTimer>
#include <QUuid>
#include <algorithm>

namespace fui {
QString stateName(State s) {
    switch (s) {
    case State::Pending: return QStringLiteral("等待"); case State::Probing: return QStringLiteral("读取媒体");
    case State::Running: return QStringLiteral("运行中"); case State::Paused: return QStringLiteral("已暂停");
    case State::Done: return QStringLiteral("完成"); case State::Failed: return QStringLiteral("失败");
    case State::Cancelled: return QStringLiteral("已停止");
    } return {};
}
JobQueue::JobQueue(QObject *parent) : QObject(parent) {}
JobQueue::~JobQueue() {
    shuttingDown_ = true;
    for (const auto &job : jobs) if (job->process) {
        disconnect(job->process, nullptr, this, nullptr);
        if (job->state == State::Paused) suspendProcess(job->process->processId(), false);
        job->process->kill(); job->process->waitForFinished(2000);
    }
}
QSharedPointer<Job> JobQueue::find(const QString &id) const { for (const auto &job : jobs) if (job->id == id) return job; return {}; }
bool JobQueue::active() const { for (const auto &job : jobs) if (job->active()) return true; return false; }
QSharedPointer<Job> JobQueue::add(const QString &in, const QString &out, const QJsonObject &preset) {
    if (!QFileInfo(in).isFile()) throw Error(QStringLiteral("输入文件不存在：") + in);
    if (QFileInfo(in).absoluteFilePath() == QFileInfo(out).absoluteFilePath() || (!QFileInfo(in).canonicalFilePath().isEmpty() && QFileInfo(in).canonicalFilePath() == QFileInfo(out).canonicalFilePath())) throw Error(QStringLiteral("输出不能覆盖输入文件"));
    for (const auto &job : jobs) if (QFileInfo(job->output).absoluteFilePath() == QFileInfo(out).absoluteFilePath()) throw Error(QStringLiteral("输出路径已被队列占用：") + out);
    auto job = QSharedPointer<Job>::create(); job->id = QUuid::createUuid().toString(QUuid::Id128);
    job->input = QFileInfo(in).absoluteFilePath(); job->output = QFileInfo(out).absoluteFilePath();
    job->name = QFileInfo(in).fileName(); job->preset = preset; jobs << job; emit changed();
    if (started_) QTimer::singleShot(0, this, &JobQueue::schedule);
    return job;
}
QSharedPointer<Job> JobQueue::addPlan(const QString &name, const QString &in, const QString &out, const Plan &plan) {
    if (plan.steps.isEmpty()) throw Error(QStringLiteral("工具任务不能为空"));
    auto job = add(in, out, {}); job->name = name; job->plan = plan; job->explicitPlan = true; emit changed(); return job;
}
void JobQueue::start() { started_ = true; schedule(); }
void JobQueue::schedule() {
    if (!started_ || shuttingDown_) return;
    int running = 0; for (const auto &job : jobs) if (job->active()) ++running;
    for (const auto &job : jobs) if (running < std::clamp(concurrency, 1, 8) && job->state == State::Pending) { ++running; prepare(job); }
    bool pending = false; for (const auto &job : jobs) if (job->state == State::Pending) pending = true;
    if (!pending && !active()) { started_ = false; emit idle(); }
}
void JobQueue::append(const QSharedPointer<Job> &job, const QString &message) {
    job->log += message; if (!message.endsWith('\n')) job->log += '\n';
    if (job->log.size() > 131072) job->log.remove(0, job->log.size() - 131072);
}
void JobQueue::prepare(const QSharedPointer<Job> &job) {
    job->step = 0; job->percent = 0; job->elapsed.start();
    const QFileInfo original(job->input); job->sourceCreated = original.birthTime(); job->sourceModified = original.lastModified(); job->sourceAccessed = original.lastRead();
    if (job->explicitPlan) { runStep(job); return; }
    try {
        if (QFileInfo::exists(job->output)) throw Error(QStringLiteral("输出已经存在，未覆盖：") + job->output);
        if (!QDir().mkpath(QFileInfo(job->output).absolutePath())) throw Error(QStringLiteral("无法创建输出目录"));
        ffmpeg = resolveTool("ffmpeg", ffmpeg); ffprobe = resolveTool("ffprobe", ffprobe);
        job->cache = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/ffmpegfreeui-XXXXXX");
        if (!job->cache->isValid()) throw Error(QStringLiteral("无法创建任务临时目录"));
        job->state = State::Probing; auto *probe = new QProcess(this); job->process = probe;
        connect(probe, &QProcess::finished, this, [this, job, probe](int, QProcess::ExitStatus) {
            if (job->process != probe) return;
            const auto data = QJsonDocument::fromJson(probe->readAllStandardOutput());
            const double duration = data.object().value("format").toObject().value("duration").toString().toDouble();
            probe->deleteLater(); job->process = nullptr;
            if (job->state == State::Cancelled) { finish(job, State::Cancelled, QStringLiteral("任务已停止")); return; }
            try { job->plan = compilePreset(job->preset, job->input, job->output, ffmpeg, duration, job->cache->filePath("pass")); runStep(job); }
            catch (const std::exception &e) { finish(job, State::Failed, QString::fromUtf8(e.what())); }
        });
        connect(probe, &QProcess::errorOccurred, this, [this, job, probe](QProcess::ProcessError e) {
            if (e == QProcess::FailedToStart && job->process == probe) { job->process = nullptr; probe->deleteLater(); finish(job, State::Failed, probe->errorString()); }
        });
        probe->start(ffprobe, {"-v", "error", "-show_entries", "format=duration", "-of", "json", job->input});
        QTimer::singleShot(15000, probe, [probe] { if (probe->state() != QProcess::NotRunning) probe->kill(); });
        emit changed();
    } catch (const std::exception &e) { finish(job, State::Failed, QString::fromUtf8(e.what())); }
}
void JobQueue::runStep(const QSharedPointer<Job> &job) {
    if (job->step >= job->plan.steps.size()) { finish(job, State::Done, QStringLiteral("任务完成")); return; }
    const auto command = job->plan.steps[job->step];
    job->state = State::Running; job->stdoutBuffer.clear(); job->stderrBuffer.clear();
    append(job, command.label + '\n' + displayCommand(command));
    auto *process = new QProcess(this); job->process = process;
    connect(process, &QProcess::readyReadStandardOutput, this, [this, job] { consume(job); });
    connect(process, &QProcess::readyReadStandardError, this, [this, job] { consume(job); });
    connect(process, &QProcess::errorOccurred, this, [this, job, process](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart && job->process == process) { job->process = nullptr; process->deleteLater(); finish(job, State::Failed, process->errorString()); }
    });
    connect(process, &QProcess::finished, this, [this, job, process](int code, QProcess::ExitStatus status) {
        if (job->process != process) return;
        consume(job, true); process->deleteLater(); job->process = nullptr;
        if (job->state == State::Cancelled) { finish(job, State::Cancelled, QStringLiteral("任务已停止，部分输出文件保留")); return; }
        if (code || status == QProcess::CrashExit) { finish(job, State::Failed, QStringLiteral("进程失败，退出码 %1").arg(code)); return; }
        ++job->step; runStep(job);
    });
    process->start(command.program, command.args); emit changed();
}
void JobQueue::consume(const QSharedPointer<Job> &job, bool final) {
    if (!job->process) return;
    job->stdoutBuffer += job->process->readAllStandardOutput(); job->stderrBuffer += job->process->readAllStandardError();
    int idx;
    while ((idx = job->stdoutBuffer.indexOf('\n')) >= 0) {
        auto line = QString::fromUtf8(job->stdoutBuffer.left(idx)).trimmed(); job->stdoutBuffer.remove(0, idx + 1);
        auto key = line.section('=', 0, 0), value = line.section('=', 1);
        if (key == "out_time_us") job->mediaSeconds = value.toDouble() / 1000000.0;
        else if (key == "speed") job->speed = value.remove('x').toDouble();
        else if (key == "fps") job->fps = value.toDouble();
        else if (key == "total_size") job->size = value.toLongLong();
        else if (key == "progress") {
            if (job->plan.duration > 0) job->percent = std::clamp(100.0 * (job->step + job->mediaSeconds / job->plan.duration) / job->plan.steps.size(), 0.0, 99.9);
        }
    }
    while ((idx = job->stderrBuffer.indexOf('\n')) >= 0) { append(job, QString::fromUtf8(job->stderrBuffer.left(idx))); job->stderrBuffer.remove(0, idx + 1); }
    if (job->stderrBuffer.size() > 131072 || final) { append(job, QString::fromUtf8(job->stderrBuffer)); job->stderrBuffer.clear(); }
    if (job->stdoutBuffer.size() > 131072 || final) { if (!job->stdoutBuffer.isEmpty()) append(job, QString::fromUtf8(job->stdoutBuffer)); job->stdoutBuffer.clear(); }
    emit changed();
}
void JobQueue::finish(const QSharedPointer<Job> &job, State state, const QString &message) {
    if (state == State::Done && QFileInfo::exists(job->output)) {
        QFile output(job->output);
        for (const auto &v : QList<QPair<QString, QPair<QFileDevice::FileTime, QDateTime>>>{
            {QStringLiteral("创建"), {QFileDevice::FileBirthTime, job->sourceCreated}},
            {QStringLiteral("修改"), {QFileDevice::FileModificationTime, job->sourceModified}},
            {QStringLiteral("访问"), {QFileDevice::FileAccessTime, job->sourceAccessed}}}) {
            if (!job->preset.value(QStringLiteral("输出命名_保留") + v.first + QStringLiteral("时间")).toBool()) continue;
            if ((!output.isOpen() && !output.open(QIODevice::ReadWrite)) || !v.second.second.isValid() || !output.setFileTime(v.second.second, v.second.first)) {
                state = State::Failed; append(job, QStringLiteral("转码结束，但无法保留%1时间：%2").arg(v.first, output.errorString()));
            }
        }
    }
    job->state = state; if (state == State::Done) { job->percent = 100; job->size = QFileInfo(job->output).size(); }
    append(job, message); job->cache.reset(); emit changed(); QTimer::singleShot(0, this, &JobQueue::schedule);
}
void JobQueue::pause(const QString &id) {
    auto job = find(id); if (!job || job->state != State::Running || !job->process) return;
    QString message; if (suspendProcess(job->process->processId(), true, &message)) { job->state = State::Paused; emit changed(); } else emit error(message);
}
void JobQueue::resume(const QString &id) {
    auto job = find(id); if (!job || job->state != State::Paused || !job->process) return;
    QString message; if (suspendProcess(job->process->processId(), false, &message)) { job->state = State::Running; emit changed(); } else emit error(message);
}
void JobQueue::cancel(const QString &id) {
    auto job = find(id); if (!job || job->state == State::Done) return;
    if (job->process) {
        if (job->state == State::Paused) suspendProcess(job->process->processId(), false);
        job->state = State::Cancelled; job->process->terminate();
        auto *process = job->process; QTimer::singleShot(2000, process, [process] { if (process->state() != QProcess::NotRunning) process->kill(); });
    } else job->state = State::Cancelled;
    emit changed();
}
void JobQueue::stop() { started_ = false; for (const auto &job : jobs) if (job->active()) cancel(job->id); }
void JobQueue::reset(const QString &id) { auto job = find(id); if (!job || job->active() || job->process) return; job->state = State::Pending; job->percent = 0; job->log.clear(); emit changed(); }
void JobQueue::remove(const QString &id) { auto job = find(id); if (!job || job->active() || job->process) return; jobs.removeAll(job); emit changed(); }
void JobQueue::move(const QString &id, int delta) { auto job = find(id); if (!job || job->active()) return; int i = jobs.indexOf(job), j = i + delta; if (j >= 0 && j < jobs.size() && !jobs[j]->active()) { jobs.swapItemsAt(i, j); emit changed(); } }
void JobQueue::save(const QString &path) const {
    QJsonArray list;
    for (const auto &job : jobs) if (job->state != State::Done) {
        QJsonArray steps; if (job->explicitPlan) for (const auto &c : job->plan.steps) steps << QJsonObject{{"program", c.program}, {"args", QJsonArray::fromStringList(c.args)}, {"label", c.label}};
        list << QJsonObject{{"input", job->input}, {"output", job->output}, {"name", job->name}, {"preset", job->preset}, {"steps", steps}, {"duration", job->plan.duration}};
    }
    saveJson(path, {{"version", 1}, {"jobs", list}});
}
void JobQueue::restore(const QString &path) {
    QFile file(path); if (!file.exists()) return; if (!file.open(QIODevice::ReadOnly)) throw Error(file.errorString());
    QJsonParseError error; auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error || !document.isObject() || document.object().value("version").toInt() != 1) throw Error(QStringLiteral("队列缓存格式错误"));
    for (const auto &v : document.object().value("jobs").toArray()) {
        const auto j = v.toObject(); const auto in = j.value("input").toString(); if (!QFileInfo::exists(in)) continue;
        auto job = add(in, j.value("output").toString(), j.value("preset").toObject()); job->name = j.value("name").toString();
        for (const auto &c : j.value("steps").toArray()) { auto o = c.toObject(); QStringList args; for (const auto &a : o.value("args").toArray()) args << a.toString(); job->plan.steps << Command{o.value("program").toString(), args, o.value("label").toString()}; }
        job->plan.duration = j.value("duration").toDouble(); job->explicitPlan = !job->plan.steps.isEmpty();
    }
}
}
