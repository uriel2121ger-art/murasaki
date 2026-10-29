// The daemon sizes the copy of a stage and the overlay chooses what plays: they must agree, or a stage plays the original (or a blurry copy) for ever. This test builds the
// copies with the real daemon service (a fake ffmpeg that writes the output), hands the daemon's own item JSON to the real overlay controller, and checks that the overlay
// plays the daemon's copy, for desks that differ in scale, primary, rotation, fit and cap.

#include "animfixtures.h"
#include "assignment.h"
#include "localpets.h"
#include "murasakiservice.h"
#include "overlaycontroller.h"
#include "petstore.h"
#include "settings.h"
#include "stagegeometry.h"
#include "transcodebuckets.h"
#include "testutil.h"
#include "transcodemanager.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <QScopeGuard>
#include <QTimer>

#include <map>
#include <vector>

using namespace murasaki;
using namespace Qt::StringLiterals;

namespace {

// The overlay's view of the daemon: the service itself, called directly.
class ServiceBackend : public DBusBackend
{
public:
    explicit ServiceBackend(MurasakiService *service) : m_service(service) {}
    void start() override {}
    void call(const QString &method, const QVariantList &args, Callback callback) override
    {
        Reply reply;
        reply.ok = true;
        if (method == u"ListPets"_s)
            reply.value = m_service->ListPets();
        else if (method == u"Settings"_s)
            reply.value = m_service->Settings();
        else if (method == u"GetItem"_s)
            reply.value = m_service->GetItem(args.value(0).toString());
        else if (method == u"SetOutputScales"_s)
            sentScales = args.value(0).toString();
        QTimer::singleShot(0, this, [callback = std::move(callback), reply] { callback(reply); });
    }
    void property(const QString &name, Callback callback) override
    {
        Reply reply;
        reply.ok = true;
        reply.value = name == u"ApiVersion"_s ? QVariant(17u) : QVariant(false);
        QTimer::singleShot(0, this, [callback = std::move(callback), reply] { callback(reply); });
    }
    void announce(const QString &name, const QVariantList &args = {}) { emit signalReceived(name, args); }
    QString sentScales;

private:
    MurasakiService *m_service;
};

struct Window : PetSurface {
    Window(std::vector<Window *> *registry, QString petId, QString output) : registry(registry), petId(std::move(petId)), output(std::move(output)) { registry->push_back(this); }
    ~Window() override { std::erase(*registry, this); }
    void apply(const PetView &v) override { view = v; }
    bool inputEnabled() const override { return false; }
    QRectF geometry() const override { return {}; }
    std::vector<Window *> *registry;
    QString petId;
    QString output;
    PetView view;
};

struct Output {
    QString name;
    QRect rect;
    double scale;
};

struct Scenario {
    const char *name;
    QList<Output> outputs;
    QString primary;
    // each stage: pet output ("*" or a name), mode, fit, rotation
    struct Stage {
        QString output, mode, fit;
        double rotation = 0;
    };
    QList<Stage> stages;
    int capPixels = 8294400;
};

} // namespace

class TstStageNeed : public QObject
{
    Q_OBJECT

private slots:
    void theOverlayPlaysTheCopyTheDaemonMade_data();
    void theOverlayPlaysTheCopyTheDaemonMade();

private:
    QList<Scenario> m_scenarios;
};

void TstStageNeed::theOverlayPlaysTheCopyTheDaemonMade_data()
{
    QTest::addColumn<int>("which");
    const QList<Output> mixed{{u"DP-1"_s, QRect(1920, 0, 2560, 1440), 1.0}, {u"HDMI-A-1"_s, QRect(0, 0, 1920, 1080), 2.0}};
    const QList<Output> same{{u"DP-1"_s, QRect(0, 0, 1920, 1080), 1.0}, {u"DP-2"_s, QRect(1920, 0, 1920, 1080), 1.0}};
    m_scenarios = {
        {"primary is the 4K output", mixed, u"HDMI-A-1"_s, {{u"*"_s, u"output"_s, u"contain"_s, 0}}},
        {"primary is the 1440p output", mixed, u"DP-1"_s, {{u"*"_s, u"output"_s, u"contain"_s, 0}}},
        {"a named output", mixed, u"DP-1"_s, {{u"HDMI-A-1"_s, u"output"_s, u"stretch"_s, 0}}},
        {"desktop across a mixed desk", mixed, u"DP-1"_s, {{u"*"_s, u"desktop"_s, u"cover"_s, 0}}},
        {"turned a quarter", same, u"DP-1"_s, {{u"DP-2"_s, u"output"_s, u"contain"_s, 90}}},
        {"turned at an odd angle", same, u"DP-1"_s, {{u"DP-1"_s, u"output"_s, u"contain"_s, 37}}},
        {"two stages share a small cap", same, u"DP-1"_s, {{u"DP-1"_s, u"output"_s, u"stretch"_s, 0}, {u"DP-2"_s, u"output"_s, u"stretch"_s, 0}}, 2600000},
        {"a cap that reduces a mixed desk", mixed, u"DP-1"_s, {{u"HDMI-A-1"_s, u"output"_s, u"stretch"_s, 0}}, 1500000},
    };
    for (int i = 0; i < int(m_scenarios.size()); ++i)
        QTest::newRow(m_scenarios.at(i).name) << i;
}

void TstStageNeed::theOverlayPlaysTheCopyTheDaemonMade()
{
    QFETCH(int, which);
    const Scenario &scenario = m_scenarios.at(which);

    QTemporaryDir tmp;
    const QByteArray oldPath = qgetenv("PATH");
    const auto restore = qScopeGuard([&] { qputenv("PATH", oldPath); });
    // 4000x3000, two frames at 60 fps (a heavy pet, so that the daemon makes copies).
    testutil::writeFile(tmp.filePath(u"anima/9/huge.apng"_s), animfixtures::apng(4000, 3000, {{1, 60}, {1, 60}}));
    testutil::writeFile(tmp.filePath(u"bin/ffmpeg"_s), "#!/bin/sh\nfor l; do :; done\necho fake > \"$l\"\n");
    QFile::setPermissions(tmp.filePath(u"bin/ffmpeg"_s), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    qputenv("PATH", tmp.filePath(u"bin"_s).toUtf8() + ":" + oldPath);
    Library library;
    library.setAnimaDirs({tmp.filePath(u"anima"_s)});
    library.rescan();
    AssignmentStore assignments(tmp.filePath(u"assignments.json"_s));
    SettingsStore settings(tmp.filePath(u"settings.json"_s));
    PetStore pets(tmp.filePath(u"config/pets.json"_s));
    TranscodeManager transcodes(tmp.filePath(u"cache/pets"_s));
    LocalPetStore localPets(tmp.filePath(u"config/local-pets.json"_s));
    MurasakiService service(&library, &assignments, &settings, u"ok"_s, {});
    pets.load();
    localPets.load();
    service.attachLocalPets(&localPets);
    service.attachPets(&pets, &transcodes);
    service.setTranscodeDebounceMs(50);
    QVERIFY(settings.merge({{u"stageMaxPixels"_s, scenario.capPixels}}));

    // the overlay's outputs, and what it tells the daemon about them
    ServiceBackend backend(&service);
    std::vector<Window *> windows;
    OverlayController controller(&backend, [&](const QString &output, const QString &petId) -> std::unique_ptr<PetSurface> {
        return std::make_unique<Window>(&windows, petId, output);
    });
    QStringList names;
    QHash<QString, double> scales;
    QHash<QString, QRect> rects;
    for (const Output &o : scenario.outputs) {
        names << o.name;
        scales.insert(o.name, o.scale);
        rects.insert(o.name, o.rect);
    }
    controller.setOutputs(names, scenario.primary, scales, rects);
    controller.setOutputFullscreen({}, true);
    controller.start();
    backend.availabilityChanged(true);
    QTest::qWait(100);
    QVERIFY2(!backend.sentScales.isEmpty(), "the overlay told the daemon nothing about the outputs");
    service.SetOutputScales(backend.sentScales); // (what the daemon would have heard)

    QStringList ids;
    for (const Scenario::Stage &stage : scenario.stages) {
        QJsonObject options{{u"stage"_s, QJsonObject{{u"mode"_s, stage.mode}, {u"fit"_s, stage.fit}}}, {u"rotation"_s, stage.rotation}};
        const QString id = service.AddPet(u"anima:9"_s, u"huge"_s, stage.output, QString::fromUtf8(QJsonDocument(options).toJson(QJsonDocument::Compact)));
        QVERIFY2(!id.isEmpty(), "the daemon refused the stage");
        ids << id;
    }
    // the daemon makes the copies it decided on
    QVERIFY(QTest::qWaitFor([&] {
        const QJsonObject item = QJsonDocument::fromJson(service.GetItem(u"anima:9"_s).toUtf8()).object();
        for (const QJsonValue &bucket : item.value(u"variants"_s).toArray().at(0).toObject().value(u"buckets"_s).toArray()) {
            const QString state = bucket.toObject().value(u"state"_s).toString();
            if (state == u"queued"_s || state == u"running"_s)
                return false;
        }
        return !item.value(u"variants"_s).toArray().at(0).toObject().value(u"buckets"_s).toArray().isEmpty();
    }, 15000));
    // it made one copy, of the size the shared function gives for these outputs, this primary, this cap and these stages (computed here from the scenario alone)
    QList<StageOutput> outputs;
    for (const Output &o : scenario.outputs)
        outputs.append({o.name, o.rect, o.scale, o.name == scenario.primary});
    QList<StageRequest> requests;
    for (const Scenario::Stage &stage : scenario.stages) {
        StageRequest request;
        request.mode = *stageModeFromName(stage.mode);
        request.fit = *stageFitFromName(stage.fit);
        request.output = stage.output;
        request.rotation = stage.rotation;
        request.native = QSize(4000, 3000);
        requests.append(request);
    }
    const double factor = stageResolutionFactor(stageRequestPixels(outputs, requests), scenario.capPixels);
    int expectedNeed = 0;
    for (const StageRequest &request : requests)
        expectedNeed = std::max(expectedNeed, stageRequestNeed(outputs, request, factor, 4000));
    QList<int> made;
    for (const QJsonValue &bucket : QJsonDocument::fromJson(service.GetItem(u"anima:9"_s).toUtf8()).object().value(u"variants"_s).toArray().at(0).toObject().value(u"buckets"_s).toArray()) {
        if (bucket.toObject().value(u"state"_s).toString() == u"done"_s)
            made.append(bucket.toObject().value(u"bucket"_s).toInt());
    }
    QCOMPARE(made, QList<int>{bucketForNeed(expectedNeed)});
    // and the overlay, given the daemon's own answers, plays a copy for every stage
    backend.announce(u"PetsChanged"_s);
    backend.announce(u"SettingsChanged"_s);
    backend.announce(u"TranscodeChanged"_s, {u"anima:9"_s});
    QTest::qWait(300);
    const QJsonArray described = controller.describe();
    QCOMPARE(int(described.size()), int(scenario.stages.size()));
    for (const QJsonValue &value : described) {
        const QJsonObject o = value.toObject();
        QVERIFY2(o.value(u"bucket"_s).toInt() > 0,
                 qPrintable(u"the overlay plays the original although the daemon made copies: "_s + QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact))));
        QVERIFY2(!o.value(u"degraded"_s).toArray().contains(u"stage: no copy big enough"_s), "the overlay found every copy too small");
        QVERIFY(o.value(u"file"_s).toString().endsWith(u".webm"_s));
    }
}

QTEST_MAIN(TstStageNeed)
#include "tst_stageneed.moc"
