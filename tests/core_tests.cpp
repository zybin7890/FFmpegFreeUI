// SPDX-License-Identifier: AGPL-3.0-only
#include "jobqueue.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QThread>
#include <QTextStream>
#include <QTimer>
#include <functional>
#include <stdexcept>

namespace {
int checks = 0;
void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); ++checks; }
void rejects(const std::function<void()> &f, const char *message) { bool failed = false; try { f(); } catch (const fui::Error &) { failed = true; } check(failed, message); }
void wait(const std::function<bool()> &ready, int timeout = 30000) {
    QElapsedTimer clock; clock.start(); while (!ready() && clock.elapsed() < timeout) { QCoreApplication::processEvents(); QThread::msleep(10); } check(ready(), "event timeout");
}
void execute(const QString &program, const QStringList &args) {
    QProcess p; p.start(program, args); check(p.waitForFinished(30000), "process timeout"); if (p.exitCode()) throw std::runtime_error(p.readAllStandardError().toStdString()); ++checks;
}
QJsonObject preset() {
    auto p = fui::defaults(); p[QStringLiteral("视频参数_编码器_具体编码")] = "libx264"; p[QStringLiteral("视频参数_编码器_编码预设")] = "ultrafast";
    p[QStringLiteral("视频参数_比特率_控制方式")] = 1; p[QStringLiteral("视频参数_质量控制_值")] = "25"; p[QStringLiteral("音频参数_编码器_代号")] = "aac.native"; return p;
}
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir dir; check(dir.isValid(), "temporary directory");
        check(fui::schema().size() == 186, "upstream schema coverage"); check(fui::builtins().size() == 11, "built-in preset coverage");
        auto p = preset(); p["unknown-future-field"] = QJsonObject{{"value", 7}};
        fui::saveJson(dir.filePath("test.3fui"), p); check(fui::loadPreset(dir.filePath("test.3fui")) == p, "preset lossless roundtrip");
        check(fui::tokenize("-i \"C:\\media files\\中文.mp4\" -vf 'scale=160:90' -metadata title=\"a b\"") == QStringList{"-i", QStringLiteral("C:\\media files\\中文.mp4"), "-vf", "scale=160:90", "-metadata", "title=a b"}, "argument tokenization");
        rejects([] { fui::tokenize("-i 'broken"); }, "unbalanced quote rejection");
        rejects([&] { fui::compilePreset(p, "same.mp4", "same.mp4", "ffmpeg"); }, "input overwrite rejection");
        const auto ffmpeg = fui::resolveTool("ffmpeg"), ffprobe = fui::resolveTool("ffprobe");
        const auto input = dir.filePath(QStringLiteral("中文 input 'one'.mkv"));
        auto names = preset(); names[QStringLiteral("输出_自动命名选项")] = 5;
        check(QRegularExpression("_[0-9]{8}\\.mp4$").match(fui::outputPath(names, input, dir.path())).hasMatch(), "numeric random output naming");
        names[QStringLiteral("输出_自动命名选项")] = 9;
        check(QRegularExpression("_[A-Za-z]{16}\\.mp4$").match(fui::outputPath(names, input, dir.path())).hasMatch(), "alphabetic random output naming");
        names[QStringLiteral("输出_自动命名选项")] = 11;
        auto numbered = fui::outputPath(names, input, dir.path());
        check(numbered.endsWith("01.mp4") && fui::outputPath(names, input, dir.path(), {numbered}).endsWith("02.mp4"), "numbered outputs start at one");
        auto aliases = preset(); aliases[QStringLiteral("音频参数_编码器_代号")] = "aac.nmr";
        const auto audioArgs = fui::compilePreset(aliases, input, dir.filePath("aliases.mp4"), ffmpeg).steps[0].args;
        check(audioArgs.value(audioArgs.indexOf("-aac_coder") + 1) == "nmr", "upstream audio alias default options");
        aliases = preset(); aliases[QStringLiteral("剪辑区间_方法")] = 3; aliases[QStringLiteral("剪辑区间_入点")] = "12"; aliases[QStringLiteral("剪辑区间_出点")] = "15"; aliases[QStringLiteral("剪辑区间_向前解码多久秒")] = "5";
        const auto seekArgs = fui::compilePreset(aliases, input, dir.filePath("seek.mp4"), ffmpeg, 20).steps[0].args;
        check(seekArgs.value(seekArgs.indexOf("-ss") + 1) == "7" && seekArgs.value(seekArgs.lastIndexOf("-ss") + 1) == "5", "accurate input and output seek");
        aliases = preset(); aliases[QStringLiteral("自定义参数_完全自己写")] = "-i <InputFile> -metadata title=\"<InputFileName>\" -metadata comment=\"<InputFileNameWithOutExtension>\" -metadata album=\"<InputFilePath>\" <OutputFile>";
        const auto placeholderArgs = fui::compilePreset(aliases, input, dir.filePath("names.mp4"), ffmpeg).steps[0].args;
        check(placeholderArgs.contains("title=" + QFileInfo(input).fileName()) && placeholderArgs.contains("comment=" + QFileInfo(input).completeBaseName()) && placeholderArgs.contains("album=" + QFileInfo(input).absolutePath()), "upstream filename and directory placeholders");
        execute(ffmpeg, {"-v", "error", "-f", "lavfi", "-i", "testsrc2=size=160x90:rate=10", "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000", "-t", "2", "-c:v", "libx264", "-preset", "ultrafast", "-c:a", "pcm_s16le", input});
        const auto preservedTime = QDateTime::fromSecsSinceEpoch(1600000000);
        { QFile original(input); check(original.open(QIODevice::ReadWrite) && original.setFileTime(preservedTime, QFileDevice::FileModificationTime), "source timestamp setup"); }
        p[QStringLiteral("输出命名_保留修改时间")] = true;
        check(fui::probeDuration(input, ffprobe) > 1.9, "ffprobe duration");
        p[QStringLiteral("视频参数_分辨率")] = "128x72";
        fui::JobQueue queue; auto job = queue.add(input, dir.filePath("out.mp4"), p); queue.start();
        wait([&] { return job->state == fui::State::Done || job->state == fui::State::Failed; });
        if (job->state != fui::State::Done) throw std::runtime_error(job->log.toStdString());
        check(QFileInfo(job->output).size() > 0 && job->percent == 100, "queue real transcode");
        check(QFileInfo(job->output).lastModified().toSecsSinceEpoch() == preservedTime.toSecsSinceEpoch(), "output modification time preserved");
        QProcess probe; probe.start(ffprobe, {"-v", "error", "-show_entries", "stream=width,height,codec_name", "-of", "json", job->output}); check(probe.waitForFinished(), "probe output");
        const auto streams = QJsonDocument::fromJson(probe.readAllStandardOutput()).object().value("streams").toArray();
        check(streams[0].toObject().value("width").toInt() == 128 && streams[0].toObject().value("height").toInt() == 72, "filter dimensions");
        check(streams[1].toObject().value("codec_name").toString() == "aac", "upstream audio codec ID");
        p[QStringLiteral("视频参数_比特率_控制方式")] = 6; p[QStringLiteral("视频参数_比特率_基础")] = "200k";
        auto passes = fui::compilePreset(p, input, dir.filePath("two.mp4"), ffmpeg, 2, dir.filePath("pass"));
        check(passes.steps.size() == 2 && passes.steps[0].args.contains("null"), "two-pass plan");
        execute(passes.steps[0].program, passes.steps[0].args); execute(passes.steps[1].program, passes.steps[1].args); check(QFileInfo(dir.filePath("two.mp4")).size() > 0, "real two-pass encoding");
        p = preset(); p[QStringLiteral("视频参数_编码器_具体编码")] = "copy"; p[QStringLiteral("视频参数_分辨率")] = "128x72";
        rejects([&] { fui::compilePreset(p, input, dir.filePath("bad.mp4"), ffmpeg); }, "copy plus filter rejection");
        p = preset(); p[QStringLiteral("视频参数_NV_FRUC_目标帧率")] = "60";
        rejects([&] { fui::compilePreset(p, input, dir.filePath("bad.mp4"), ffmpeg); }, "unsupported active preset rejection");
        p = preset(); auto name = fui::outputPath(p, input, dir.path()); check(fui::outputPath(p, input, dir.path(), {name}) != name, "batch output collision prevention");
        const auto sentinel = dir.filePath("existing.mp4"); { QFile file(sentinel); check(file.open(QIODevice::WriteOnly), "sentinel open"); file.write("preserve"); }
        auto failure = queue.add(input, sentinel, p); queue.start(); wait([&] { return failure->state == fui::State::Failed; });
        { QFile file(sentinel); file.open(QIODevice::ReadOnly); check(file.readAll() == "preserve", "existing output preserved"); }
        fui::JobQueue controls;
        auto slowPreset = preset(); slowPreset[QStringLiteral("自定义参数_开头参数")] = "-re";
        auto slow = controls.add(input, dir.filePath("slow.mp4"), slowPreset); controls.start(); wait([&] { return slow->state == fui::State::Running && slow->process && slow->process->processId() > 0; });
        controls.pause(slow->id); check(slow->state == fui::State::Paused, "native process pause");
        controls.resume(slow->id); check(slow->state == fui::State::Running, "native process resume");
        controls.cancel(slow->id); wait([&] { return slow->state == fui::State::Cancelled && !slow->process; }); check(!controls.active(), "cancel releases process");
        fui::JobQueue recovery; auto pending = recovery.add(input, dir.filePath("recover.mp4"), preset()); recovery.save(dir.filePath("queue.json"));
        fui::JobQueue restored; restored.restore(dir.filePath("queue.json")); check(restored.jobs.size() == 1 && restored.jobs[0]->state == fui::State::Pending && restored.jobs[0]->preset == pending->preset, "atomic queue recovery");
        auto custom = preset(); custom[QStringLiteral("自定义参数_完全自己写")] = "-i \"<InputFile>\" -c copy -y \"<OutputFile>\"";
        auto command = fui::compilePreset(custom, input, dir.filePath("manual.mkv"), ffmpeg); check(command.steps[0].args.contains(input) && !command.steps[0].args.contains("-y"), "placeholder substitution without shell");
        execute(command.steps[0].program, command.steps[0].args);
        QTextStream(stdout) << "PASS " << checks << " checks, real FFmpeg transcode, two-pass, pause/resume/cancel, preset and queue recovery\n"; return 0;
    } catch (const std::exception &e) { QTextStream(stderr) << "FAIL: " << QString::fromUtf8(e.what()) << '\n'; return 1; }
}
