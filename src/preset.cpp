// SPDX-License-Identifier: AGPL-3.0-only
#include "preset.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSet>
#include <QUuid>
#include <algorithm>
#include <cmath>

static void initializeResources() { Q_INIT_RESOURCE(resources); }
namespace fui {
namespace {
QJsonDocument resource(const QString &name) {
    initializeResources(); QFile f(QStringLiteral(":/fui/") + name);
    if (!f.open(QIODevice::ReadOnly)) throw Error(QStringLiteral("无法读取内置数据：") + name);
    return QJsonDocument::fromJson(f.readAll());
}
QString s(const QJsonObject &p, const QString &key) {
    const auto v = p.value(key);
    return v.isDouble() ? QString::number(v.toDouble(), 'g', 12) : v.toString().trimmed();
}
void opt(QStringList &a, const QString &name, const QString &value) { if (!value.isEmpty()) a << name << value; }
QString sub(QString v, const QString &in, const QString &out) {
    v.replace(QStringLiteral("<InputFile>"), in).replace(QStringLiteral("<输入文件>"), in);
    v.replace(QStringLiteral("<OutputFile>"), out).replace(QStringLiteral("<输出文件>"), out);
    v.replace("<InputDir>", QFileInfo(in).absolutePath()).replace("<InputFilePath>", QFileInfo(in).absolutePath());
    v.replace("<InputFileName>", QFileInfo(in).fileName()).replace("<InputFileNameWithOutExtension>", QFileInfo(in).completeBaseName());
    v.replace("<InputFileWithOutExtension>", QDir(QFileInfo(in).absolutePath()).filePath(QFileInfo(in).completeBaseName()));
    return v;
}
void raw(QStringList &a, const QString &v, const QString &in, const QString &out) { for (const auto &t : tokenize(v)) a << sub(t, in, out); }
QString escapeFilter(QString v) {
    // Two parsers consume this string: the filtergraph parser and the option parser.
    v.replace('\\', "\\\\").replace('\'', "\\'").replace(':', "\\:");
    v.replace('\\', "\\\\").replace('\'', "\\'").replace(',', "\\,").replace(';', "\\;").replace('[', "\\[").replace(']', "\\]");
    return v;
}
QString named(const QString &name, const QList<QPair<QString, QString>> &pairs) {
    QStringList a; for (const auto &v : pairs) if (!v.second.isEmpty()) a << v.first + '=' + v.second;
    return a.isEmpty() ? name : name + '=' + a.join(':');
}
double timeSeconds(const QString &v) {
    if (v.isEmpty()) return 0;
    double result = 0; const auto parts = v.split(':');
    if (parts.size() > 3) throw Error(QStringLiteral("时间格式错误：") + v);
    for (const auto &part : parts) { bool ok; double n = part.toDouble(&ok);
        if (!ok || n < 0 || !std::isfinite(n)) throw Error(QStringLiteral("时间格式错误：") + v);
        result = result * 60 + n;
    } return result;
}
QString sorted(QMap<int, QString> values, const QJsonObject &p, int kind, const QString &in, const QString &out) {
    QStringList result;
    for (const auto &v : p.value(QStringLiteral("滤镜排序系统")).toArray()) {
        const auto item = v.toObject();
        if (item.value(QStringLiteral("是自定义滤镜")).toBool()) {
            if (item.value(QStringLiteral("滤镜目标流类型")).toInt() == kind && !s(item, QStringLiteral("自定义滤镜内容")).isEmpty())
                result << sub(s(item, QStringLiteral("自定义滤镜内容")), in, out);
        } else { int id = item.value(QStringLiteral("滤镜标识符")).toInt(); if (values.contains(id)) result << values.take(id); }
    }
    for (const auto &v : values) if (!v.isEmpty()) result << v;
    return result.join(',');
}
QString subtitleFilter(const QJsonObject &p, const QString &in, const QString &out) {
    const QString prefix = QStringLiteral("视频参数_烧录字幕_");
    auto get = [&](const QString &k) { return s(p, prefix + k); };
    auto num = [&](const QString &k) { return p.value(prefix + k).toInt(); };
    const auto custom = get(QStringLiteral("自己写滤镜取代所有设置"));
    if (!custom.isEmpty()) return sub(custom, in, out);
    if (!num(QStringLiteral("滤镜选择"))) return {};
    const bool embedded = num(QStringLiteral("字幕来源是外部文件")) == 2;
    QString file = in;
    if (!embedded) {
        if (num(QStringLiteral("字幕来源是外部文件")) != 1) throw Error(QStringLiteral("烧录字幕需要选择来源"));
        QString folder = get(QStringLiteral("外部字幕文件夹位置")); if (folder.isEmpty()) folder = QFileInfo(in).absolutePath();
        QString base = get(QStringLiteral("外部字幕文件名")); if (base.isEmpty()) base = QFileInfo(in).completeBaseName();
        QDir dir(folder); file = dir.filePath(base);
        if (!QFileInfo::exists(file)) {
            file.clear(); auto priority = p.value(prefix + QStringLiteral("字幕格式优先级")).toArray();
            if (priority.isEmpty()) priority = QJsonArray{1, 2, 3};
            const QStringList exts{"", "srt", "ass", "ssa"};
            for (const auto &v : priority) if (v.toInt() > 0 && v.toInt() < exts.size()) {
                auto candidate = dir.filePath(base + '.' + exts[v.toInt()]);
                if (QFileInfo::exists(candidate)) { file = candidate; break; }
            }
            if (file.isEmpty()) throw Error(QStringLiteral("找不到外部字幕：") + dir.filePath(base));
        }
    }
    QStringList params{"filename=" + escapeFilter(QDir::fromNativeSeparators(file))}, style;
    if (embedded) { auto stream = get(QStringLiteral("指定内嵌的流")); if (stream.isEmpty()) throw Error(QStringLiteral("请选择内嵌字幕流序号")); params << "stream_index=" + stream; }
    if (!get(QStringLiteral("字体文件夹")).isEmpty()) params << "fontsdir=" + escapeFilter(get(QStringLiteral("字体文件夹")));
    const QList<QPair<QString, QString>> keys{{"FontName", QStringLiteral("基本样式_名称")}, {"FontSize", QStringLiteral("基本样式_大小")},
        {"Outline", QStringLiteral("描边宽度")}, {"Shadow", QStringLiteral("阴影距离")}, {"MarginV", QStringLiteral("垂直边距")},
        {"MarginL", QStringLiteral("左边距")}, {"MarginR", QStringLiteral("右边距")}, {"Spacing", QStringLiteral("字距")}, {"LineSpacing", QStringLiteral("行距")}};
    for (const auto &key : keys) if (!get(key.second).isEmpty() && get(key.second) != "0") style << key.first + '=' + get(key.second);
    for (const auto &key : QList<QPair<QString, QString>>{{"Bold", QStringLiteral("粗体")}, {"Italic", QStringLiteral("斜体")},
        {"Underline", QStringLiteral("下划线")}, {"StrikeOut", QStringLiteral("删除线")}})
        if (p.value(prefix + QStringLiteral("基本样式_") + key.second).toBool()) style << key.first + "=-1";
    if (num(QStringLiteral("边框样式"))) style << "BorderStyle=" + QString::number(num(QStringLiteral("边框样式")) == 2 ? 3 : 1);
    if (num(QStringLiteral("对齐方位"))) style << "Alignment=" + get(QStringLiteral("对齐方位"));
    for (const auto &key : QList<QPair<QString, QString>>{{"PrimaryColour", QStringLiteral("主要颜色")}, {"SecondaryColour", QStringLiteral("次要颜色")},
        {"OutlineColour", QStringLiteral("描边颜色")}, {"BackColour", QStringLiteral("背景颜色")}}) {
        const auto c = p.value(prefix + key.second).toObject(); if (!c.value(QStringLiteral("已设置")).toBool()) continue;
        const quint32 abgr = (quint32(255 - c.value("A").toInt(255)) << 24) | (quint32(c.value("B").toInt()) << 16) |
            (quint32(c.value("G").toInt()) << 8) | quint32(c.value("R").toInt());
        style << key.first + "=&H" + QString::number(abgr, 16).rightJustified(8, '0').toUpper();
    }
    if (!get(QStringLiteral("补充样式")).isEmpty()) style << get(QStringLiteral("补充样式"));
    if (!style.isEmpty()) params << "force_style=" + escapeFilter(style.join(','));
    const bool ass = num(QStringLiteral("滤镜选择")) == 2 && !embedded && style.isEmpty() && (file.endsWith(".ass") || file.endsWith(".ssa"));
    return (ass ? "ass=" : "subtitles=") + params.join(':');
}
QString filters(const QJsonObject &p, int kind, const QString &in, const QString &out) {
    QMap<int, QString> f;
    const QString pre = kind == 1 ? QStringLiteral("视频参数_") : QStringLiteral("音频参数_");
    auto get = [&](const QString &k) { return s(p, pre + k); };
    auto n = [&](const QString &k) { return p.value(pre + k).toInt(); };
    auto b = [&](const QString &k) { return p.value(pre + k).toBool(); };
    auto effect = [&](int id, const QString &group, const QString &name, const QStringList &names) {
        QList<QPair<QString, QString>> a; for (int i = 0; i < names.size(); ++i) a << qMakePair(names[i], get(group + QStringLiteral("_参数") + QString::number(i + 1)));
        f[id] = named(name, a);
    };
    if (kind == 2) {
        QList<QPair<QString, QString>> a;
        for (const auto &k : QList<QPair<QString, QString>>{{"I", QStringLiteral("目标响度")}, {"LRA", QStringLiteral("动态范围")}, {"TP", QStringLiteral("峰值电平")}})
            if (b(QStringLiteral("响度标准化_启用调整") + k.second)) a << qMakePair(k.first, get(QStringLiteral("响度标准化_") + k.second));
        if (!a.isEmpty()) f[51] = named("loudnorm", a);
        if (!get(QStringLiteral("位深度")).isEmpty()) f[52] = "aformat=sample_fmts=" + get(QStringLiteral("位深度"));
        if (!get(QStringLiteral("采样率")).isEmpty()) f[53] = "aresample=" + get(QStringLiteral("采样率"));
        if (!s(p, QStringLiteral("自定义参数_音频滤镜")).isEmpty()) f[102] = sub(s(p, QStringLiteral("自定义参数_音频滤镜")), in, out);
        return sorted(f, p, kind, in, out);
    }
    if (!get(QStringLiteral("分辨率_裁剪滤镜参数")).isEmpty()) f[1] = "crop=" + get(QStringLiteral("分辨率_裁剪滤镜参数"));
    QString dims = get(QStringLiteral("分辨率")); dims.replace('x', ':').replace('*', ':');
    const auto w = get(QStringLiteral("分辨率自动计算_宽度")), h = get(QStringLiteral("分辨率自动计算_高度"));
    if (!dims.isEmpty() || !w.isEmpty() || !h.isEmpty()) {
        if (dims.isEmpty()) dims = (w.isEmpty() ? "-2" : w) + ':' + (h.isEmpty() ? "-2" : h);
        auto name = get(QStringLiteral("分辨率自动计算_缩放滤镜")); if (name.isEmpty()) name = "scale";
        f[2] = name + '=' + dims;
        if (!get(QStringLiteral("分辨率自动计算_缩放算法")).isEmpty()) f[2] += ":flags=" + get(QStringLiteral("分辨率自动计算_缩放算法"));
    }
    QList<QPair<QString, QString>> a;
    for (const auto &k : QStringList{"max", "keep", "hi", "lo", "frac"}) if (!get(QStringLiteral("抽帧_") + k).isEmpty()) a << qMakePair(k, get(QStringLiteral("抽帧_") + k));
    if (!a.isEmpty()) f[3] = named("mpdecimate", a);
    if (!get(QStringLiteral("插帧_目标帧率")).isEmpty()) f[4] = named("minterpolate", {{"fps", get(QStringLiteral("插帧_目标帧率"))},
        {"mi_mode", get(QStringLiteral("插帧_插帧模式"))}, {"me_mode", get(QStringLiteral("插帧_运动估计模式"))}, {"me", get(QStringLiteral("插帧_运动估计算法"))},
        {"mc_mode", get(QStringLiteral("插帧_运动补偿模式"))}, {"vsbmc", b(QStringLiteral("插帧_可变块大小的运动补偿")) ? "1" : "0"},
        {"mb_size", get(QStringLiteral("插帧_块大小"))}, {"search_param", get(QStringLiteral("插帧_搜索范围"))}, {"scd_threshold", get(QStringLiteral("插帧_场景变化检测强度"))}});
    if (!get(QStringLiteral("动态模糊_连续混合帧数")).isEmpty()) f[5] = named("tmix", {{"frames", get(QStringLiteral("动态模糊_连续混合帧数"))},
        {"weights", get(QStringLiteral("动态模糊_每帧权重"))}, {"scale", get(QStringLiteral("动态模糊_输出缩放系数"))}, {"planes", get(QStringLiteral("动态模糊_处理颜色平面"))}});
    auto supersample = [&](const QJsonObject &v) { return named("libplacebo", {{"w", s(v, QStringLiteral("目标宽度"))}, {"h", s(v, QStringLiteral("目标高度"))},
        {"upscaler", s(v, QStringLiteral("上采样算法"))}, {"downscaler", s(v, QStringLiteral("下采样算法"))}, {"antiringing", s(v, QStringLiteral("抗振铃强度"))},
        {"custom_shader_path", s(v, QStringLiteral("着色器文件路径")).isEmpty() ? QString() : escapeFilter(s(v, QStringLiteral("着色器文件路径")))}}); };
    QStringList supers;
    for (const auto &v : p.value(QStringLiteral("视频参数_超分_滤镜叠加策略组")).toArray()) supers << supersample(v.toObject());
    auto direct = p.value(QStringLiteral("视频参数_超分_直接面板")).toObject();
    if (supers.isEmpty()) for (auto it = direct.begin(); it != direct.end(); ++it) if (!it.value().toString().isEmpty()) { supers << supersample(direct); break; }
    if (!supers.isEmpty()) f[6] = supers.join(',');
    switch (n(QStringLiteral("降噪_方式"))) {
    case 1: effect(7, QStringLiteral("降噪"), "hqdn3d", {"luma_spatial", "chroma_spatial", "luma_tmp", "chroma_tmp"}); break;
    case 2: effect(7, QStringLiteral("降噪"), "nlmeans", {"s", "p", "r", "pc"}); break;
    case 3: effect(7, QStringLiteral("降噪"), "atadenoise", {"0a", "0b", "1a", "1b"}); break;
    case 4: effect(7, QStringLiteral("降噪"), "bm3d", {"sigma", "block", "bstep", "group"}); break;
    case 5: effect(7, QStringLiteral("降噪"), "bilateral_cuda", {"sigmaS", "sigmaR", "window_size"}); break;
    }
    if (n(QStringLiteral("锐化_方式")) == 1) effect(8, QStringLiteral("锐化"), "cas", {"strength"});
    if (n(QStringLiteral("锐化_方式")) == 2) effect(8, QStringLiteral("锐化"), "unsharp", {"luma_msize_x", "luma_msize_y", "luma_amount"});
    int grain = n(QStringLiteral("胶片颗粒_方式"));
    if (grain == 1 || grain == 3) f[9] = named("noise", {{"alls", get(QStringLiteral("胶片颗粒_参数1"))}, {"allf", grain == 1 ? "t+u" : "t+a+u"}, {"all_seed", get(QStringLiteral("胶片颗粒_参数2"))}});
    if (grain == 2) f[9] = named("noise", {{"c0s", get(QStringLiteral("胶片颗粒_参数1"))}, {"c0f", "t+u"}, {"c1s", get(QStringLiteral("胶片颗粒_参数2"))},
        {"c1f", "t+u"}, {"c2s", get(QStringLiteral("胶片颗粒_参数2"))}, {"c2f", "t+u"}, {"all_seed", get(QStringLiteral("胶片颗粒_参数3"))}});
    if (grain == 4) f[9] = "libplacebo=apply_filmgrain=true";
    int deband = n(QStringLiteral("平滑断层_方式"));
    if (deband == 1 || deband == 2) f[10] = named("deband", {{"1thr", get(QStringLiteral("平滑断层_参数1"))}, {"2thr", get(QStringLiteral("平滑断层_参数1"))},
        {"3thr", get(QStringLiteral("平滑断层_参数1"))}, {"range", get(QStringLiteral("平滑断层_参数2"))}, {"direction", get(QStringLiteral("平滑断层_参数3"))}, {"blur", "1"}, {"coupling", get(QStringLiteral("平滑断层_参数4"))}});
    if (deband == 3) effect(10, QStringLiteral("平滑断层"), "gradfun", {"strength", "radius"});
    if (deband == 4) f[10] = named("libplacebo", {{"deband", "true"}, {"deband_iterations", get(QStringLiteral("平滑断层_参数1"))},
        {"deband_threshold", get(QStringLiteral("平滑断层_参数2"))}, {"deband_radius", get(QStringLiteral("平滑断层_参数3"))}, {"deband_grain", get(QStringLiteral("平滑断层_参数4"))}});
    const QStringList scans{"", "yadif=mode=send_frame:parity=auto:deint=all", "yadif=mode=send_frame:parity=tff:deint=all", "yadif=mode=send_frame:parity=bff:deint=all",
        "tinterlace=mode=interleave_top", "tinterlace=mode=interleave_bottom", "fieldmatch,yadif=deint=interlaced,decimate", "yadif=mode=send_field:parity=auto:deint=all",
        "pullup=jl=1:jr=1,fps=25", "yadif=mode=send_frame:parity=auto:deint=all", "yadif=mode=send_field:parity=auto:deint=all",
        "bwdif=mode=send_frame:parity=auto:deint=all", "bwdif=mode=send_field:parity=auto:deint=all", "yadif_cuda=mode=send_frame:parity=auto:deint=all", "bwdif_cuda=mode=send_frame:parity=auto:deint=all"};
    if (n(QStringLiteral("处理扫描方式")) > 0 && n(QStringLiteral("处理扫描方式")) < scans.size()) f[11] = scans[n(QStringLiteral("处理扫描方式"))];
    QStringList flip; int angle = n(QStringLiteral("画面翻转_角度翻转"));
    if (angle > 0 && angle < 7) for (int i = 0; i < (angle - 1) % 3 + 1; ++i) flip << (angle < 4 ? "transpose=1" : "transpose=2");
    if (n(QStringLiteral("画面翻转_镜像翻转")) == 1) flip << "hflip";
    if (n(QStringLiteral("画面翻转_镜像翻转")) == 2) flip << "vflip";
    if (!flip.isEmpty()) f[12] = flip.join(',');
    auto subtitle = subtitleFilter(p, in, out); if (!subtitle.isEmpty()) f[13] = subtitle;
    if (!get(QStringLiteral("色彩管理_像素格式预先转换")).isEmpty()) f[16] = "format=" + get(QStringLiteral("色彩管理_像素格式预先转换"));
    if (get(QStringLiteral("色彩管理_处理方式")) != QStringLiteral("仅写入元数据")) {
        auto name = get(QStringLiteral("色彩管理_滤镜选择")); if (name.isEmpty()) name = "colorspace";
        a.clear(); const QStringList labels{QStringLiteral("矩阵系数"), QStringLiteral("色域"), QStringLiteral("传输特性"), QStringLiteral("范围")};
        const QStringList names = name == "libplacebo" ? QStringList{"colorspace", "color_primaries", "color_trc", "range"} : name == "colorspace" ? QStringList{"space", "primaries", "trc", "range"} : QStringList{"matrix", "primaries", "transfer", "range"};
        for (int i = 0; i < labels.size(); ++i) { auto v = get(QStringLiteral("色彩管理_") + labels[i]); if (!v.isEmpty() && v != "auto") a << qMakePair(names[i], v.section(' ', 0, 0)); }
        if (name == "libplacebo" && !get(QStringLiteral("色彩管理_色调映射算法")).isEmpty()) a << qMakePair(QString("tonemapping"), get(QStringLiteral("色彩管理_色调映射算法")));
        if (!a.isEmpty()) f[14] = named(name, a);
    }
    a.clear();
    for (const auto &k : QList<QPair<QString, QString>>{{"brightness", QStringLiteral("亮度")}, {"contrast", QStringLiteral("对比度")}, {"saturation", QStringLiteral("饱和度")}, {"gamma", QStringLiteral("伽马")}})
        if (b(QStringLiteral("色彩管理_启用调整") + k.second)) a << qMakePair(k.first, get(QStringLiteral("色彩管理_") + k.second));
    if (!a.isEmpty()) f[15] = named("eq", a);
    if (!s(p, QStringLiteral("自定义参数_视频滤镜")).isEmpty()) f[101] = sub(s(p, QStringLiteral("自定义参数_视频滤镜")), in, out);
    return sorted(f, p, kind, in, out);
}
}
QJsonArray schema() { return resource("schema.json").array(); }
QJsonArray builtins() { return resource("builtins.json").array(); }
QJsonObject defaults() { return resource("defaults.json").object(); }
QJsonObject loadPreset(const QString &path) {
    QFile f(path); if (!f.open(QIODevice::ReadOnly)) throw Error(f.errorString());
    QJsonParseError e; auto d = QJsonDocument::fromJson(f.readAll(), &e);
    if (!d.isObject() || e.error) throw Error(QStringLiteral("预设 JSON 错误：") + e.errorString());
    if (d.object().value(QStringLiteral("预设文件版本")).toInt(6) != 6) throw Error(QStringLiteral("当前支持版本 6 的 .3fui 预设"));
    auto p = defaults();
    const auto object = d.object(); for (auto it = object.begin(); it != object.end(); ++it) p[it.key()] = it.value(); return p;
}
void saveJson(const QString &path, const QJsonObject &o) {
    QSaveFile f(path); if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(o).toJson()) < 0 || !f.commit()) throw Error(f.errorString());
}
QStringList tokenize(const QString &text) {
    QStringList result; QString word; QChar quote; bool active = false;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar ch = text[i];
        if (ch == '\\' && i + 1 < text.size()) { QChar next = text[i + 1];
            if ((quote != '\'' && (next == '"' || next == '\\')) || (quote.isNull() && (next.isSpace() || next == '\''))) { word += next; ++i; active = true; continue; }
        }
        if (ch == '"' || ch == '\'') { if (quote == ch) quote = QChar(); else if (quote.isNull()) quote = ch; else word += ch; active = true; }
        else if (ch.isSpace() && quote.isNull()) { if (active) { result << word; word.clear(); active = false; } }
        else { word += ch; active = true; }
    }
    if (!quote.isNull()) throw Error(QStringLiteral("自定义参数有未闭合的引号"));
    if (active) result << word;
    return result;
}
QString displayCommand(const Command &c) {
    QStringList words{c.program}; words += c.args;
    for (auto &w : words) if (w.isEmpty() || w.contains(QRegularExpression("[\\s\"'<>;]"))) { w.replace('\\', "\\\\").replace('"', "\\\""); w = '"' + w + '"'; }
    return words.join(' ');
}
QString resolveTool(const QString &name, const QString &configured) {
    if (!configured.trimmed().isEmpty()) { auto v = QStandardPaths::findExecutable(configured); if (!v.isEmpty()) return v; throw Error(QStringLiteral("找不到程序：") + configured); }
    auto beside = QDir(QCoreApplication::applicationDirPath()).filePath(name);
#ifdef Q_OS_WIN
    beside += ".exe";
#endif
    if (QFileInfo(beside).isExecutable()) return beside;
    auto v = QStandardPaths::findExecutable(name); if (v.isEmpty()) throw Error(QStringLiteral("找不到 ") + name + QStringLiteral("；请安装或在设置中指定路径")); return v;
}
double probeDuration(const QString &input, const QString &ffprobe) {
    QProcess p; p.start(ffprobe, {"-v", "error", "-show_entries", "format=duration", "-of", "json", input});
    if (!p.waitForFinished(15000)) { p.kill(); p.waitForFinished(); return 0; }
    if (p.exitCode()) return 0;
    return QJsonDocument::fromJson(p.readAllStandardOutput()).object().value("format").toObject().value("duration").toString().toDouble();
}
Plan compilePreset(const QJsonObject &p, const QString &in, const QString &out, const QString &ffmpeg, double duration, const QString &passLog) {
    auto get = [&](const QString &k) { return s(p, k); }; auto n = [&](const QString &k) { return p.value(k).toInt(); }; auto b = [&](const QString &k) { return p.value(k).toBool(); };
    if (in.isEmpty() || out.isEmpty()) throw Error(QStringLiteral("输入和输出路径不能为空"));
#ifndef Q_OS_WIN
    if (b(QStringLiteral("输出命名_保留创建时间"))) throw Error(QStringLiteral("Linux 不支持可靠修改文件创建时间；可保留修改时间和访问时间"));
#endif
    if (QFileInfo(in).absoluteFilePath() == QFileInfo(out).absoluteFilePath() || (!QFileInfo(in).canonicalFilePath().isEmpty() && QFileInfo(in).canonicalFilePath() == QFileInfo(out).canonicalFilePath())) throw Error(QStringLiteral("输出不能覆盖输入文件"));
    Plan plan; plan.duration = duration;
    QStringList base{"-hide_banner", "-nostdin", "-n", "-progress", "pipe:1", "-nostats"};
    if (!get(QStringLiteral("自定义参数_完全自己写")).isEmpty()) { raw(base, get(QStringLiteral("自定义参数_完全自己写")), in, out); base.removeAll("-y"); base.removeAll("-n"); base.prepend("-n"); plan.steps << Command{ffmpeg, base, QStringLiteral("自定义转码")}; return plan; }
    if (!get(QStringLiteral("视频参数_NV_FRUC_目标帧率")).isEmpty() || b(QStringLiteral("视频参数_视频帧服务器_使用VapourSynth"))) throw Error(QStringLiteral("NV_FRUC / VapourSynth 专用管线尚未移植，请使用自定义参数或 FFmpeg 原生滤镜"));
    QString codec = get(QStringLiteral("视频参数_编码器_具体编码")), acodec = get(QStringLiteral("音频参数_编码器_代号"));
    if (codec == QStringLiteral("复制流")) codec = "copy";
    if (codec == QStringLiteral("禁用")) codec = "-vn";
    const auto codecs = resource("audio-codecs.json").object(); const auto audio = codecs.value(acodec).toObject();
    if (!audio.isEmpty()) acodec = audio.value("codec").toString();
    const bool noVideo = codec == "-vn" || codec == "disable", noAudio = acodec == "-an" || acodec == "disable";
    auto vf = noVideo ? QString() : filters(p, 1, in, out);
    auto af = noAudio ? QString() : filters(p, 2, in, out);
    int trim = n(QStringLiteral("剪辑区间_方法")); double start = timeSeconds(get(QStringLiteral("剪辑区间_入点"))), end = timeSeconds(get(QStringLiteral("剪辑区间_出点")));
    if (trim == 5) { if (duration <= 0) throw Error(QStringLiteral("掐头去尾需要可读取的媒体时长")); end = duration - end; }
    if (trim == 6) throw Error(QStringLiteral("剔除中间的多段滤镜图尚未移植，请使用完全自己写模式"));
    if (trim && end > 0 && end <= start) throw Error(QStringLiteral("出点必须大于入点"));
    if (trim == 4 || trim == 5) { auto range = "start=" + QString::number(start) + (end > 0 ? ":end=" + QString::number(end) : QString()); vf = "trim=" + range + ",setpts=PTS-STARTPTS" + (vf.isEmpty() ? QString() : ',' + vf); af = "atrim=" + range + ",asetpts=PTS-STARTPTS" + (af.isEmpty() ? QString() : ',' + af); }
    if (trim && duration > 0) plan.duration = (end > 0 ? end : duration) - start;
    if ((codec == "copy" && !vf.isEmpty()) || (acodec == "copy" && !af.isEmpty())) throw Error(QStringLiteral("复制流不能同时应用滤镜"));
    int quality = n(QStringLiteral("视频参数_比特率_控制方式")); bool twoPass = quality == 6;
    if (twoPass && (noVideo || codec == "copy" || get(QStringLiteral("视频参数_比特率_基础")).isEmpty() || passLog.isEmpty())) throw Error(QStringLiteral("二次编码需要视频编码器、基础码率和独立日志路径"));
    if (twoPass && (codec.contains("nvenc") || codec.contains("qsv") || codec.contains("amf"))) throw Error(QStringLiteral("硬件编码器请使用自身的码率控制模式"));
    for (int pass = twoPass ? 1 : 0; pass <= (twoPass ? 2 : 0); ++pass) {
        auto a = base;
        raw(a, get(QStringLiteral("自定义参数_开头参数")), in, out);
        opt(a, "-hwaccel", get(QStringLiteral("解码参数_解码器"))); opt(a, "-threads", get(QStringLiteral("解码参数_CPU解码线程数")));
        opt(a, "-hwaccel_output_format", get(QStringLiteral("解码参数_解码数据格式")));
        auto hwName = get(QStringLiteral("解码参数_指定硬件的参数名")); if (!hwName.isEmpty()) opt(a, '-' + hwName.remove(QRegularExpression("^-+")), get(QStringLiteral("解码参数_指定硬件的参数")));
        const double decodeBack = trim == 3 ? timeSeconds(get(QStringLiteral("剪辑区间_向前解码多久秒"))) : 0;
        const double inputSeek = decodeBack > 0 ? std::max(0.0, start - decodeBack) : start;
        if (trim == 1 || trim == 3) opt(a, "-ss", QString::number(inputSeek));
        auto actualIn = in;
        if (b(QStringLiteral("视频参数_视频帧服务器_使用AviSynth"))) {
#ifdef Q_OS_WIN
            actualIn = get(QStringLiteral("视频参数_视频帧服务器_avs脚本文件")); if (actualIn.isEmpty()) throw Error(QStringLiteral("请选择 AviSynth 脚本"));
#else
            throw Error(QStringLiteral("Linux 不提供原版 AviSynth Windows 后端"));
#endif
        }
        a << "-i" << actualIn; raw(a, get(QStringLiteral("自定义参数_之前参数")), in, out);
        QStringList late; int nextInput = 1;
        if (n(QStringLiteral("章节_来源")) && !get(QStringLiteral("章节_文件路径")).isEmpty()) { if (n(QStringLiteral("章节_来源")) == 1) a << "-f" << "ffmetadata"; a << "-i" << get(QStringLiteral("章节_文件路径")); late << "-map_chapters" << QString::number(nextInput++); }
        auto map = [&](const QString &type, const QString &label, bool disabled) {
            if (disabled) return;
            auto selected = p.value(QStringLiteral("流控制_将") + label + QStringLiteral("参数应用于指定流")).toArray();
            if (b(QStringLiteral("流控制_启用保留其他") + label + QStringLiteral("流")) && !selected.isEmpty()) throw Error(QStringLiteral("选择部分流并保留其他流尚未移植，请使用显式 -map 参数"));
            if (selected.isEmpty()) a << "-map" << ("0:" + type + "?"); else for (const auto &v : selected) { auto spec = v.toString(); if (!spec.startsWith("0:")) spec = "0:" + spec; a << "-map" << spec; }
        };
        map("v", QStringLiteral("视频"), noVideo); if (pass != 1) map("a", QStringLiteral("音频"), noAudio);
        if (trim == 2) opt(a, "-ss", QString::number(start));
        if (trim == 3 && decodeBack > 0) opt(a, "-ss", QString::number(start - inputSeek));
        if (trim >= 1 && trim <= 3 && end > 0) opt(a, "-t", QString::number(end - start));
        if (noVideo) a << "-vn"; else {
            opt(a, "-c:v", codec);
            if (codec != "copy") {
                opt(a, "-vf", vf);
                opt(a, codec == "libaom-av1" || codec == "libvpx-vp9" ? "-cpu-used" : codec == "libjxl" ? "-effort" : "-preset", get(QStringLiteral("视频参数_编码器_编码预设")));
                const QList<QPair<QString, QString>> videoKeys{{"-profile:v", QStringLiteral("编码器_配置文件")}, {"-tune", QStringLiteral("编码器_场景优化")}, {"-gpu", QStringLiteral("编码器_gpu")},
                    {"-threads:v", QStringLiteral("编码器_threads")}, {"-pix_fmt", QStringLiteral("色彩管理_像素格式")}, {"-r", QStringLiteral("帧速率")},
                    {"-b:v", QStringLiteral("比特率_基础")}, {"-minrate", QStringLiteral("比特率_最低值")}, {"-maxrate", QStringLiteral("比特率_最高值")}, {"-bufsize", QStringLiteral("比特率_缓冲区")}};
                for (const auto &k : videoKeys) opt(a, k.first, get(QStringLiteral("视频参数_") + k.second));
                opt(a, codec == "libjxl" ? "-distance" : "-q:v", get(QStringLiteral("视频参数_编码器_图片编码器质量值")));
                auto fps = get(QStringLiteral("视频参数_帧速率模式")); if (fps.contains(' ')) fps = fps.section(' ', -1); if (fps == "cfr" || fps == "vfr") opt(a, "-fps_mode", fps);
                auto qname = get(QStringLiteral("视频参数_质量控制_参数名")); if (qname.isEmpty()) qname = quality == 1 ? "crf" : quality == 4 ? "qp" : "";
                if (!twoPass && !qname.isEmpty()) opt(a, '-' + qname.remove(QRegularExpression("^-+")), get(QStringLiteral("视频参数_质量控制_值")));
                auto advanced = get(QStringLiteral("视频参数_质量控制_进阶参数集"));
                if (codec.contains("nvenc") && !tokenize(advanced).contains("-rc")) { if (quality == 2) opt(a, "-rc", "vbr"); if (quality == 4) opt(a, "-rc", "constqp"); if (quality == 5) opt(a, "-rc", "cbr"); }
                raw(a, advanced, in, out);
            }
        }
        if (pass == 1 || noAudio) a << "-an"; else { opt(a, "-c:a", acodec); if (acodec != "copy") {
            opt(a, "-af", af); opt(a, "-b:a", get(QStringLiteral("音频参数_比特率"))); opt(a, "-ac", get(QStringLiteral("音频参数_声道数")));
            for (const auto &v : audio.value("args").toArray()) a << v.toString();
            for (const auto &suffix : QStringList{"", "2"}) { auto name = get(QStringLiteral("音频参数_质量参数名") + suffix); if (!name.isEmpty()) opt(a, '-' + name.remove(QRegularExpression("^-+")), get(QStringLiteral("音频参数_质量值") + suffix)); }
        }}
        int smode = n(QStringLiteral("流控制_如何操作指定的字幕")); if (pass != 1 && (smode || b(QStringLiteral("流控制_启用保留其他字幕流")))) {
            map("s", QStringLiteral("字幕"), false); a << "-c:s" << QStringList{"copy", "copy", "mov_text", "srt", "ass", "ssa"}.value(smode, "copy"); }
        for (const auto &ext : QStringList{"SRT", "ASS", "SSA"}) if (b(QStringLiteral("流控制_自动混流") + ext)) throw Error(QStringLiteral("旁挂字幕自动混流尚未移植，请使用混流工具添加字幕"));
        int metadata = n(QStringLiteral("流控制_元数据选项")); if (metadata) a << "-map_metadata" << (metadata == 2 ? "-1" : "0");
        int chapterMode = n(QStringLiteral("流控制_章节选项")); if (!n(QStringLiteral("章节_来源")) && chapterMode) a << "-map_chapters" << (chapterMode == 2 ? "-1" : "0");
        if (n(QStringLiteral("流控制_附件选项")) == 1) a << "-map" << "0:t?" << "-c:t" << "copy";
        for (const auto &v : p.value(QStringLiteral("元数据_要写入的信息")).toArray()) { auto m = v.toObject(); if (!s(m, QStringLiteral("字段")).isEmpty()) a << "-metadata" << (s(m, QStringLiteral("字段")) + '=' + s(m, QStringLiteral("值"))); }
        for (const auto &v : p.value(QStringLiteral("附件_要写入的附件")).toArray()) { auto at = v.toObject(); int type = at.value(QStringLiteral("类型")).toInt(); if (!type) continue;
            if (type <= 3) throw Error(QStringLiteral("封面图的流序号处理尚未移植，请使用自定义流映射"));
            a << "-attach" << s(at, QStringLiteral("文件路径")) << "-metadata:s:t" << (type == 4 ? "mimetype=application/x-truetype-font" : "mimetype=text/plain"); }
        for (const auto &k : QList<QPair<QString, QString>>{{"-colorspace", QStringLiteral("矩阵系数")}, {"-color_primaries", QStringLiteral("色域")}, {"-color_trc", QStringLiteral("传输特性")}, {"-color_range", QStringLiteral("范围")}}) {
            auto v = get(QStringLiteral("视频参数_色彩管理_") + k.second).section(' ', 0, 0); if (v != "auto") opt(a, k.first, v); }
        if (pass != 1) a += late;
        raw(a, get(QStringLiteral("自定义参数_视频参数")), in, out); if (pass != 1) raw(a, get(QStringLiteral("自定义参数_音频参数")), in, out);
        raw(a, get(QStringLiteral("自定义参数_之后参数")), in, out);
        if (twoPass) a << "-pass" << QString::number(pass) << "-passlogfile" << passLog;
        int discard = n(QStringLiteral("输出_输出文件参数使用方法"));
        if (pass == 1 || discard == 2) {
            a << "-f" << "null";
            a << "-";
        } else if (!discard) a << out;
        raw(a, get(QStringLiteral("自定义参数_最后参数")), in, out);
        a.removeAll("-y"); a.removeAll("-n"); a.prepend("-n");
        plan.steps << Command{ffmpeg, a, twoPass ? QStringLiteral("二次编码第 %1 遍").arg(pass) : QStringLiteral("转码")};
    } return plan;
}
QString outputPath(const QJsonObject &p, const QString &in, const QString &directory, const QStringList &reserved) {
    QFileInfo file(in); auto ext = s(p, QStringLiteral("输出容器")); if (ext.isEmpty()) ext = ".mp4"; if (!ext.startsWith('.')) ext.prepend('.');
    if (ext.contains('/') || ext.contains('\\')) throw Error(QStringLiteral("输出容器只能是扩展名"));
    auto base = s(p, QStringLiteral("输出命名_替代文本")); if (base.isEmpty()) base = file.completeBaseName();
    base = s(p, QStringLiteral("输出命名_开头文本")) + base + s(p, QStringLiteral("输出命名_结尾文本"));
    if (base.contains('/') || base.contains('\\')) throw Error(QStringLiteral("输出名称不能包含路径分隔符"));
    int naming = p.value(QStringLiteral("输出_自动命名选项")).toInt(1);
    if (naming == 1) { base.remove(QRegularExpression("_\\d{4}\\.\\d{2}\\.\\d{2}-\\d{2}\\.\\d{2}\\.\\d{2}$")); base += '_' + QDateTime::currentDateTime().toString("yyyy.MM.dd-HH.mm.ss"); }
    if (naming == 3) base += "_3FUI";
    if (naming == 4) base += '_' + s(p, QStringLiteral("视频参数_编码器_具体编码")) + '_' + s(p, QStringLiteral("视频参数_质量控制_值"));
    if (naming >= 5 && naming <= 10) {
        const int kind = (naming - 5) % 3;
        const QString alphabet = kind == 0 ? "0123456789" : kind == 1 ? "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ" : "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
        QString random; for (int i = 0; i < (naming >= 8 ? 16 : 8); ++i) random += alphabet[QRandomGenerator::global()->bounded(static_cast<int>(alphabet.size()))]; base += '_' + random;
    }
    QString folder = directory; if (folder.isEmpty()) folder = s(p, QStringLiteral("输出目录")); if (folder.isEmpty()) folder = s(p, QStringLiteral("输出位置"));
    if (folder.isEmpty()) folder = file.absolutePath();
    else if (!s(p, QStringLiteral("输出位置_保留子文件夹结构起始点")).isEmpty()) {
        const auto relative = QDir(s(p, QStringLiteral("输出位置_保留子文件夹结构起始点"))).relativeFilePath(file.absolutePath());
        if (!QDir::isAbsolutePath(relative) && relative != ".." && !relative.startsWith("../")) folder = QDir(folder).filePath(relative);
    }
    QDir dir(folder); int i = 1;
    auto candidate = dir.filePath(base + (naming == 11 ? "01" : naming == 12 ? "001" : "") + ext);
    while (QFileInfo::exists(candidate) || reserved.contains(candidate) || QFileInfo(candidate).absoluteFilePath() == file.absoluteFilePath())
        candidate = dir.filePath(base + (naming == 11 || naming == 12 ? QString() : "~") + QString::number(naming == 11 || naming == 12 ? ++i : i++).rightJustified(naming == 11 ? 2 : naming == 12 ? 3 : 1, '0') + ext);
    return QFileInfo(candidate).absoluteFilePath();
}
}
