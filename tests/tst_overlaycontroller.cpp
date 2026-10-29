// OverlayController against a scripted daemon and fake pet surfaces: what each pet shows and plays, how the
// arrange mode, the transcode state, the outputs and the session policy change it, and what the user's edits
// send back.

#include "crashledger.h"
#include "puppetnames.h"
#include "petbehaviours.h"
#include "overlaycontroller.h"
#include "petaudiocontroller.h"
#include "testutil.h"

#include <QDir>
#include <QElapsedTimer>
#include <QScopeGuard>
#include <QFile>
#include <QJsonArray>
#include <ctime>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <cmath>
#include <map>
#include <memory>

using namespace Qt::StringLiterals;
using namespace murasaki;

namespace {

QString json(const QJsonValue &value)
{
    return QString::fromUtf8(value.isArray() ? QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact)
                                             : QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
}

// The daemon as the overlay sees it.
class FakeDaemon : public DBusBackend
{
public:
    using DBusBackend::DBusBackend;

    void start() override { ++startCount; }
    void call(const QString &method, const QVariantList &args, Callback callback) override
    {
        calls.append({method, args});
        Reply reply;
        reply.ok = true;
        if (method == u"UpdatePet"_s && failUpdatePet) {
            reply.ok = false;
            reply.message = u"refused by the test"_s;
        }
        if (method == u"ListPets"_s) {
            reply.value = json(pets);
        } else if (method == u"Settings"_s) {
            reply.ok = settingsAnswer;
            reply.value = json(settings);
        } else if (method == u"GetItem"_s) {
            const auto it = items.find(args.value(0).toString());
            if (transientItemErrors > 0) {
                --transientItemErrors;
                reply.ok = false;
                reply.error = u"org.freedesktop.DBus.Error.NoReply"_s;
            } else if (it == items.end()) {
                reply.ok = false;
                reply.error = u"org.freedesktop.DBus.Error.InvalidArgs"_s;
            } else {
                reply.value = json(it->second);
            }
        }
        QTimer::singleShot(0, this, [callback = std::move(callback), reply] { callback(reply); });
    }
    void property(const QString &name, Callback callback) override
    {
        Reply reply;
        reply.ok = (name != u"PetsHidden"_s || hiddenReadable) && (name != u"PetRuntime"_s || runtimeReadable); // a daemon older than revision 12 / 18 has no such property
        reply.value = name == u"ApiVersion"_s ? QVariant(apiVersion)
            : name == u"PetsHidden"_s         ? QVariant(petsHidden)
            : name == u"PetRuntime"_s         ? QVariant(runtime)
                                              : QVariant(arrange);
        QTimer::singleShot(0, this, [callback = std::move(callback), reply] { callback(reply); });
    }

    void emitSignal(const QString &name, const QVariantList &args = {}) { emit signalReceived(name, args); }

    int callCount(const QString &method) const
    {
        int n = 0;
        for (const auto &call : calls)
            n += call.first == method;
        return n;
    }
    QVariantList lastArgs(const QString &method) const
    {
        for (int i = calls.size() - 1; i >= 0; --i) {
            if (calls.at(i).first == method)
                return calls.at(i).second;
        }
        return {};
    }

    QJsonArray pets;
    QJsonObject settings; // what Settings() answers
    bool settingsAnswer = true;
    std::map<QString, QJsonObject> items;
    uint apiVersion = 5;
    bool arrange = false;
    bool petsHidden = false;
    bool failUpdatePet = false; // the daemon refuses every UpdatePet
    bool hiddenReadable = true;
    QString runtime = u"{}"_s; // what PetRuntime says (revision 18)
    bool runtimeReadable = true;
    int startCount = 0;
    int transientItemErrors = 0; // the next GetItem calls fail like a busy daemon
    QList<QPair<QString, QVariantList>> calls;
};

struct FakeSurface : PetSurface {
    explicit FakeSurface(std::vector<FakeSurface *> *registry, QString petId, QString output)
        : registry(registry), petId(std::move(petId)), output(std::move(output))
    {
        registry->push_back(this);
        serial = ++created;
    }
    ~FakeSurface() override { std::erase(*registry, this); }
    void apply(const PetView &v) override
    {
        view = v;
        ++applied;
    }
    bool inputEnabled() const override { return view.arrange; }
    QStringList degraded() const override
    {
        return view.placement.keepAbove ? QStringList() : QStringList{u"keep above off: always below windows"_s};
    }
    QRectF geometry() const override { return QRectF(view.placement.x, view.placement.y, view.placement.width, 10); }
    bool wantsGlobalPointer() const override { return wantsPointer; }
    void globalPointer(QPointF position) override { pointers.append(position); }
    PetInputStatus inputStatus() const override { return status; }
    double videoPosition() const override { return position; }
    void seekVideo(double seconds) override { seeks.append(seconds); }
    double videoDuration() const override { return duration; }
    double videoSeekLatency() const override { return latency; }
    double latency = -1;
    double position = -1;
    double duration = -1;
    QList<double> seeks;
    bool wantsPointer = false;
    QList<QPointF> pointers;
    PetInputStatus status;

    using PetSurface::m_callbacks; // the test plays the window's part

    std::vector<FakeSurface *> *registry;
    QString petId;
    QString output;
    PetView view;
    int applied = 0;
    int serial = 0; // creation order: an address can be reused by the next surface
    static inline int created = 0;
};

// Answers the controller's requests for masks when the test says so.
struct FakeMaskLoader : MaskLoader {
    struct Request {
        QString key, item, variant, file;
        std::size_t budget;
        Done done;
    };
    void load(const QString &key, const QString &item, const QString &variant, const QString &file, std::size_t budget, Done done) override
    {
        requests.push_back({key, item, variant, file, budget, std::move(done)});
    }
    void cancel(const QString &key) override { cancelled << key; }
    std::vector<Request> requests;
    QStringList cancelled;
};

std::shared_ptr<const MaskSet> someMasks()
{
    MaskBuilder b(40, 30, 2);
    std::vector<std::uint8_t> alpha(40 * 30, 255);
    b.addFrame(alpha.data(), 40, 1, 40, 30);
    b.addFrame(alpha.data(), 40, 1, 40, 30);
    return std::make_shared<const MaskSet>(b.finish());
}

QJsonObject placement(const QString &petId, const QString &itemId, const QString &output, double x = 10)
{
    return {{u"petId"_s, petId}, {u"itemId"_s, itemId}, {u"variant"_s, u"idle"_s}, {u"output"_s, output},
            {u"x"_s, x}, {u"y"_s, 20}, {u"width"_s, 200}, {u"opacity"_s, 0.8}, {u"playOriginal"_s, false},
            {u"paused"_s, false}};
}

QJsonObject item(const QString &file, const QString &state = u"none"_s, const QString &transcodeFile = {})
{
    QJsonObject transcode{{u"state"_s, state}};
    if (!transcodeFile.isEmpty())
        transcode.insert(u"file"_s, transcodeFile);
    return {{u"id"_s, u"anima:1"_s},
            {u"variants"_s, QJsonArray{QJsonObject{{u"name"_s, u"idle"_s}, {u"file"_s, file}, {u"width"_s, 400},
                                                   {u"height"_s, 300}, {u"transcode"_s, transcode}}}}};
}

// An animation of 10 frames at 30 fps, 400x300: the GPU frame cache has something to cache.
QJsonObject animatedItem(const QString &id, const QString &file)
{
    return {{u"id"_s, id},
            {u"variants"_s, QJsonArray{QJsonObject{{u"name"_s, u"idle"_s}, {u"file"_s, file}, {u"width"_s, 400},
                                                   {u"height"_s, 300}, {u"fps"_s, 30}, {u"frames"_s, 10},
                                                   {u"transcode"_s, QJsonObject{{u"state"_s, u"none"_s}}}}}}};
}

// A 4000x3000 animation of 10 frames at 30 fps with the given finished copies (bucket to file) and buckets being made.
QJsonObject bigItem(const QString &file, const QMap<int, QString> &done, const QList<int> &running = {})
{
    QJsonArray buckets;
    for (auto it = done.cbegin(); it != done.cend(); ++it)
        buckets.append(QJsonObject{{u"bucket"_s, it.key()}, {u"state"_s, u"done"_s}, {u"file"_s, it.value()}});
    for (const int bucket : running)
        buckets.append(QJsonObject{{u"bucket"_s, bucket}, {u"state"_s, u"running"_s}, {u"progress"_s, 0.5}});
    return {{u"id"_s, u"anima:1"_s},
            {u"variants"_s, QJsonArray{QJsonObject{{u"name"_s, u"idle"_s}, {u"file"_s, file}, {u"width"_s, 4000}, {u"height"_s, 3000}, {u"fps"_s, 30},
                                                   {u"frames"_s, 10}, {u"transcode"_s, QJsonObject{{u"state"_s, u"none"_s}}}, {u"buckets"_s, buckets}}}}};
}

QJsonObject placementWidth(const QString &petId, const QString &itemId, double width)
{
    QJsonObject object = placement(petId, itemId, u"DP-1"_s);
    object[u"width"_s] = width;
    return object;
}

// The same with a native rate of `fps` (what the daemon measures from the file's delays).
QJsonObject animatedItemAt(const QString &id, const QString &file, double fps)
{
    QJsonObject item = animatedItem(id, file);
    QJsonObject variant = item[u"variants"_s].toArray().at(0).toObject();
    variant[u"fps"_s] = fps;
    item[u"variants"_s] = QJsonArray{variant};
    return item;
}

QJsonObject placementFps(const QString &petId, const QString &itemId, int fps)
{
    QJsonObject object = placement(petId, itemId, u"DP-1"_s);
    object[u"fps"_s] = fps;
    return object;
}

// A revision 6 item: the variant lists a state per bucket, "transcode" being the one the placements need.
QJsonObject bucketItem(const QString &file, const QMap<int, QString> &done, const QMap<int, QString> &running = {})
{
    QJsonArray buckets;
    for (auto it = done.cbegin(); it != done.cend(); ++it)
        buckets.append(QJsonObject{{u"bucket"_s, it.key()}, {u"state"_s, u"done"_s}, {u"file"_s, it.value()}});
    for (auto it = running.cbegin(); it != running.cend(); ++it)
        buckets.append(QJsonObject{{u"bucket"_s, it.key()}, {u"state"_s, u"running"_s}, {u"progress"_s, 0.5}});
    return {{u"id"_s, u"anima:1"_s},
            {u"variants"_s, QJsonArray{QJsonObject{{u"name"_s, u"idle"_s}, {u"file"_s, file}, {u"width"_s, 400},
                                                   {u"height"_s, 300}, {u"transcode"_s, QJsonObject{{u"state"_s, u"none"_s}}},
                                                   {u"buckets"_s, buckets}}}}};
}

// The player process, scripted: the lines the sound controller sends are kept.
class FakePlayer final : public PetAudioLink
{
public:
    bool start() override
    {
        up = true;
        return true;
    }
    bool running() const override { return up; }
    void send(const QString &text) override { lines << text; }
    void stop() override { up = false; }
    QStringList with(const QString &word) const
    {
        QStringList out;
        for (const QString &l : lines)
            if (l.startsWith(word + u' '))
                out << l;
        return out;
    }
    QStringList lines;
    bool up = false;
};

QJsonObject withSound(QJsonObject p, const QString &mode, bool clickThrough = true)
{
    p.insert(u"audio"_s, QJsonObject{{u"asset"_s, QString(64, QLatin1Char('a'))}, {u"mode"_s, mode}, {u"volume"_s, 50}});
    p.insert(u"clickThrough"_s, clickThrough);
    return p;
}

// The daemon fake answers on the next turn of the event loop, and the answer calls the controller that asked: a controller that is destroyed with answers still
// in flight would be called after it is gone. The tests let those answers in before the controller goes.
struct DrainThenDelete {
    void operator()(OverlayController *controller) const
    {
        for (int i = 0; i < 5; ++i)
            QCoreApplication::processEvents();
        delete controller;
    }
};
using ControllerPtr = std::unique_ptr<OverlayController, DrainThenDelete>;

// An item with four animations of different shapes (idle 400x300, grab 300x300, move 200x100, rel 400x300), each 10 frames at 30 fps, for the states of a pet.
QJsonObject statesItem(const QString &dir, const QStringList &pendingVariants = {})
{
    QJsonArray variants;
    const struct {
        const char *name;
        int w, h;
    } shapes[] = {{"idle", 400, 300}, {"grab", 300, 300}, {"move", 200, 100}, {"rel", 400, 300}};
    for (const auto &shape : shapes) {
        const QString name = QString::fromLatin1(shape.name);
        QJsonArray buckets;
        if (pendingVariants.contains(name))
            buckets.append(QJsonObject{{u"bucket"_s, 256}, {u"state"_s, u"running"_s}, {u"progress"_s, 0.5}});
        variants.append(QJsonObject{{u"name"_s, name}, {u"file"_s, dir + u'/' + name + u".apng"_s}, {u"width"_s, shape.w}, {u"height"_s, shape.h}, {u"fps"_s, 30},
                                    {u"frames"_s, 10}, {u"transcode"_s, QJsonObject{{u"state"_s, u"none"_s}}}, {u"buckets"_s, buckets}});
    }
    return {{u"id"_s, u"anima:1"_s}, {u"variants"_s, variants}};
}

QJsonObject withStates(QJsonObject p, const QByteArray &states)
{
    p.insert(u"states"_s, QJsonDocument::fromJson(states).object());
    return p;
}

} // namespace

class TstOverlayController : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void createsOneSurfacePerPetOnItsOutput();
    void starRunsOnThePrimaryOutput();
    void outputsComeAndGo();
    void playsTheTranscodeWhenDone();
    void playOriginalKeepsTheOriginal();
    void arrangeFollowsTheDaemonAndToggles();
    void hidingUnmapsEveryPetAndKeepsTheirState();
    void bothSessionStatesAreReadAgainOnEveryConnection();
    void theRuntimeLayerHidesMovesAndReAnimatesOnePet();
    void aMotionIsDrawnByTheOverlayAndEndsOnItsOwn();
    void theUsersHandWinsOverTheRuntimeLayer();
    void theRuntimeLayerIsTheDaemonsAndGoesWithIt();
    void aTimelineCanHideAStageButNotMoveIt();
    void theShortcutsCallTheDaemon();
    void keepAboveReachesTheWindowAndItsDegradeIsReported();
    void theBehaviourFieldsOfAWrongOrOldPayloadFallBackToTheDefaults();
    void sessionPolicyPausesEveryPet();
    void editsGoToTheDaemon();
    void removedPetsLoseTheirSurface();
    void unknownItemsShowNothing();
    void oldDaemonsShowNothing();
    void petsStayWhenTheDaemonLeaves();
    void failingFilesAreGivenUp();
    void describeListsTheWindows();
    void arrangeEndsWhenTheDaemonVanishes();
    void transientItemErrorsKeepThePet();
    void brokenTranscodeFallsBackToTheOriginal();
    void aGoodFrameResetsTheFailures();
    void aCrashingFileIsBlockedAcrossRestarts();
    void theOpeningMarkerFollowsTheWindow();
    void resetFailuresGivesABlockedPetAnotherChance();
    void petsOpenTheirFilesOneAtATime();
    void aCrashIsChargedToTheOneFileThatWasOpening();
    void aSilentWindowIsNotWaitedForForever();
    void pausedPetsDoNotHoldTheOthersBack();
    void picksTheSmallestFinishedBucketThatFits();
    void theOutputScaleRaisesTheNeed();
    void aBrokenBucketFallsBackToAnotherOne();
    void aRevision5DaemonHasOneCopyAtTheBiggestSize();
    void sendsTheOutputScalesToARevision6Daemon();
    void theFrameCacheIsOffByDefault();
    void spritesAreAdmittedBySavingWithinTheBudget();
    void aKnownCutSizeIsWhatAdmissionCounts();
    void framesAreDecodedOnlyOnceTheVideoPlays();
    void aFinishedDecodeSwitchesTheMode();
    void aFailedDecodeKeepsTheVideoAndIsNotRetried();
    void aResizeRebuildsTheFramesAndKeepsTheOldOnesMeanwhile();
    void decodesTakeTurnsWithTheLedger();
    void theBudgetCanShrinkAndStillImagesAreNotCached();
    void mpvSpeedTurnsTheNativeRateIntoThePlacementsRate();
    void aRetimedCopyPlaysAtNormalSpeed();
    void aCopyRetimedToAnotherRateServesAFixedRateAtTheRatio();
    void aCopyRetimedBeforeServesEveryFixedRate();
    void aDaemonThatDoesNotRetimeStillGetsTheRightSpeed();
    void aChangeOfTimingIsLiveAndNeedsNoNewFrames();
    void aSpeedIsTheViewsAndLibmpvsSpeed();
    void theFrameRateLimitFollowsTheSettingsAndThePower();
    void theVideoPathIsCappedOnlyWhereTheAnimationIsFaster();
    void losingTheAdmissionDuringADecodeFreesTheLedgerTurn();
    void aDecodeMayTakeAQuarterMoreThanItsEstimate();
    void aResizeHoldsTheOldAndNewFramesAgainstTheBudget();
    void theBehavioursOfAPlacementReachTheView();
    void aPetOnTheVideoPathAsksForItsMasksOnlyWhenItNeedsThem();
    void aPassForAnOldFileIsCancelled();
    void aPetOnTheFrameCacheMakesMasksOnlyWhenItNeedsThem();
    void theDefaultSetupCostsNothing();
    void removingTheLastPetThatNeededThePointerStopsTheSource();
    void thePetsShareTheMaskBudgetAndKeepTheirUnion();
    void theGlobalPointerRunsOnlyWhileAPetNeedsItAndReachesOnlyThosePets();
    void aGlobalSourceThatFailsMakesTheSessionLocal();
    void describeReportsThePointerInput();
    void thePetsSoundFollowsTheViewTheWrapsAndTheClicks();
    void aStageCoversItsOutputWithItsOwnRectangle();
    void aDesktopStageHasOneWindowPerOutputWithTheirParts();
    void aStageThatCannotBeDrawnIsAnOrdinaryPetWithAName();
    void aFullscreenWindowHidesTheStageOnThatOutputOnly();
    void aGameHidesThePetsOnItsScreenOnlyWhenTheSettingIsOn();
    void theStageSettingAndTheOverrideDecideWhetherItHides();
    void aSessionThatCannotSayWhatIsFullscreenShowsTheStageAndSaysSo();
    void aStageWaitsForTheFirstAnswerOfThePolicyServer();
    void aStageTheSessionCannotDrawIsAnOrdinaryPetInEveryRespect();
    void aWindowThatComesBackFromAFullscreenWindowFollowsTheOthersClock();
    void oneStageHoldsAnOutputAndTheLaterOneIsNotShown();
    void turningTheModeOffGivesTheOrdinaryPetBack();
    void outputsComeAndGoUnderADesktopStage();
    void aStageTakesNoPointerAndNoMasks();
    void aStagePlaysACopyOnlyWhenItIsBigEnoughAndTheOriginalMeanwhile();
    void theResolutionCapReducesTheCopyAndIsSharedByTheStages();
    void aStageOnTheFrameCacheDecodesAtTheSizeOfItsRectangle();
    void theWindowsOfADesktopStageAreSeekedToTheFirstOnesPosition();
    void windowsOnEitherSideOfTheWrapAreCloseAndOneAboutToWrapIsNotJudged();
    void aSeekAimsAheadByWhatTheWindowMeasuredOfItsLastSeek();
    void aSeekThatEndsPastTheWrapContinuesFromTheStart();
    void withoutACompositorAStageIsAPetUntilOneRuns();
    void aSeekIsNotRepeatedBeforeItHasSettled();
    void theOutputsAreSentToTheDaemonWithTheirPlaces();
    void aMixedScaleDeskIsSizedForTheOutputTheStageIsOnAndSaysWhyTheOriginalPlays();
    void windowsOfAPetWithoutAStageHaveNoStageKeys();
    void aPetWithoutStatesHasNoMachineNoTimerAndTheWindowsItAlwaysHad();
    void theFileAndTheSizeFollowTheStateOfTheDrag();
    void theWindowKeepsItsPictureWhileTheNewFileLoadsAndSlowSwitchesAreNamed();
    void aSequenceAdvancesOnTheClockAndDroppingTheStatesStopsTheTimer();
    void aClickMovesAClickSequenceOn();
    void arrangingPausingAndHidingPutThePetBackToIdle();
    void aDodgeStepShowsTheMovingAnimationForItsLength();
    void missingVariantsPendingCopiesAndTheBudgetAreNamed();
    void aVariantTheRuntimeLayerSetsIsNotOverriddenByIdle();
    void lookMirrorsThePetTowardsTheCursorAndRunsThePointerSourceOnlyWhileItShould();
    void trackingNeedsTheGlobalPointerAndSaysSo();
    void aTimelinesTrackActionTurnsTrackingOnInTheLayerAndTheEndTurnsItOff();
    void hidingPausingAndArrangingStopTheTracking();
    void followWalksTheWindowTowardsTheCursorAndNeverWritesTheStore();
    void whenFollowingStopsTheSetOffPetGoesHomeOrIsStoredOnce();
    void aFollowOfTheRuntimeLayerNeverStoresAPlaceEvenWithReturnHomeOff();
    void aStageFollowerMovesItsPictureInsideItsScreenAndNothingElse();
    void aDesktopStageLooksButDoesNotWalk();
    void aDesktopStageLooksInTheSpaceOfItsPicture();
    void aStagePictureBiggerThanItsScreenStaysWhereItIs();
    void aFollowerIsInTheMovingStateWhileItWalks();
    void aFollowerWhoseWindowWasLostMidDragWalksAgain();
    void aPlaceTheDaemonRefusedOrChangedIsNotHeldForEver();
    void aFollowerDoesNotMakeWindowsChangeThirtyTimesASecond();
    void aParkedFollowerAsksNothingOfThePointerSource();
    void aFollowerOnAnOutputWithAnOriginSeesTheCursorInItsOwnSpace();
    void thePreviewIsLimitedAndNeverBreaksADrag();
    void aPreviewShowsAStateAndNeedsAShownPetWithStates();
    void aClickOnAOnceEntryThatIsPlayingStartsItAgain();
    void aControllerGoneBeforeTheDaemonAnswersIsNotCalled();
    void aWindowLostMidDragTakesTheGrabWithIt();
    void aHiddenPausedOrArrangedPetWaitsForNoSequence();
    void releasedIsCountedFromTheFirstFrameOfItsPicture();
    void aStateWhoseFileIsBlockedNeverDropsTheWindowFromItsOwnCallback();
    void thePictureOfTheNextStateIsPreparedWithinTheBudgetAndSwappedIn();
    void aPetWithoutStatesOrWithoutRoomPreparesNothing();
    void aFailedPreloadIsNotRetriedAndAnotherPictureDecodesAsAlways();
    void aLockedPetDoesNotFollowAndAHeldOneStandsStill();
    void aFollowerParksOverAFullscreenWindowAndIsHomeWhenHiddenOrArranged();
    void aPuppetPetIsShownWithNoneOfThePictureMachinery();
    void aPuppetsStatesPlayItsMotionsAndExpressions();
    void aClickPlaysAPuppetsTapMotion();
    void aPuppetEngineThatCrashesIsCountedRestartedAndBlocked();
    void aPuppetEngineThatRefusesIsNamedAndNotCounted();
    void aPuppetItemWithoutAModelFileInsideItsFolderIsNamedNotDropped();
    void lookIsAPuppetsGazeNotAMirror();
    void thePuppetSandboxFollowsTheImportSetting();
    void thePuppetPictureSizeFollowsItsSetting();

private:
    // A pet with states on an item of four animations, a clock the test moves, and the window's part played by the test.
    void statesPet(const QByteArray &states, const QStringList &pending = {})
    {
        daemon.items[u"anima:1"_s] = statesItem(tmp.path(), pending);
        daemon.pets = QJsonArray{withStates(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), states)};
    }
    ControllerPtr statesController()
    {
        auto controller = makeController();
        controller->setStateClock([this] { return stateClock; });
        return controller;
    }
    // The window's part, and the turn of the event loop in which the controller applies what the machine says (it never does so inside the window's callback).
    // Everything that is already on its way in the event loop (the daemon's answers, the controller's own queued turns, chains of them) has run.
    static void settle()
    {
        for (int i = 0; i < 12; ++i)
            QCoreApplication::processEvents();
    }
    static void send(FakeSurface *pet, const PetPointerEvent &event)
    {
        pet->m_callbacks.pointerEvent(event);
        QCoreApplication::processEvents();
    }
    static void click(FakeSurface *pet)
    {
        pet->m_callbacks.clicked();
        QCoreApplication::processEvents();
    }
    static PetPointerEvent grab() { return {PetPointerEvent::Kind::Grab}; }
    static PetPointerEvent drag(double dx, double dy = 0) { return {PetPointerEvent::Kind::DragMove, dx, dy}; }
    static PetPointerEvent release() { return {PetPointerEvent::Kind::Release}; }
    static PetPointerEvent dodge(int ms) { return {PetPointerEvent::Kind::Dodge, 40, 0, ms}; }
    QString fileOf(const QString &variant) const { return tmp.path() + u'/' + variant + u".apng"_s; }
    qint64 stateClock = 0;

    FakeSurface *surfaceOn(const QString &petId, const QString &output) const
    {
        for (FakeSurface *s : surfaces) {
            if (s->petId == petId && s->output == output)
                return s;
        }
        return nullptr;
    }
    static QJsonObject asStage(QJsonObject p, const QString &mode, const QString &fit = u"contain"_s, const QString &over = u"inherit"_s)
    {
        p.insert(u"stage"_s, QJsonObject{{u"mode"_s, mode}, {u"fit"_s, fit}, {u"overFullscreen"_s, over}});
        return p;
    }
    static QHash<QString, QRect> twoOutputs()
    {
        return {{u"DP-1"_s, QRect(0, 0, 1920, 1080)}, {u"HDMI-A-1"_s, QRect(1920, 0, 1280, 1024)}};
    }

    void setUpDaemon()
    {
        daemon.pets = QJsonArray{placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s)};
        daemon.items[u"anima:1"_s] = item(original);
    }
    ControllerPtr makeController()
    {
        ControllerPtr controller(new OverlayController(
            &daemon, [this](const QString &output, const QString &petId) -> std::unique_ptr<PetSurface> {
                return std::make_unique<FakeSurface>(&surfaces, petId, output);
            }));
        if (ledger)
            controller->setCrashLedger(ledger);
        controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s);
        if (surfaceAnswers)
            controller->setOutputFullscreen({}, true); // (the policy server answered: nothing is fullscreen)
        return controller;
    }
    FakeSurface *surface(const QString &petId) const
    {
        for (FakeSurface *s : surfaces) {
            if (s->petId == petId)
                return s;
        }
        return nullptr;
    }
    void connectDaemon(OverlayController &controller)
    {
        controller.start();
        daemon.availabilityChanged(true);
        QTRY_VERIFY(controller.surfaceCount() > 0 || daemon.callCount(u"GetItem"_s) > 0);
        settle();
    }

    QTemporaryDir tmp;
    QString original;
    QString transcoded;
    FakeDaemon daemon;
    std::vector<FakeSurface *> surfaces;
    CrashLedger *ledger = nullptr;
    bool surfaceAnswers = true;
};

void TstOverlayController::init()
{
    stateClock = 0;
    daemon.calls.clear();
    daemon.pets = {};
    daemon.settings = {};
    surfaceAnswers = true;
    daemon.items.clear();
    daemon.apiVersion = 5;
    daemon.arrange = false;
    daemon.transientItemErrors = 0;
    QVERIFY(surfaces.empty());
    original = tmp.filePath(u"pet.apng"_s);
    transcoded = tmp.filePath(u"pet.webm"_s);
    testutil::writeFile(original, "x");
    testutil::writeFile(transcoded, "y");
    setUpDaemon();
}

// ---- the fullscreen mode (docs/pets/stage.md) --------------------------------------------------------------------------------------

void TstOverlayController::aStageCoversItsOutputWithItsOwnRectangle()
{
    daemon.pets = QJsonArray{asStage(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), u"output"_s)};
    auto controller = makeController();
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    connectDaemon(*controller);
    QCOMPARE(controller->surfaceCount(), 1);
    QCOMPARE(int(surfaces.size()), 1);
    const PetView &view = surface(u"p1"_s)->view;
    QVERIFY(view.stage.active);
    // the item is 400x300: contained in 1920x1080 it is 1440x1080 in the middle
    QCOMPARE(view.stage.petRect, QRectF(240, 0, 1440, 1080));
    QVERIFY(!view.arrange && !view.hidden);
    QVERIFY(!view.spriteSize.isValid());
    const QJsonObject described = controller->describe().at(0).toObject();
    QCOMPARE(described.value(u"stage"_s).toString(), u"output"_s);
    QCOMPARE(described.value(u"stageOutputs"_s).toArray().at(0).toString(), u"DP-1"_s);
    QCOMPARE(described.value(u"stageHidden"_s).toString(), QString());
    // cover and stretch change the rectangle, in the window's coordinates
    daemon.pets = QJsonArray{asStage(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), u"output"_s, u"cover"_s)};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_COMPARE(surface(u"p1"_s)->view.stage.petRect, QRectF(0, -(1440.0 - 1080) / 2, 1920, 1440));
    daemon.pets = QJsonArray{asStage(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), u"output"_s, u"stretch"_s)};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_COMPARE(surface(u"p1"_s)->view.stage.petRect, QRectF(0, 0, 1920, 1080));
    // "*" is the primary, and the stage follows it
    daemon.pets = QJsonArray{asStage(placement(u"p1"_s, u"anima:1"_s, u"*"_s), u"output"_s, u"stretch"_s)};
    daemon.emitSignal(u"PetsChanged"_s);
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"HDMI-A-1"_s, {}, twoOutputs());
    QTRY_VERIFY(surfaceOn(u"p1"_s, u"HDMI-A-1"_s));
    QCOMPARE(surface(u"p1"_s)->view.stage.petRect, QRectF(0, 0, 1280, 1024));
}

void TstOverlayController::aDesktopStageHasOneWindowPerOutputWithTheirParts()
{
    daemon.pets = QJsonArray{asStage(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), u"desktop"_s, u"stretch"_s)};
    auto controller = makeController();
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    connectDaemon(*controller);
    QTRY_COMPARE(int(surfaces.size()), 2);
    QVERIFY(surfaceOn(u"p1"_s, u"DP-1"_s));
    QVERIFY(surfaceOn(u"p1"_s, u"HDMI-A-1"_s));
    // one picture across the bounding box (3200 x 1080), each window shows its part: the rectangle is in the window's own coordinates
    QCOMPARE(surfaceOn(u"p1"_s, u"DP-1"_s)->view.stage.petRect, QRectF(0, 0, 3200, 1080));
    QCOMPARE(surfaceOn(u"p1"_s, u"HDMI-A-1"_s)->view.stage.petRect, QRectF(-1920, 0, 3200, 1080));
    QCOMPARE(controller->surfaceCount(), 1); // (a pet; its other windows belong to it)
    const QJsonObject described = controller->describe().at(0).toObject();
    QCOMPARE(described.value(u"stage"_s).toString(), u"desktop"_s);
    QCOMPARE(described.value(u"stageOutputs"_s).toArray().size(), 2);
}

void TstOverlayController::aStageThatCannotBeDrawnIsAnOrdinaryPetWithAName()
{
    daemon.pets = QJsonArray{asStage(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), u"output"_s)};
    auto controller = makeController(); // (no geometry of the outputs)
    connectDaemon(*controller);
    QCOMPARE(controller->surfaceCount(), 1);
    QVERIFY(!surface(u"p1"_s)->view.stage.active);
    const QJsonObject described = controller->describe().at(0).toObject();
    QVERIFY(described.value(u"degraded"_s).toArray().contains(u"stage: not fullscreen on this session"_s));
    // once the geometry comes it is a stage
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    QTRY_VERIFY(surface(u"p1"_s)->view.stage.active);
}

void TstOverlayController::aGameHidesThePetsOnItsScreenOnlyWhenTheSettingIsOn()
{
    daemon.pets = QJsonArray{placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), placement(u"p2"_s, u"anima:1"_s, u"HDMI-A-1"_s)};
    auto controller = makeController();
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    connectDaemon(*controller);
    QTRY_VERIFY(surface(u"p1"_s) && surface(u"p2"_s));
    // the setting off (the default): a game hides nothing
    controller->setGameScreens({u"DP-1"_s});
    QVERIFY(!surface(u"p1"_s)->view.hidden);
    // on: the pet on the game's screen hides and pauses, the other one plays on
    daemon.settings = QJsonObject{{u"petsHideOnGame"_s, true}};
    daemon.emitSignal(u"SettingsChanged"_s);
    QTRY_VERIFY(surface(u"p1"_s)->view.hidden);
    QVERIFY(surface(u"p1"_s)->view.paused);
    QVERIFY(!surface(u"p2"_s)->view.hidden);
    QVERIFY(!surface(u"p2"_s)->view.paused);
    QVERIFY(controller->describe().at(0).toObject().value(u"gameHidden"_s).toBool());
    // the game moves to the other screen
    controller->setGameScreens({u"HDMI-A-1"_s});
    QVERIFY(!surface(u"p1"_s)->view.hidden);
    QVERIFY(surface(u"p2"_s)->view.hidden);
    // a session that cannot say where: every screen
    controller->setGameScreens({u"*"_s});
    QVERIFY(surface(u"p1"_s)->view.hidden && surface(u"p2"_s)->view.hidden);
    // the game ends: both come back
    controller->setGameScreens({});
    QVERIFY(!surface(u"p1"_s)->view.hidden && !surface(u"p2"_s)->view.hidden);
    QVERIFY(!controller->describe().at(0).toObject().contains(u"gameHidden"_s));
}

void TstOverlayController::aFullscreenWindowHidesTheStageOnThatOutputOnly()
{
    daemon.pets = QJsonArray{asStage(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), u"desktop"_s)};
    auto controller = makeController();
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    connectDaemon(*controller);
    QTRY_COMPARE(int(surfaces.size()), 2);
    controller->setOutputFullscreen({{u"HDMI-A-1"_s, u"mpv"_s}}, true);
    QVERIFY(surfaceOn(u"p1"_s, u"HDMI-A-1"_s)->view.hidden);
    QVERIFY(surfaceOn(u"p1"_s, u"HDMI-A-1"_s)->view.paused);
    QVERIFY(!surfaceOn(u"p1"_s, u"DP-1"_s)->view.hidden);
    QVERIFY(!surfaceOn(u"p1"_s, u"DP-1"_s)->view.paused);
    QCOMPARE(controller->describe().at(0).toObject().value(u"stageHidden"_s).toString(), QString()); // (one of two is still drawn)
    // both
    controller->setOutputFullscreen({{u"HDMI-A-1"_s, u"mpv"_s}, {u"DP-1"_s, u"game"_s}}, true);
    QVERIFY(surfaceOn(u"p1"_s, u"DP-1"_s)->view.hidden);
    QJsonObject described = controller->describe().at(0).toObject();
    QCOMPARE(described.value(u"stageHidden"_s).toString(), u"fullscreen"_s);
    QVERIFY(described.value(u"degraded"_s).toArray().contains(u"stage: hidden by a fullscreen window: game"_s));
    // a game or a listed application ("*") hides every output
    controller->setOutputFullscreen({{u"*"_s, QString()}}, true);
    QVERIFY(surfaceOn(u"p1"_s, u"DP-1"_s)->view.hidden && surfaceOn(u"p1"_s, u"HDMI-A-1"_s)->view.hidden);
    // and it returns at once
    controller->setOutputFullscreen({}, true);
    QVERIFY(!surfaceOn(u"p1"_s, u"DP-1"_s)->view.hidden && !surfaceOn(u"p1"_s, u"HDMI-A-1"_s)->view.hidden);
    QVERIFY(!surfaceOn(u"p1"_s, u"DP-1"_s)->view.paused);
    QCOMPARE(controller->describe().at(0).toObject().value(u"stageHidden"_s).toString(), QString());
}

void TstOverlayController::theStageSettingAndTheOverrideDecideWhetherItHides()
{
    daemon.pets = QJsonArray{asStage(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), u"output"_s, u"contain"_s, u"show"_s),
                             asStage(placement(u"p2"_s, u"anima:1"_s, u"HDMI-A-1"_s), u"output"_s, u"contain"_s, u"inherit"_s)};
    auto controller = makeController();
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    connectDaemon(*controller);
    controller->setOutputFullscreen({{u"DP-1"_s, u"mpv"_s}, {u"HDMI-A-1"_s, u"mpv"_s}}, true);
    QVERIFY(!surface(u"p1"_s)->view.hidden); // show: always
    QVERIFY(surface(u"p2"_s)->view.hidden);  // inherit: the setting is on by default
    // the setting off: inherit shows; hide still hides
    daemon.settings = QJsonObject{{u"stageHideOverFullscreen"_s, false}};
    daemon.emitSignal(u"SettingsChanged"_s);
    QTRY_VERIFY(!surface(u"p2"_s)->view.hidden);
    daemon.pets = QJsonArray{asStage(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), u"output"_s, u"contain"_s, u"hide"_s),
                             asStage(placement(u"p2"_s, u"anima:1"_s, u"HDMI-A-1"_s), u"output"_s, u"contain"_s, u"inherit"_s)};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_VERIFY(surface(u"p1"_s)->view.hidden);
    QVERIFY(!surface(u"p2"_s)->view.hidden);
}

void TstOverlayController::aSessionThatCannotSayWhatIsFullscreenShowsTheStageAndSaysSo()
{
    daemon.pets = QJsonArray{asStage(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), u"output"_s)};
    auto controller = makeController();
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    connectDaemon(*controller);
    const auto unknown = [&] { return controller->describe().at(0).toObject().value(u"degraded"_s).toArray().contains(u"stage: fullscreen apps unknown"_s); };
    // the surface answered "known: false" (no window monitor): shown, with the name
    controller->setOutputFullscreen({}, false);
    QVERIFY(!surface(u"p1"_s)->view.hidden);
    QVERIFY(unknown());
    controller->setOutputFullscreen({}, true);
    QVERIFY(!unknown());
    controller->setOutputFullscreen({}, false);
    QVERIFY(unknown());
    // a pet that says `show` does not depend on the answer
    daemon.pets = QJsonArray{asStage(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), u"output"_s, u"contain"_s, u"show"_s)};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_VERIFY(!unknown());
}

void TstOverlayController::aStageWaitsForTheFirstAnswerOfThePolicyServer()
{
    // Until the surface has said anything (or is known to be absent) a stage that would hide for a fullscreen window is not shown: a game must not have it over
    // it for the length of the first read.
    surfaceAnswers = false;
    daemon.pets = QJsonArray{asStage(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), u"desktop"_s)};
    auto controller = makeController();
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    connectDaemon(*controller);
    QTRY_COMPARE(int(surfaces.size()), 2);
    for (FakeSurface *window : surfaces) {
        QVERIFY(window->view.hidden);
        QVERIFY(window->view.paused);
    }
    QCOMPARE(controller->describe().at(0).toObject().value(u"stageHidden"_s).toString(), u"starting"_s);
    QVERIFY(!controller->describe().at(0).toObject().value(u"degraded"_s).toArray().contains(u"stage: fullscreen apps unknown"_s)); // (nothing is "unknown" yet)
    // the surface answers: a fullscreen window on the first output keeps it hidden there only
    controller->setOutputFullscreen({{u"DP-1"_s, u"game"_s}}, true);
    QVERIFY(surfaceOn(u"p1"_s, u"DP-1"_s)->view.hidden);
    QVERIFY(!surfaceOn(u"p1"_s, u"HDMI-A-1"_s)->view.hidden);
    QCOMPARE(controller->describe().at(0).toObject().value(u"stageHidden"_s).toString(), QString());
    // a surface known to be absent (its first word is "nothing is known") shows the stage
    surfaces.clear();
    surfaceAnswers = false;
    auto absent = makeController();
    absent->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    connectDaemon(*absent);
    QVERIFY(surfaceOn(u"p1"_s, u"DP-1"_s)->view.hidden);
    absent->setOutputFullscreen({}, false);
    QVERIFY(!surfaceOn(u"p1"_s, u"DP-1"_s)->view.hidden);
    QVERIFY(!surfaceOn(u"p1"_s, u"HDMI-A-1"_s)->view.hidden);
}

void TstOverlayController::aStageTheSessionCannotDrawIsAnOrdinaryPetInEveryRespect()
{
    // A stage whose output has no place in the layout is drawn as a pet: it needs masks if it asks for pointer behaviours, and it plays a copy sized as a pet.
    FakeMaskLoader loader;
    QJsonObject p = asStage(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), u"output"_s);
    p.insert(u"hoverHide"_s, true);
    p.insert(u"clickThrough"_s, false);
    daemon.pets = QJsonArray{p};
    auto controller = makeController(); // (no geometry)
    controller->setMaskLoader(&loader);
    connectDaemon(*controller);
    const PetView &view = surface(u"p1"_s)->view;
    QVERIFY(!view.stage.active);
    QVERIFY(!view.placement.stage.active()); // the window is given the pet it is drawing
    QVERIFY(needsMasks(view.placement));
    QVERIFY(view.maskBudget > 0);
    QVERIFY(controller->describe().at(0).toObject().value(u"degraded"_s).toArray().contains(u"stage: not fullscreen on this session"_s));
    // an output that is gone, and one that is too large, have their own names
    controller->setOutputs({u"DP-1"_s}, u"DP-1"_s, {}, {{u"DP-1"_s, QRect(0, 0, 20000, 1080)}});
    QTRY_VERIFY(controller->describe().at(0).toObject().value(u"degraded"_s).toArray().contains(u"stage: output too large"_s));
    QVERIFY(surface(u"p1"_s)); // (shown as a pet)
    daemon.pets = QJsonArray{asStage(placement(u"p1"_s, u"anima:1"_s, u"HDMI-A-1"_s), u"output"_s)};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_VERIFY(controller->describe().at(0).toObject().value(u"degraded"_s).toArray().contains(u"stage: output missing"_s));
}

void TstOverlayController::aWindowThatComesBackFromAFullscreenWindowFollowsTheOthersClock()
{
    daemon.pets = QJsonArray{asStage(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), u"desktop"_s)};
    auto controller = makeController();
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    connectDaemon(*controller);
    QTRY_COMPARE(int(surfaces.size()), 2);
    FakeSurface *first = surfaceOn(u"p1"_s, u"DP-1"_s);
    FakeSurface *second = surfaceOn(u"p1"_s, u"HDMI-A-1"_s);
    // a fullscreen window on the first output pauses its slice (no position); the second one plays on
    controller->setOutputFullscreen({{u"DP-1"_s, u"game"_s}}, true);
    first->position = -1;
    second->position = 40.0;
    controller->syncStageVideo();
    QVERIFY(first->seeks.isEmpty() && second->seeks.isEmpty());
    // it leaves: the first window plays again from where it stopped, far behind. The reference is the window that kept playing (the first by name would drag it back).
    controller->setOutputFullscreen({}, true);
    first->position = 12.0;
    second->position = 41.0;
    controller->syncStageVideo();
    QCOMPARE(first->seeks.size(), 1);
    QVERIFY(std::abs(first->seeks.first() - 41.1) < 1e-9); // (aimed 0.1 s ahead, the allowance for a seek whose time is not measured yet)
    QVERIFY(second->seeks.isEmpty());
    // and the round after, both having a position, the first by name leads again
    first->seeks.clear();
    first->position = 42.0;
    second->position = 42.5;
    controller->syncStageVideo();
    QCOMPARE(second->seeks.size(), 1);
    QVERIFY(std::abs(second->seeks.first() - 42.1) < 1e-9);
}

void TstOverlayController::oneStageHoldsAnOutputAndTheLaterOneIsNotShown()
{
    // (the store refuses this, but a file edited by hand, or "*" and the primary's own name, can say it)
    daemon.pets = QJsonArray{asStage(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), u"output"_s),
                             asStage(placement(u"p2"_s, u"anima:1"_s, u"*"_s), u"output"_s),
                             asStage(placement(u"p3"_s, u"anima:1"_s, u"HDMI-A-1"_s), u"output"_s)};
    auto controller = makeController();
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    connectDaemon(*controller);
    QVERIFY(surface(u"p1"_s));
    QVERIFY(!surface(u"p2"_s)); // the primary is DP-1, which p1 has
    QVERIFY(surface(u"p3"_s));
    for (const QJsonValue &value : controller->describe()) {
        const QJsonObject o = value.toObject();
        if (o.value(u"petId"_s).toString() == u"p2"_s) {
            QVERIFY(!o.value(u"shown"_s).toBool());
            QVERIFY(o.value(u"degraded"_s).toArray().contains(u"stage: output already has a stage"_s));
        }
    }
    // a desktop stage before them takes everything
    daemon.pets = QJsonArray{asStage(placement(u"d"_s, u"anima:1"_s, u"DP-1"_s), u"desktop"_s), asStage(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), u"output"_s)};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_VERIFY(surface(u"d"_s));
    QTRY_VERIFY(!surface(u"p1"_s));
}

void TstOverlayController::turningTheModeOffGivesTheOrdinaryPetBack()
{
    daemon.pets = QJsonArray{asStage(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s, 33), u"desktop"_s)};
    auto controller = makeController();
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    connectDaemon(*controller);
    QTRY_COMPARE(int(surfaces.size()), 2);
    daemon.pets = QJsonArray{placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s, 33)};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_COMPARE(int(surfaces.size()), 1);
    QVERIFY(!surface(u"p1"_s)->view.stage.active);
    // the place and the size it had are the placement's own (nothing was overwritten)
    QCOMPARE(surface(u"p1"_s)->view.placement.x, 33.0);
    QCOMPARE(surface(u"p1"_s)->view.placement.width, 200.0);
    QVERIFY(controller->describe().at(0).toObject().value(u"stage"_s).isUndefined());
}

void TstOverlayController::outputsComeAndGoUnderADesktopStage()
{
    daemon.pets = QJsonArray{asStage(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), u"desktop"_s, u"stretch"_s)};
    auto controller = makeController();
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    connectDaemon(*controller);
    QTRY_COMPARE(int(surfaces.size()), 2);
    // the second output is unplugged: one window, the picture is the first output's
    controller->setOutputs({u"DP-1"_s}, u"DP-1"_s, {}, {{u"DP-1"_s, QRect(0, 0, 1920, 1080)}});
    QTRY_COMPARE(int(surfaces.size()), 1);
    QCOMPARE(surface(u"p1"_s)->view.stage.petRect, QRectF(0, 0, 1920, 1080));
    // and plugged again
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    QTRY_COMPARE(int(surfaces.size()), 2);
    QCOMPARE(surfaceOn(u"p1"_s, u"HDMI-A-1"_s)->view.stage.petRect, QRectF(-1920, 0, 3200, 1080));
    // the first output goes: the window of the other becomes the pet's own
    controller->setOutputs({u"HDMI-A-1"_s}, u"HDMI-A-1"_s, {}, {{u"HDMI-A-1"_s, QRect(1920, 0, 1280, 1024)}});
    QTRY_VERIFY(surfaceOn(u"p1"_s, u"HDMI-A-1"_s));
    QTRY_COMPARE(int(surfaces.size()), 1);
    QCOMPARE(surface(u"p1"_s)->view.stage.petRect, QRectF(0, 0, 1280, 1024));
}

void TstOverlayController::aStageTakesNoPointerAndNoMasks()
{
    FakeMaskLoader loader;
    QJsonObject p = asStage(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), u"output"_s);
    p.insert(u"hoverHide"_s, true);
    p.insert(u"clickThrough"_s, false);
    daemon.pets = QJsonArray{p};
    auto controller = makeController();
    controller->setMaskLoader(&loader);
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    connectDaemon(*controller);
    QVERIFY(surface(u"p1"_s)->view.stage.active);
    QVERIFY(!needsMasks(surface(u"p1"_s)->view.placement));
    QVERIFY(loader.requests.empty());
    QCOMPARE(surface(u"p1"_s)->view.maskBudget, qint64(0));
}

void TstOverlayController::aStagePlaysACopyOnlyWhenItIsBigEnoughAndTheOriginalMeanwhile()
{
    // 4000x3000 contained in 1920x1080 is 1440x1080: the animation is drawn 1440 px on its long side.
    daemon.apiVersion = 17;
    daemon.pets = QJsonArray{asStage(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), u"output"_s)};
    daemon.items[u"anima:1"_s] = bigItem(original, {{650, transcoded}}, {1920});
    auto controller = makeController();
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    connectDaemon(*controller);
    // a 650 px copy would be stretched over the screen: the original plays, and the page says a copy is being made
    QCOMPARE(surface(u"p1"_s)->view.file, original);
    QVERIFY(controller->describe().at(0).toObject().value(u"degraded"_s).toArray().contains(u"stage: transcode pending"_s));
    // the 1920 copy is done: it plays (and nothing is pending)
    const QString big = tmp.filePath(u"pet-1920.webm"_s);
    testutil::writeFile(big, "z");
    daemon.items[u"anima:1"_s] = bigItem(original, {{650, transcoded}, {1920, big}});
    daemon.emitSignal(u"TranscodeChanged"_s, {u"anima:1"_s});
    QTRY_COMPARE(surface(u"p1"_s)->view.file, big);
    QCOMPARE(surface(u"p1"_s)->view.bucket, 1920);
    QVERIFY(!controller->describe().at(0).toObject().value(u"degraded"_s).toArray().contains(u"stage: transcode pending"_s));
    // a smaller screen scale needs less: a 1280 copy is enough for a 1280x1024 output
    daemon.pets = QJsonArray{asStage(placement(u"p1"_s, u"anima:1"_s, u"HDMI-A-1"_s), u"output"_s)};
    const QString mid = tmp.filePath(u"pet-1280.webm"_s);
    testutil::writeFile(mid, "m");
    daemon.items[u"anima:1"_s] = bigItem(original, {{650, transcoded}, {1280, mid}, {1920, big}});
    daemon.emitSignal(u"PetsChanged"_s);
    daemon.emitSignal(u"TranscodeChanged"_s, {u"anima:1"_s});
    QTRY_COMPARE(surface(u"p1"_s)->view.bucket, 1280);
}

void TstOverlayController::theResolutionCapReducesTheCopyAndIsSharedByTheStages()
{
    // One stage on DP-1 covers 1440x1080 = 1 555 200 pixels. A cap of 1 000 000 draws it at sqrt(1e6 / 1.5552e6) = 0.802 of the size: 1440 x 0.802 = 1155 px.
    daemon.apiVersion = 17;
    daemon.settings = QJsonObject{{u"stageMaxPixels"_s, 1000000}};
    daemon.pets = QJsonArray{asStage(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), u"output"_s)};
    const QString mid = tmp.filePath(u"pet-1280.webm"_s);
    testutil::writeFile(mid, "m");
    daemon.items[u"anima:1"_s] = bigItem(original, {{1280, mid}});
    auto controller = makeController();
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    connectDaemon(*controller);
    QTRY_COMPARE(surface(u"p1"_s)->view.bucket, 1280); // (1155 px needed: the 1280 copy is the smallest that is big enough)
    QJsonObject o = controller->describe().at(0).toObject();
    QVERIFY(o.value(u"stageScale"_s).toDouble() < 0.90 && o.value(u"stageScale"_s).toDouble() > 0.88);  // (the 1280 copy against the 1440 it would have been)
    QVERIFY(o.value(u"degraded"_s).toArray().contains(u"stage: resolution reduced"_s));
    // the cap is not met by an original (nothing smaller to play): it is shown as it is and does not claim a reduction
    daemon.items[u"anima:1"_s] = bigItem(original, {});
    daemon.emitSignal(u"TranscodeChanged"_s, {u"anima:1"_s});
    QTRY_COMPARE(surface(u"p1"_s)->view.file, original);
    o = controller->describe().at(0).toObject();
    QCOMPARE(o.value(u"stageScale"_s).toDouble(), 1.0);
    QVERIFY(!o.value(u"degraded"_s).toArray().contains(u"stage: resolution reduced"_s));
    // a roomy cap changes nothing
    daemon.settings = QJsonObject{{u"stageMaxPixels"_s, 33177600}};
    daemon.items[u"anima:1"_s] = bigItem(original, {{1920, mid}});
    daemon.emitSignal(u"SettingsChanged"_s);
    daemon.emitSignal(u"TranscodeChanged"_s, {u"anima:1"_s});
    QTRY_COMPARE(surface(u"p1"_s)->view.bucket, 1920);
    QCOMPARE(controller->describe().at(0).toObject().value(u"stageScale"_s).toDouble(), 1.0);
    // two stages share the cap: both are drawn smaller than one alone would be
    daemon.items[u"anima:1"_s] = bigItem(original, {{1280, mid}});
    daemon.emitSignal(u"TranscodeChanged"_s, {u"anima:1"_s});
    daemon.settings = QJsonObject{{u"stageMaxPixels"_s, 2000000}};
    daemon.emitSignal(u"SettingsChanged"_s);
    settle();
    QCOMPARE(controller->describe().at(0).toObject().value(u"stageScale"_s).toDouble(), 1.0); // (1 555 200 alone fits)
    daemon.pets = QJsonArray{asStage(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), u"output"_s), asStage(placement(u"p2"_s, u"anima:1"_s, u"HDMI-A-1"_s), u"output"_s)};
    daemon.emitSignal(u"PetsChanged"_s);
    // (1 555 200 + 1 228 800 = 2 784 000 px against 2 000 000: a factor of 0.85 for both)
    QTRY_COMPARE(int(controller->describe().size()), 2);
    QVERIFY(controller->describe().at(0).toObject().value(u"stageScale"_s).toDouble() < 1.0);
}

void TstOverlayController::aStageOnTheFrameCacheDecodesAtTheSizeOfItsRectangle()
{
    daemon.pets = QJsonArray{asStage(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), u"output"_s)};
    daemon.items[u"anima:1"_s] = animatedItem(u"anima:1"_s, original); // 400x300, 10 frames
    auto controller = makeController();
    controller->setFrameCacheBudget(256 << 20);
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    connectDaemon(*controller);
    // the frames are never made larger than the file: 400x300 of the 1440x1080 rectangle
    QCOMPARE(surface(u"p1"_s)->view.spriteSize, QSize(400, 300));
    QCOMPARE(surface(u"p1"_s)->view.stage.petRect, QRectF(240, 0, 1440, 1080));
    // a desktop stage plays through libmpv: no frames, whatever the budget
    daemon.pets = QJsonArray{asStage(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), u"desktop"_s)};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_VERIFY(!surfaceOn(u"p1"_s, u"DP-1"_s)->view.spriteSize.isValid());
    QVERIFY(!surfaceOn(u"p1"_s, u"HDMI-A-1"_s)->view.spriteSize.isValid());
}

void TstOverlayController::theWindowsOfADesktopStageAreSeekedToTheFirstOnesPosition()
{
    daemon.pets = QJsonArray{asStage(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), u"desktop"_s)};
    auto controller = makeController();
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    connectDaemon(*controller);
    QTRY_COMPARE(int(surfaces.size()), 2);
    FakeSurface *first = surfaceOn(u"p1"_s, u"DP-1"_s);
    FakeSurface *second = surfaceOn(u"p1"_s, u"HDMI-A-1"_s);
    // within the tolerance: nothing
    first->position = 2.00;
    second->position = 2.04;
    controller->syncStageVideo();
    QVERIFY(first->seeks.isEmpty() && second->seeks.isEmpty());
    // the second one drifted: it is seeked to the first one's position (the reference is the first output by name)
    second->position = 2.30;
    controller->syncStageVideo();
    QVERIFY(first->seeks.isEmpty());
    QCOMPARE(second->seeks.size(), 1);
    QVERIFY(std::abs(second->seeks.first() - 2.1) < 1e-9);
    // a window without a position (loading, hidden) is left out, and the reference is the next one
    first->position = -1;
    second->position = 5.0;
    controller->syncStageVideo();
    QCOMPARE(second->seeks.size(), 1);
    // an ordinary pet is never touched
    daemon.pets = QJsonArray{placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s)};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_COMPARE(int(surfaces.size()), 1);
    surface(u"p1"_s)->position = 1;
    controller->syncStageVideo();
    QVERIFY(surface(u"p1"_s)->seeks.isEmpty());
}

void TstOverlayController::aMixedScaleDeskIsSizedForTheOutputTheStageIsOnAndSaysWhyTheOriginalPlays()
{
    // "*" is the primary (the overlay says which): a 4K output at scale 2 (1920x1080 logical) wants a 3840 px copy, the 1440p one at scale 1 only 2560.
    daemon.apiVersion = 17;
    const QHash<QString, QRect> places{{u"DP-1"_s, QRect(1920, 0, 2560, 1440)}, {u"HDMI-A-1"_s, QRect(0, 0, 1920, 1080)}};
    const QString small = tmp.filePath(u"pet-2560.webm"_s);
    const QString big = tmp.filePath(u"pet-3840.webm"_s);
    testutil::writeFile(small, "s");
    testutil::writeFile(big, "b");
    daemon.pets = QJsonArray{asStage(placement(u"p1"_s, u"anima:1"_s, u"*"_s), u"output"_s)};
    daemon.items[u"anima:1"_s] = bigItem(original, {{2560, small}});
    auto controller = makeController();
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"HDMI-A-1"_s, {{u"HDMI-A-1"_s, 2.0}}, places);
    connectDaemon(*controller);
    // the primary is the 4K one: the 2560 copy is too small for it, the original plays, and the page says so (no copy is being made)
    QCOMPARE(surface(u"p1"_s)->view.file, original);
    QVERIFY(controller->describe().at(0).toObject().value(u"degraded"_s).toArray().contains(u"stage: no copy big enough"_s));
    daemon.items[u"anima:1"_s] = bigItem(original, {{2560, small}, {3840, big}});
    daemon.emitSignal(u"TranscodeChanged"_s, {u"anima:1"_s});
    QTRY_COMPARE(surface(u"p1"_s)->view.bucket, 3840);
    // the primary is the 1440p one: the 2560 copy serves it
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {{u"HDMI-A-1"_s, 2.0}}, places);
    QTRY_COMPARE(surface(u"p1"_s)->view.bucket, 2560);
    // and the daemon is told which is the primary
    const QJsonObject sent = QJsonDocument::fromJson(daemon.lastArgs(u"SetOutputScales"_s).value(0).toString().toUtf8()).object();
    QVERIFY(sent.value(u"DP-1"_s).toObject().value(u"primary"_s).toBool());
    QVERIFY(!sent.value(u"HDMI-A-1"_s).toObject().value(u"primary"_s).toBool());
}

void TstOverlayController::windowsOnEitherSideOfTheWrapAreCloseAndOneAboutToWrapIsNotJudged()
{
    daemon.pets = QJsonArray{asStage(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), u"desktop"_s)};
    auto controller = makeController();
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    connectDaemon(*controller);
    QTRY_COMPARE(int(surfaces.size()), 2);
    FakeSurface *first = surfaceOn(u"p1"_s, u"DP-1"_s);
    FakeSurface *second = surfaceOn(u"p1"_s, u"HDMI-A-1"_s);
    first->duration = second->duration = 4.0;
    // the second is within 0.3 s of the end of the loop while the first has just wrapped: they are 0.1 s apart on the circle and nothing is done (it is not judged)
    first->position = 0.05;
    second->position = 3.85;
    controller->syncStageVideo();
    QVERIFY(first->seeks.isEmpty() && second->seeks.isEmpty());
    // mid loop, on the circle: 3.4 is 0.6 s behind 0.0... the first (0.20) and the second (3.60) are 0.6 s apart across the wrap, and the second is seeked to the first
    first->position = 0.20;
    second->position = 3.60;
    controller->syncStageVideo();
    QVERIFY(first->seeks.isEmpty());
    QCOMPARE(second->seeks.size(), 1);
    QVERIFY(std::abs(second->seeks.first() - 0.30) < 1e-9); // (0.20 and the 0.1 s allowance)
}

void TstOverlayController::aSeekAimsAheadByWhatTheWindowMeasuredOfItsLastSeek()
{
    daemon.pets = QJsonArray{asStage(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), u"desktop"_s)};
    auto controller = makeController();
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    connectDaemon(*controller);
    QTRY_COMPARE(int(surfaces.size()), 2);
    FakeSurface *first = surfaceOn(u"p1"_s, u"DP-1"_s);
    FakeSurface *second = surfaceOn(u"p1"_s, u"HDMI-A-1"_s);
    // a window whose seeks take 0.35 s (a 4K copy on a slow computer) is aimed 0.35 s ahead: it lands where the others are by then
    second->latency = 0.35;
    first->position = 20.0;
    second->position = 25.0;
    controller->syncStageVideo();
    QCOMPARE(second->seeks.size(), 1);
    QVERIFY(std::abs(second->seeks.first() - 20.35) < 1e-9);
    // a measurement that is absurd (the machine stalled) is not followed further than a second (the controller's own timer also rounds, so the wait covers it)
    second->latency = 30.0;
    second->position = 25.0;
    QTest::qWait(3600);
    controller->syncStageVideo();
    QVERIFY(second->seeks.size() >= 2);
    QVERIFY2(std::abs(second->seeks.last() - 21.0) < 1e-9, qPrintable(QString::number(second->seeks.last())));
    // it converges: once the window is where the others are (within the tolerance), nothing more is done
    const int before = second->seeks.size();
    second->latency = 0.35;
    first->position = 30.0;
    second->position = 30.05;
    QTest::qWait(3600);
    controller->syncStageVideo();
    QCOMPARE(second->seeks.size(), before);
}

void TstOverlayController::aSeekThatEndsPastTheWrapContinuesFromTheStart()
{
    daemon.pets = QJsonArray{asStage(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), u"desktop"_s)};
    auto controller = makeController();
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    connectDaemon(*controller);
    QTRY_COMPARE(int(surfaces.size()), 2);
    FakeSurface *first = surfaceOn(u"p1"_s, u"DP-1"_s);
    FakeSurface *second = surfaceOn(u"p1"_s, u"HDMI-A-1"_s);
    first->duration = second->duration = 4.0;
    second->latency = 0.2;
    first->position = 3.6;   // the reference is 0.4 s from the end of the loop
    second->position = 2.0;
    controller->syncStageVideo();
    QCOMPARE(second->seeks.size(), 1);
    QVERIFY2(std::abs(second->seeks.first() - 3.8) < 1e-9, qPrintable(QString::number(second->seeks.first())));
    // 3.65 + 0.5 s of latency passes the end of the 4 s loop: the seek lands on the start side of the wrap (the controller's own timer also rounds: the wait covers it)
    second->latency = 0.5;
    first->position = 3.55;  // (0.45 s from the end: not left out of the round)
    second->position = 1.0;
    QTest::qWait(3600);
    controller->syncStageVideo();
    QVERIFY(second->seeks.size() >= 2);
    QVERIFY2(std::abs(second->seeks.last() - 0.05) < 1e-9, qPrintable(QString::number(second->seeks.last())));
}

void TstOverlayController::withoutACompositorAStageIsAPetUntilOneRuns()
{
    QJsonObject p = asStage(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), u"output"_s);
    p.insert(u"hoverHide"_s, true);
    daemon.pets = QJsonArray{p};
    auto controller = makeController();
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    connectDaemon(*controller);
    QVERIFY(surface(u"p1"_s)->view.stage.active); // (a session with a compositor is the default)
    // X11 with no compositing manager: an opaque full-screen rectangle with no input would cover the desktop: a pet, with the reason
    controller->setStageCompositing(false);
    QVERIFY(!surface(u"p1"_s)->view.stage.active);
    QVERIFY(!surface(u"p1"_s)->view.placement.stage.active());
    QVERIFY(needsMasks(surface(u"p1"_s)->view.placement)); // (it asks for the pointer behaviours, as a pet does)
    QVERIFY(controller->describe().at(0).toObject().value(u"degraded"_s).toArray().contains(u"stage: needs a compositor"_s));
    QVERIFY(controller->describe().at(0).toObject().value(u"stage"_s).isUndefined()); // not a stage: no stage keys
    // a desktop stage too: one window, not one opaque rectangle per output
    daemon.pets = QJsonArray{asStage(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), u"desktop"_s)};
    daemon.emitSignal(u"PetsChanged"_s);
    QTest::qWait(100);
    QCOMPARE(int(surfaces.size()), 1);
    QVERIFY(!surface(u"p1"_s)->view.stage.active);
    // the compositing manager starts: the stage appears (and the second window of the desktop stage), and the name goes
    controller->setStageCompositing(true);
    QTRY_COMPARE(int(surfaces.size()), 2);
    QVERIFY(surface(u"p1"_s)->view.stage.active);
    QVERIFY(!controller->describe().at(0).toObject().value(u"degraded"_s).toArray().contains(u"stage: needs a compositor"_s));
    // and stops again: back to a pet
    controller->setStageCompositing(false);
    QTRY_COMPARE(int(surfaces.size()), 1);
    QVERIFY(!surface(u"p1"_s)->view.stage.active);
}

void TstOverlayController::aSeekIsNotRepeatedBeforeItHasSettled()
{
    daemon.pets = QJsonArray{asStage(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), u"desktop"_s)};
    auto controller = makeController();
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    connectDaemon(*controller);
    QTRY_COMPARE(int(surfaces.size()), 2);
    FakeSurface *first = surfaceOn(u"p1"_s, u"DP-1"_s);
    FakeSurface *second = surfaceOn(u"p1"_s, u"HDMI-A-1"_s);
    first->position = 10.0;
    second->position = 12.0;
    controller->syncStageVideo();
    QCOMPARE(second->seeks.size(), 1);
    QVERIFY(std::abs(second->seeks.first() - 10.1) < 1e-9);
    // the seek has not shown in the position yet: the next round does not seek again
    controller->syncStageVideo();
    controller->syncStageVideo();
    QCOMPARE(second->seeks.size(), 1);
    // after the cooldown it may
    QTest::qWait(3100);
    controller->syncStageVideo();
    QCOMPARE(second->seeks.size(), 2);
}

void TstOverlayController::theOutputsAreSentToTheDaemonWithTheirPlaces()
{
    daemon.apiVersion = 17;
    auto controller = makeController();
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {{u"DP-1"_s, 1.5}}, twoOutputs());
    connectDaemon(*controller);
    QTRY_VERIFY(daemon.callCount(u"SetOutputScales"_s) > 0);
    QJsonObject sent = QJsonDocument::fromJson(daemon.lastArgs(u"SetOutputScales"_s).value(0).toString().toUtf8()).object();
    QCOMPARE(sent.value(u"DP-1"_s).toObject().value(u"scale"_s).toDouble(), 1.5);
    QCOMPARE(sent.value(u"DP-1"_s).toObject().value(u"width"_s).toInt(), 1920);
    QCOMPARE(sent.value(u"HDMI-A-1"_s).toObject().value(u"x"_s).toInt(), 1920);
    // an older daemon is sent what it understands: the scales
    daemon.apiVersion = 16;
    auto older = makeController();
    older->setOutputs({u"DP-1"_s}, u"DP-1"_s, {{u"DP-1"_s, 2.0}}, twoOutputs());
    older->start();
    daemon.availabilityChanged(true);
    QTest::qWait(60);
    sent = QJsonDocument::fromJson(daemon.lastArgs(u"SetOutputScales"_s).value(0).toString().toUtf8()).object();
    QVERIFY(sent.value(u"DP-1"_s).isDouble());
}

void TstOverlayController::windowsOfAPetWithoutAStageHaveNoStageKeys()
{
    auto controller = makeController();
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    connectDaemon(*controller);
    const QJsonObject o = controller->describe().at(0).toObject();
    for (const char *key : {"stage", "stageOutputs", "stageHidden", "stageScale"})
        QVERIFY2(!o.contains(QLatin1String(key)), key);
    QVERIFY(!surface(u"p1"_s)->view.stage.active);
    // fullscreen windows do not touch an ordinary pet: pets stay above them by design
    controller->setOutputFullscreen({{u"DP-1"_s, u"mpv"_s}}, true);
    QVERIFY(!surface(u"p1"_s)->view.hidden);
    QVERIFY(!surface(u"p1"_s)->view.paused);
}

// ---- the states of a pet (docs/pets/states.md) --------------------------------------------------------------------------------------

void TstOverlayController::aPetWithoutStatesHasNoMachineNoTimerAndTheWindowsItAlwaysHad()
{
    auto controller = makeController();
    connectDaemon(*controller);
    // Windows() of a pet that has no states or tracking is what it was before they existed: exactly these keys, none of the new ones.
    const QJsonObject o = controller->describe().at(0).toObject();
    QStringList keys = o.keys();
    keys.sort();
    QStringList golden{u"petId"_s, u"itemId"_s, u"output"_s, u"shown"_s, u"paused"_s, u"file"_s, u"bucket"_s, u"fps"_s, u"speed"_s, u"timing"_s, u"timingSpeed"_s,
                       u"fpsCap"_s, u"mode"_s, u"cacheBytes"_s, u"failures"_s, u"blocked"_s, u"degraded"_s, u"x"_s, u"y"_s, u"width"_s, u"height"_s, u"interactive"_s};
    golden.sort();
    QCOMPARE(keys, golden);
    for (const char *key : {"state", "stateSource", "tracking", "trackingActive"})
        QVERIFY2(!o.contains(QLatin1String(key)), key);
    QVERIFY(o.value(u"degraded"_s).toArray().isEmpty());
    // no timer and no machine: an event from the window is ignored, and the picture is the placement's variant
    QVERIFY(!controller->statesTimerActive());
    FakeSurface *pet = surface(u"p1"_s);
    const int applied = pet->applied;
    send(pet, grab());
    send(pet, drag(30));
    send(pet, release());
    click(pet);
    controller->tickStates();
    QCOMPARE(pet->applied, applied);
    QCOMPARE(pet->view.file, original);
    QVERIFY(!controller->statesTimerActive());
}

void TstOverlayController::theFileAndTheSizeFollowTheStateOfTheDrag()
{
    statesPet(R"({"grabbed":{"variant":"grab"},"moving":{"variant":"move"},"released":{"variant":"rel"}})");
    auto controller = statesController();
    connectDaemon(*controller);
    FakeSurface *pet = surface(u"p1"_s);
    const int serial = pet->serial;
    QCOMPARE(pet->view.file, fileOf(u"idle"_s)); // idle has no entry: the placement's variant
    QCOMPARE(pet->view.native, QSize(400, 300));
    QJsonObject o = controller->describe().at(0).toObject();
    QCOMPARE(o.value(u"state"_s).toString(), u"idle"_s);
    QCOMPARE(o.value(u"stateSource"_s).toString(), u"variant"_s);
    QVERIFY(!controller->statesTimerActive()); // nothing is due: no timer runs for a pet at rest

    stateClock = 1000;
    send(pet, grab());
    QCOMPARE(pet->view.file, fileOf(u"grab"_s));
    QCOMPARE(pet->view.native, QSize(300, 300)); // the shown width is the placement's; the height follows this animation (the window does the sums)
    QCOMPARE(pet->view.placement.width, 200.0);
    QCOMPARE(pet->view.placement.x, 10.0);        // and the pet's place is untouched
    QCOMPARE(controller->describe().at(0).toObject().value(u"state"_s).toString(), u"grabbed"_s);
    QVERIFY(!controller->statesTimerActive()); // held still: nothing is due until it moves
    stateClock = 1010;
    send(pet, drag(3)); // under 6 px
    QCOMPARE(pet->view.file, fileOf(u"grab"_s));
    send(pet, drag(4));
    QCOMPARE(pet->view.file, fileOf(u"move"_s));
    QVERIFY(controller->statesTimerActive()); // and now the rest after 300 ms is due
    QCOMPARE(pet->view.native, QSize(200, 100));
    QCOMPARE(controller->describe().at(0).toObject().value(u"state"_s).toString(), u"moving"_s);
    // 300 ms without motion, the button still down: grabbed again
    stateClock = 1309;
    controller->tickStates();
    QCOMPARE(pet->view.file, fileOf(u"move"_s));
    stateClock = 1310;
    controller->tickStates();
    QCOMPARE(pet->view.file, fileOf(u"grab"_s));
    // release: the released animation for one pass of it (10 frames at 30 fps: 333 ms), then idle
    stateClock = 2000;
    send(pet, release());
    QCOMPARE(pet->view.file, fileOf(u"rel"_s));
    stateClock = 2332;
    controller->tickStates();
    QCOMPARE(pet->view.file, fileOf(u"rel"_s));
    stateClock = 2333;
    controller->tickStates();
    QCOMPARE(pet->view.file, fileOf(u"idle"_s));
    QCOMPARE(controller->describe().at(0).toObject().value(u"state"_s).toString(), u"idle"_s);
    QCOMPARE(pet->serial, serial); // one window the whole time
    QCOMPARE(controller->surfaceCount(), 1);
}

void TstOverlayController::theWindowKeepsItsPictureWhileTheNewFileLoadsAndSlowSwitchesAreNamed()
{
    statesPet(R"({"grabbed":{"variant":"grab"}})");
    auto controller = statesController();
    connectDaemon(*controller);
    FakeSurface *pet = surface(u"p1"_s);
    pet->m_callbacks.started(); // the idle file plays
    QCOMPARE(controller->describe().at(0).toObject().value(u"file"_s).toString(), fileOf(u"idle"_s));
    stateClock = 1000;
    send(pet, grab());
    // The window is handed the new file and goes on showing what it has until that file has a first frame: it is the same window, never dropped or blanked.
    QCOMPARE(pet->view.file, fileOf(u"grab"_s));
    QCOMPARE(controller->surfaceCount(), 1);
    QCOMPARE(surface(u"p1"_s), pet);
    QVERIFY(pet->view.original == fileOf(u"grab"_s));
    QVERIFY(!pet->view.paused);
    // a fast switch is not named
    stateClock = 1050;
    pet->m_callbacks.started();
    QVERIFY(!controller->describe().at(0).toObject().value(u"degraded"_s).toArray().contains(QString::fromLatin1(statesdegrade::kSwitchingSlow)));
    // a slow one is, until the next switch is fast
    stateClock = 2000;
    send(pet, release());
    stateClock = 2500; // the first frame came 500 ms after the file was handed over
    pet->m_callbacks.started();
    QVERIFY(controller->describe().at(0).toObject().value(u"degraded"_s).toArray().contains(QString::fromLatin1(statesdegrade::kSwitchingSlow)));
    stateClock = 3000;
    send(pet, grab());
    stateClock = 3020;
    pet->m_callbacks.started();
    QVERIFY(!controller->describe().at(0).toObject().value(u"degraded"_s).toArray().contains(QString::fromLatin1(statesdegrade::kSwitchingSlow)));
}

void TstOverlayController::aSequenceAdvancesOnTheClockAndDroppingTheStatesStopsTheTimer()
{
    statesPet(R"({"idle":{"sequence":{"items":["idle","grab","move"],"holdMs":1000}}})");
    auto controller = statesController();
    connectDaemon(*controller);
    FakeSurface *pet = surface(u"p1"_s);
    QCOMPARE(pet->view.file, fileOf(u"idle"_s));
    QCOMPARE(controller->describe().at(0).toObject().value(u"stateSource"_s).toString(), u"sequence:0"_s);
    QVERIFY(controller->statesTimerActive());
    stateClock = 999;
    controller->tickStates();
    QCOMPARE(pet->view.file, fileOf(u"idle"_s));
    stateClock = 1000;
    controller->tickStates();
    QCOMPARE(pet->view.file, fileOf(u"grab"_s));
    QCOMPARE(controller->describe().at(0).toObject().value(u"stateSource"_s).toString(), u"sequence:1"_s);
    stateClock = 2000;
    controller->tickStates();
    QCOMPARE(pet->view.file, fileOf(u"move"_s));
    // the user clears the states: the pet shows its variant again, nothing is scheduled, nothing is in Windows()
    daemon.pets = QJsonArray{placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s)};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_COMPARE(surface(u"p1"_s)->view.file, fileOf(u"idle"_s));
    QVERIFY(!controller->statesTimerActive());
    QVERIFY(!controller->describe().at(0).toObject().contains(u"state"_s));
}

void TstOverlayController::aClickMovesAClickSequenceOn()
{
    statesPet(R"({"idle":{"sequence":{"items":["idle","grab","move"],"advance":"click"}}})");
    auto controller = statesController();
    connectDaemon(*controller);
    FakeSurface *pet = surface(u"p1"_s);
    QCOMPARE(pet->view.file, fileOf(u"idle"_s));
    QVERIFY(!controller->statesTimerActive()); // nothing advances by itself
    stateClock = 5000;
    click(pet);
    QCOMPARE(pet->view.file, fileOf(u"grab"_s));
    click(pet);
    QCOMPARE(pet->view.file, fileOf(u"move"_s));
    stateClock = 100000;
    controller->tickStates();
    QCOMPARE(pet->view.file, fileOf(u"move"_s));
}

void TstOverlayController::arrangingPausingAndHidingPutThePetBackToIdle()
{
    statesPet(R"({"grabbed":{"variant":"grab"}})");
    auto controller = statesController();
    connectDaemon(*controller);
    FakeSurface *pet = surface(u"p1"_s);
    stateClock = 100;
    send(pet, grab());
    QCOMPARE(pet->view.file, fileOf(u"grab"_s));
    // arrange mode: states are off, the pet shows idle
    daemon.arrange = true;
    daemon.emitSignal(u"PetsArrangeChanged"_s, {true});
    QTRY_COMPARE(surface(u"p1"_s)->view.file, fileOf(u"idle"_s));
    QCOMPARE(controller->describe().at(0).toObject().value(u"state"_s).toString(), u"idle"_s);
    daemon.arrange = false;
    daemon.emitSignal(u"PetsArrangeChanged"_s, {false});
    QTRY_VERIFY(!surface(u"p1"_s)->view.arrange);
    // the session locks while the pet is held
    stateClock = 200;
    send(pet, grab());
    QCOMPARE(pet->view.file, fileOf(u"grab"_s));
    controller->setSessionPaused(true);
    QCOMPARE(pet->view.file, fileOf(u"idle"_s));
    controller->setSessionPaused(false);
    // all the pets are hidden
    stateClock = 300;
    send(pet, release());
    send(pet, grab());
    QCOMPARE(pet->view.file, fileOf(u"grab"_s));
    daemon.petsHidden = true;
    daemon.emitSignal(u"PetsHiddenChanged"_s, {true});
    QTRY_VERIFY(surface(u"p1"_s)->view.hidden);
    QCOMPARE(surface(u"p1"_s)->view.file, fileOf(u"idle"_s));
}

void TstOverlayController::aDodgeStepShowsTheMovingAnimationForItsLength()
{
    statesPet(R"({"moving":{"variant":"move"}})");
    auto controller = statesController();
    connectDaemon(*controller);
    FakeSurface *pet = surface(u"p1"_s);
    stateClock = 100;
    send(pet, dodge(220));
    QCOMPARE(pet->view.file, fileOf(u"move"_s));
    stateClock = 319;
    controller->tickStates();
    QCOMPARE(pet->view.file, fileOf(u"move"_s));
    stateClock = 320;
    controller->tickStates();
    QCOMPARE(pet->view.file, fileOf(u"idle"_s));
}

void TstOverlayController::missingVariantsPendingCopiesAndTheBudgetAreNamed()
{
    // a state that names a variant the item lacks falls back, and the pet says so
    statesPet(R"({"grabbed":{"variant":"ghost"},"released":{"variant":"rel"}})", {u"rel"_s});
    auto controller = statesController();
    connectDaemon(*controller);
    FakeSurface *pet = surface(u"p1"_s);
    stateClock = 10;
    send(pet, grab());
    QCOMPARE(pet->view.file, fileOf(u"idle"_s));
    QJsonArray degraded = controller->describe().at(0).toObject().value(u"degraded"_s).toArray();
    QVERIFY(degraded.contains(QString::fromLatin1(statesdegrade::kMissingVariant)));
    // a variant of a state that is being converted: named, and it plays its original meanwhile
    QVERIFY(degraded.contains(QString::fromLatin1(statesdegrade::kTranscodePending)));
    send(pet, release());
    QCOMPARE(pet->view.file, fileOf(u"rel"_s));
    // the frame cache: one set of frames at a time, never more than the budget; a state whose frames do not fit plays from the video, and says so
    controller->setFrameCacheBudget(1'300'000); // 400x300 at 10 frames shown 200 wide: 1.2 MB
    stateClock = 5000;
    controller->tickStates();
    QCOMPARE(pet->view.file, fileOf(u"idle"_s));
    QVERIFY(pet->view.spriteSize.isValid());
    QVERIFY(pet->view.spriteSize.width() * pet->view.spriteSize.height() * 4 * 10 <= 1'300'000);
    QVERIFY(!controller->describe().at(0).toObject().value(u"degraded"_s).toArray().contains(QString::fromLatin1(statesdegrade::kOverBudget)));
    controller->setFrameCacheBudget(500'000);
    QVERIFY(!surface(u"p1"_s)->view.spriteSize.isValid());
    QVERIFY(controller->describe().at(0).toObject().value(u"degraded"_s).toArray().contains(QString::fromLatin1(statesdegrade::kOverBudget)));
    controller->setFrameCacheBudget(0);
    QVERIFY(!controller->describe().at(0).toObject().value(u"degraded"_s).toArray().contains(QString::fromLatin1(statesdegrade::kOverBudget)));
}

void TstOverlayController::aVariantTheRuntimeLayerSetsIsNotOverriddenByIdle()
{
    statesPet(R"({"idle":{"variant":"move"},"grabbed":{"variant":"grab"}})");
    auto controller = statesController();
    connectDaemon(*controller);
    FakeSurface *pet = surface(u"p1"_s);
    QCOMPARE(pet->view.file, fileOf(u"move"_s)); // idle names move
    daemon.runtime = u"{\"p1\": {\"variant\": \"rel\"}}"_s;
    daemon.emitSignal(u"PetRuntimeChanged"_s);
    QTRY_COMPARE(surface(u"p1"_s)->view.file, fileOf(u"rel"_s)); // a timeline chose an animation: it is shown as long as the layer holds it
    daemon.runtime = u"{}"_s;
    daemon.emitSignal(u"PetRuntimeChanged"_s);
    QTRY_COMPARE(surface(u"p1"_s)->view.file, fileOf(u"move"_s));
}

void TstOverlayController::lookMirrorsThePetTowardsTheCursorAndRunsThePointerSourceOnlyWhileItShould()
{
    QJsonObject p = placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s); // at (10, 20), 200 wide, 150 high: centre (110, 95)
    p.insert(u"tracking"_s, QJsonObject{{u"mode"_s, u"look"_s}});
    daemon.pets = QJsonArray{p, placement(u"p2"_s, u"anima:1"_s, u"DP-1"_s)};
    ManualPointerSource source(PointerSource::Level::Global);
    auto controller = statesController();
    controller->setPointerSource(&source);
    connectDaemon(*controller);
    QVERIFY(source.isWanted()); // a pet that looks needs the cursor wherever it is
    QVERIFY(!surface(u"p1"_s)->view.placement.flipH);
    QJsonObject o = controller->describe().at(0).toObject();
    QCOMPARE(o.value(u"tracking"_s).toString(), u"look"_s);
    QVERIFY(o.value(u"trackingActive"_s).toBool());
    QVERIFY(!controller->describe().at(1).toObject().contains(u"tracking"_s));
    stateClock = 1000;
    source.moveTo({900, 95});
    QVERIFY(!surface(u"p1"_s)->view.placement.flipH); // the item faces right, the cursor is on the right
    stateClock = 2000;
    source.moveTo({-300, 95});
    QVERIFY(surface(u"p1"_s)->view.placement.flipH);
    QVERIFY(!surface(u"p2"_s)->view.placement.flipH);
    QCOMPARE(surface(u"p1"_s)->pointers.size(), 0); // (the window's own pointer feed is for hide, fade and dodge)
    // a flip held back by the rate limit comes with the clock
    stateClock = 2100;
    source.moveTo({900, 95});
    QVERIFY(surface(u"p1"_s)->view.placement.flipH);
    stateClock = 2250;
    controller->tickStates();
    QVERIFY(!surface(u"p1"_s)->view.placement.flipH);
    // the placement's own flip is kept and turned over
    p.insert(u"flipH"_s, true);
    daemon.pets = QJsonArray{p};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_VERIFY(surface(u"p1"_s)->view.placement.flipH);
    stateClock = 5000;
    source.moveTo({-300, 95});
    QVERIFY(!surface(u"p1"_s)->view.placement.flipH);
    // the last pet that tracks goes: the source stops
    daemon.pets = QJsonArray{placement(u"p2"_s, u"anima:1"_s, u"DP-1"_s)};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_VERIFY(!source.isWanted());
    // tracking turned off by the user (or the rescue) does the same
    p.insert(u"tracking"_s, QJsonObject{{u"mode"_s, u"look"_s}});
    daemon.pets = QJsonArray{p};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_VERIFY(source.isWanted());
    p.insert(u"tracking"_s, QJsonObject{{u"mode"_s, u"off"_s}});
    daemon.pets = QJsonArray{p};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_VERIFY(!source.isWanted());
}

void TstOverlayController::trackingNeedsTheGlobalPointerAndSaysSo()
{
    QJsonObject p = placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s);
    p.insert(u"tracking"_s, QJsonObject{{u"mode"_s, u"look"_s}});
    daemon.pets = QJsonArray{p};
    // no pointer source at all, and a local one: off for that pet, with the name
    {
        auto controller = statesController();
        connectDaemon(*controller);
        const QJsonObject o = controller->describe().at(0).toObject();
        QVERIFY(!o.value(u"trackingActive"_s).toBool());
        QVERIFY(o.value(u"degraded"_s).toArray().contains(QString::fromLatin1(statesdegrade::kNeedsGlobalPointer)));
    }
    surfaces.clear();
    {
        ManualPointerSource local(PointerSource::Level::Local);
        auto controller = statesController();
        controller->setPointerSource(&local);
        connectDaemon(*controller);
        QVERIFY(!local.isWanted());
        QVERIFY(controller->describe().at(0).toObject().value(u"degraded"_s).toArray().contains(QString::fromLatin1(statesdegrade::kNeedsGlobalPointer)));
    }
    // a global source that fails: the session becomes local and the pet stops tracking, saying why
    surfaces.clear();
    ManualPointerSource source(PointerSource::Level::Global);
    auto controller = statesController();
    controller->setPointerSource(&source);
    connectDaemon(*controller);
    QVERIFY(source.isWanted());
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"the global pointer is unavailable"_s));
    emit source.unavailable(u"KWin refused the script"_s);
    QVERIFY(!source.isWanted());
    const QJsonObject o = controller->describe().at(0).toObject();
    QVERIFY(!o.value(u"trackingActive"_s).toBool());
    QVERIFY(o.value(u"degraded"_s).toArray().contains(QString::fromLatin1(statesdegrade::kNeedsGlobalPointer)));
}

void TstOverlayController::aTimelinesTrackActionTurnsTrackingOnInTheLayerAndTheEndTurnsItOff()
{
    // the pet's stored tracking is off; the runtime layer's mode (a timeline's `track` action) makes it look at the cursor while the layer holds it
    QJsonObject p = placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s);
    daemon.pets = QJsonArray{p};
    ManualPointerSource source(PointerSource::Level::Global);
    auto controller = statesController();
    controller->setPointerSource(&source);
    connectDaemon(*controller);
    QVERIFY(!source.isWanted());
    QVERIFY(!controller->describe().at(0).toObject().contains(u"tracking"_s));
    daemon.runtime = u"{\"p1\": {\"tracking\": \"look\"}}"_s;
    daemon.emitSignal(u"PetRuntimeChanged"_s);
    QTRY_VERIFY(source.isWanted()); // the pet needs the cursor now
    QJsonObject o = controller->describe().at(0).toObject();
    QCOMPARE(o.value(u"tracking"_s).toString(), u"look"_s);
    QVERIFY(o.value(u"trackingActive"_s).toBool());
    stateClock = 1000;
    source.moveTo({-300, 95});
    QVERIFY(surface(u"p1"_s)->view.placement.flipH); // it turned to the cursor
    // the layer is cleared (the run ended, the rescue): the stored mode, off, is back and the cursor is no longer wanted
    daemon.runtime = u"{}"_s;
    daemon.emitSignal(u"PetRuntimeChanged"_s);
    QTRY_VERIFY(!source.isWanted());
    QVERIFY(!controller->describe().at(0).toObject().contains(u"tracking"_s));
    QVERIFY(!surface(u"p1"_s)->view.placement.flipH);
}

void TstOverlayController::hidingPausingAndArrangingStopTheTracking()
{
    QJsonObject p = placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s);
    p.insert(u"tracking"_s, QJsonObject{{u"mode"_s, u"look"_s}});
    daemon.pets = QJsonArray{p};
    ManualPointerSource source(PointerSource::Level::Global);
    auto controller = statesController();
    controller->setPointerSource(&source);
    connectDaemon(*controller);
    QVERIFY(source.isWanted());
    stateClock = 100;
    source.moveTo({-300, 95});
    QVERIFY(surface(u"p1"_s)->view.placement.flipH);
    controller->setSessionPaused(true);
    QVERIFY(!source.isWanted());
    QVERIFY(!surface(u"p1"_s)->view.placement.flipH);
    controller->setSessionPaused(false);
    QVERIFY(source.isWanted());
    daemon.petsHidden = true;
    daemon.emitSignal(u"PetsHiddenChanged"_s, {true});
    QTRY_VERIFY(!source.isWanted());
    daemon.petsHidden = false;
    daemon.emitSignal(u"PetsHiddenChanged"_s, {false});
    QTRY_VERIFY(source.isWanted());
    daemon.arrange = true;
    daemon.emitSignal(u"PetsArrangeChanged"_s, {true});
    QTRY_VERIFY(!source.isWanted());
    daemon.arrange = false;
    daemon.emitSignal(u"PetsArrangeChanged"_s, {false});
    QTRY_VERIFY(source.isWanted());
    // a paused pet
    p.insert(u"paused"_s, true);
    daemon.pets = QJsonArray{p};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_VERIFY(!source.isWanted());
}

namespace {
// Moves the cursor to `to` every 33 ms for `ms` of the controller's clock.
void drive(ManualPointerSource &source, qint64 *clock, QPointF to, int ms)
{
    for (int t = 0; t < ms; t += 33) {
        *clock += 33;
        source.moveTo(to);
    }
}
QJsonObject follower(QJsonObject p, double distance = 160, bool returnHome = true)
{
    p.insert(u"tracking"_s, QJsonObject{{u"mode"_s, u"follow"_s}, {u"distance"_s, distance}, {u"returnHome"_s, returnHome}});
    return p;
}
} // namespace

void TstOverlayController::followWalksTheWindowTowardsTheCursorAndNeverWritesTheStore()
{
    daemon.pets = QJsonArray{follower(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s))}; // at (10, 20), 200 x 150: centre (110, 95)
    ManualPointerSource source(PointerSource::Level::Global);
    auto controller = statesController();
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    controller->setPointerSource(&source);
    connectDaemon(*controller);
    QVERIFY(source.isWanted());
    FakeSurface *pet = surface(u"p1"_s);
    QCOMPARE(pet->view.placement.x, 10.0);
    const int calls = daemon.callCount(u"UpdatePet"_s);
    double last = pet->view.placement.x;
    for (int i = 0; i < 240; ++i) {
        drive(source, &stateClock, {1000, 95}, 33);
        QVERIFY(pet->view.placement.x >= last);
        last = pet->view.placement.x;
        QCOMPARE(pet->view.placement.y, 20.0);
    }
    QVERIFY2(std::abs(pet->view.placement.x + 100 - (1000 - 160)) <= 1.5, qPrintable(QString::number(pet->view.placement.x))); // centre 160 px from the cursor
    QCOMPARE(daemon.callCount(u"UpdatePet"_s), calls); // a follower writes nothing, however far it goes
    QVERIFY(controller->describe().at(0).toObject().value(u"trackingActive"_s).toBool());
    // the stored place is still the old one in the daemon's list
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_COMPARE(surface(u"p1"_s)->view.placement.x, last);
    // its output is the limit: a cursor beyond it parks the pet at the edge
    drive(source, &stateClock, {6000, 95}, 8000);
    QVERIFY(surface(u"p1"_s)->view.placement.x + 200 <= 1920 + 1e-6);
    QVERIFY(surface(u"p1"_s)->view.placement.x + 200 >= 1919);
    QCOMPARE(daemon.callCount(u"UpdatePet"_s), calls);
}

void TstOverlayController::aFollowOfTheRuntimeLayerNeverStoresAPlaceEvenWithReturnHomeOff()
{
    // the pet once followed with "stay where it ends" and was turned off: off, returnHome false. A timeline's `track follow` must still not move it for good.
    QJsonObject p = placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s);
    p.insert(u"tracking"_s, QJsonObject{{u"mode"_s, u"off"_s}, {u"returnHome"_s, false}});
    daemon.pets = QJsonArray{p};
    ManualPointerSource source(PointerSource::Level::Global);
    auto controller = statesController();
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    controller->setPointerSource(&source);
    connectDaemon(*controller);
    const int calls = daemon.callCount(u"UpdatePet"_s);
    daemon.runtime = u"{\"p1\": {\"tracking\": \"follow\"}}"_s;
    daemon.emitSignal(u"PetRuntimeChanged"_s);
    QTRY_VERIFY(source.isWanted());
    drive(source, &stateClock, {1000, 95}, 4000);
    QVERIFY(surface(u"p1"_s)->view.placement.x > 500);
    daemon.runtime = u"{}"_s;
    daemon.emitSignal(u"PetRuntimeChanged"_s);
    QTest::qWait(100); // (the daemon is asked for the layer again)
    for (int i = 0; i < 400 && surface(u"p1"_s)->view.placement.x != 10.0; ++i) {
        stateClock += 33;
        controller->tickStates();
    }
    QCOMPARE(surface(u"p1"_s)->view.placement.x, 10.0); // it glided home
    QCOMPARE(daemon.callCount(u"UpdatePet"_s), calls); // nothing was written
}

void TstOverlayController::whenFollowingStopsTheSetOffPetGoesHomeOrIsStoredOnce()
{
    // returnHome (the default): off sends it back, nothing is written
    QJsonObject p = follower(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s));
    daemon.pets = QJsonArray{p};
    ManualPointerSource source(PointerSource::Level::Global);
    auto controller = statesController();
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    controller->setPointerSource(&source);
    connectDaemon(*controller);
    drive(source, &stateClock, {1000, 95}, 4000);
    QVERIFY(surface(u"p1"_s)->view.placement.x > 500);
    const int calls = daemon.callCount(u"UpdatePet"_s);
    p.insert(u"tracking"_s, QJsonObject{{u"mode"_s, u"off"_s}});
    daemon.pets = QJsonArray{p};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_VERIFY(!controller->describe().at(0).toObject().contains(u"tracking"_s)); // (off with the defaults is no tracking at all)
    QVERIFY(surface(u"p1"_s)->view.placement.x > 500);
    QVERIFY(controller->statesTimerActive()); // it is on its way home
    for (int i = 0; i < 400 && surface(u"p1"_s)->view.placement.x != 10.0; ++i) {
        stateClock += 33;
        controller->tickStates();
    }
    QCOMPARE(surface(u"p1"_s)->view.placement.x, 10.0);
    QCOMPARE(daemon.callCount(u"UpdatePet"_s), calls);
    QVERIFY(!controller->statesTimerActive());
    QVERIFY(!source.isWanted());
    // without returnHome it stays and the place is stored once
    surfaces.clear();
    daemon.pets = QJsonArray{follower(placement(u"p2"_s, u"anima:1"_s, u"DP-1"_s), 160, false)};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_VERIFY(surface(u"p2"_s));
    drive(source, &stateClock, {1000, 95}, 8000);
    const double where = surface(u"p2"_s)->view.placement.x;
    QVERIFY(where > 500);
    QJsonObject off = placement(u"p2"_s, u"anima:1"_s, u"DP-1"_s);
    off.insert(u"tracking"_s, QJsonObject{{u"mode"_s, u"off"_s}, {u"returnHome"_s, false}});
    daemon.pets = QJsonArray{off};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_COMPARE(daemon.callCount(u"UpdatePet"_s), calls + 1);
    const QJsonObject sent = QJsonDocument::fromJson(daemon.lastArgs(u"UpdatePet"_s).at(1).toString().toUtf8()).object();
    QCOMPARE(sent.value(u"x"_s).toDouble(), std::round(where));
    QCOMPARE(sent.value(u"y"_s).toDouble(), 20.0);
    // the daemon has not answered yet: the pet does not jump back; then it says where it is
    QCOMPARE(surface(u"p2"_s)->view.placement.x, std::round(where)); // (the place is held rounded, as it is stored)
    off.insert(u"x"_s, std::round(where));
    daemon.pets = QJsonArray{off};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_COMPARE(surface(u"p2"_s)->view.placement.x, std::round(where));
    stateClock += 5000;
    controller->tickStates();
    QCOMPARE(surface(u"p2"_s)->view.placement.x, std::round(where));
    QCOMPARE(daemon.callCount(u"UpdatePet"_s), calls + 1);
}

void TstOverlayController::aFollowerOnAnOutputWithAnOriginSeesTheCursorInItsOwnSpace()
{
    // the pet is on the second output, which starts at x 1920 of the compositor's layout: the cursor is global, the pet's place is the output's
    daemon.pets = QJsonArray{follower(placement(u"p1"_s, u"anima:1"_s, u"HDMI-A-1"_s), 100)};
    ManualPointerSource source(PointerSource::Level::Global);
    auto controller = statesController();
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    controller->setPointerSource(&source);
    connectDaemon(*controller);
    drive(source, &stateClock, {1920 + 700, 95}, 9000);
    // centre 100 px left of the cursor, in the output's own coordinates: 700 - 100 = 600, so x = 500
    QVERIFY2(std::abs(surface(u"p1"_s)->view.placement.x - 500) <= 1.5, qPrintable(QString::number(surface(u"p1"_s)->view.placement.x)));
    // a cursor on the other output pulls it to the nearest edge of its own and no further (the output is 1280 wide)
    drive(source, &stateClock, {-4000, 95}, 20000);
    QVERIFY(surface(u"p1"_s)->view.placement.x >= -1e-9);
    QVERIFY(surface(u"p1"_s)->view.placement.x < 1.5);
}

void TstOverlayController::aStageFollowerMovesItsPictureInsideItsScreenAndNothingElse()
{
    // The picture of a stage (400x300 contained in 1920x1080: 1440x1080 at x 240) walks inside the screen; the window never moves and nothing is stored
    QJsonObject p = follower(asStage(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), u"output"_s), 0, false); // (a stage always goes back: no place of its own to keep)
    p.insert(u"locked"_s, true);                                                                        // (and it is locked by its nature)
    daemon.pets = QJsonArray{p};
    ManualPointerSource source(PointerSource::Level::Global);
    auto controller = statesController();
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    controller->setPointerSource(&source);
    connectDaemon(*controller);
    FakeSurface *pet = surface(u"p1"_s);
    QVERIFY(pet->view.stage.active);
    QCOMPARE(pet->view.stage.petRect, QRectF(240, 0, 1440, 1080));
    const int calls = daemon.callCount(u"UpdatePet"_s);
    drive(source, &stateClock, {1900, 500}, 12000);
    QVERIFY2(std::abs(pet->view.stage.petRect.x() - 480) <= 1.0, qPrintable(QString::number(pet->view.stage.petRect.x()))); // against the right edge
    QCOMPARE(pet->view.stage.petRect.y(), 0.0);
    QCOMPARE(pet->view.stage.petRect.width(), 1440.0);
    QCOMPARE(pet->view.placement.x, 10.0); // the placement's own place is not touched
    drive(source, &stateClock, {10, 500}, 20000);
    QVERIFY(std::abs(pet->view.stage.petRect.x()) <= 1.0);
    QCOMPARE(daemon.callCount(u"UpdatePet"_s), calls);
    // turned off: the picture goes back to the middle and nothing is stored, whatever returnHome says
    QJsonObject off = asStage(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), u"output"_s);
    off.insert(u"tracking"_s, QJsonObject{{u"mode"_s, u"off"_s}, {u"returnHome"_s, false}});
    daemon.pets = QJsonArray{off};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_VERIFY(controller->statesTimerActive());
    for (int i = 0; i < 600 && std::abs(pet->view.stage.petRect.x() - 240) > 0.5; ++i) {
        stateClock += 33;
        controller->tickStates();
    }
    QCOMPARE(pet->view.stage.petRect.x(), 240.0);
    QCOMPARE(daemon.callCount(u"UpdatePet"_s), calls);
}

void TstOverlayController::aDesktopStageLooksButDoesNotWalk()
{
    QJsonObject p = follower(asStage(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), u"desktop"_s), 0);
    daemon.pets = QJsonArray{p};
    ManualPointerSource source(PointerSource::Level::Global);
    auto controller = statesController();
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    controller->setPointerSource(&source);
    connectDaemon(*controller);
    FakeSurface *pet = surface(u"p1"_s);
    const QRectF rect = pet->view.stage.petRect;
    drive(source, &stateClock, {1900, 500}, 3000);
    QCOMPARE(pet->view.stage.petRect, rect); // one picture across the screens has nowhere to walk
    QVERIFY(controller->describe().at(0).toObject().value(u"degraded"_s).toArray().contains(QString::fromLatin1(statesdegrade::kStageFollowOneScreen)));
    // look still turns it
    QJsonObject look = asStage(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), u"desktop"_s);
    look.insert(u"tracking"_s, QJsonObject{{u"mode"_s, u"look"_s}});
    daemon.pets = QJsonArray{look};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_VERIFY(controller->describe().at(0).toObject().value(u"tracking"_s).toString() == u"look"_s);
    stateClock += 1000;
    source.moveTo({-300, 500});
    QVERIFY(surface(u"p1"_s)->view.placement.flipH);
}

void TstOverlayController::aDesktopStageLooksInTheSpaceOfItsPicture()
{
    // The main screen is the second one (origin 1920): the first slice of the picture is the other screen's. The cursor and the picture must be in one space.
    // 400x300 contained in the 3200x1080 desktop: 1440x1080 from x 880, so its middle is at x 1600 of the layout.
    daemon.pets = QJsonArray{asStage(placement(u"p1"_s, u"anima:1"_s, u"*"_s), u"desktop"_s)};
    QJsonObject p = daemon.pets.at(0).toObject();
    p.insert(u"tracking"_s, QJsonObject{{u"mode"_s, u"look"_s}});
    daemon.pets = QJsonArray{p};
    ManualPointerSource source(PointerSource::Level::Global);
    auto controller = statesController();
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"HDMI-A-1"_s, {}, twoOutputs());
    controller->setPointerSource(&source);
    connectDaemon(*controller);
    FakeSurface *pet = surfaceOn(u"p1"_s, u"DP-1"_s);
    QVERIFY(pet && pet->view.stage.active);
    stateClock = 1000;
    source.moveTo({1700, 500}); // right of the middle of the picture: the item faces right, no mirror
    QVERIFY(!pet->view.placement.flipH);
    stateClock = 2000;
    source.moveTo({1500, 500}); // left of it: mirrored
    QVERIFY(pet->view.placement.flipH);
    stateClock = 3000;
    source.moveTo({3000, 500});
    QVERIFY(!pet->view.placement.flipH);
}

void TstOverlayController::aStagePictureBiggerThanItsScreenStaysWhereItIs()
{
    // `cover` fills the screen and cuts the excess: the picture is bigger than the screen and has nowhere to walk; it must not jump to one side
    QJsonObject p = follower(asStage(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), u"output"_s, u"cover"_s), 0);
    daemon.pets = QJsonArray{p};
    ManualPointerSource source(PointerSource::Level::Global);
    auto controller = statesController();
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    controller->setPointerSource(&source);
    connectDaemon(*controller);
    FakeSurface *pet = surface(u"p1"_s);
    const QRectF rect = pet->view.stage.petRect;
    QVERIFY(rect.height() > 1080 || rect.width() > 1920);
    drive(source, &stateClock, {1900, 900}, 4000);
    drive(source, &stateClock, {20, 100}, 4000);
    QCOMPARE(pet->view.stage.petRect, rect);
}

void TstOverlayController::aFollowerIsInTheMovingStateWhileItWalks()
{
    statesPet(R"({"moving":{"variant":"move"}})");
    daemon.pets = QJsonArray{follower(daemon.pets.at(0).toObject())};
    ManualPointerSource source(PointerSource::Level::Global);
    auto controller = statesController();
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    controller->setPointerSource(&source);
    connectDaemon(*controller);
    FakeSurface *pet = surface(u"p1"_s);
    QCOMPARE(pet->view.file, fileOf(u"idle"_s));
    drive(source, &stateClock, {1000, 95}, 400);
    QCOMPARE(pet->view.file, fileOf(u"move"_s));
    QCOMPARE(controller->describe().at(0).toObject().value(u"state"_s).toString(), u"moving"_s);
    QVERIFY(controller->describe().at(0).toObject().value(u"degraded"_s).toArray().contains(QString::fromLatin1(statesdegrade::kMovesLimited)));
    drive(source, &stateClock, {1000, 95}, 12000); // arrived
    stateClock += 500;
    controller->tickStates();
    QCOMPARE(pet->view.file, fileOf(u"idle"_s));
    QCOMPARE(controller->describe().at(0).toObject().value(u"state"_s).toString(), u"idle"_s);
}

void TstOverlayController::aPreviewShowsAStateAndNeedsAShownPetWithStates()
{
    statesPet(R"({"grabbed":{"variant":"grab"},"released":{"variant":"rel"}})");
    auto controller = statesController();
    connectDaemon(*controller);
    FakeSurface *pet = surface(u"p1"_s);
    QVERIFY(!controller->previewState(u"nope"_s, u"grabbed"_s));
    QVERIFY(!controller->previewState(u"p1"_s, u"sleeping"_s));
    stateClock = 100;
    QVERIFY(controller->previewState(u"p1"_s, u"grabbed"_s));
    QCOMPARE(pet->view.file, fileOf(u"grab"_s));
    QCOMPARE(controller->describe().at(0).toObject().value(u"state"_s).toString(), u"grabbed"_s);
    QVERIFY(controller->statesTimerActive());
    stateClock = 3099;
    controller->tickStates();
    QCOMPARE(pet->view.file, fileOf(u"grab"_s));
    stateClock = 3100;
    controller->tickStates();
    QCOMPARE(pet->view.file, fileOf(u"idle"_s));
    // a pet that is arranged, paused or hidden shows nothing to try; one without states has nothing to show
    controller->setSessionPaused(true);
    QVERIFY(!controller->previewState(u"p1"_s, u"grabbed"_s));
    controller->setSessionPaused(false);
    daemon.pets = QJsonArray{placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s)};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_VERIFY(!controller->describe().at(0).toObject().contains(u"state"_s));
    QVERIFY(!controller->previewState(u"p1"_s, u"grabbed"_s));
}

// The window's part for a pet on the frame cache: its own frames are in.
void framesAreIn(FakeSurface *pet, qint64 bytes = 1'200'000)
{
    pet->m_callbacks.started();
    QTRY_VERIFY(!pet->view.spriteKey.isEmpty());
    pet->m_callbacks.spriteDone(true, bytes, QString());
}

void TstOverlayController::aClickOnAOnceEntryThatIsPlayingStartsItAgain()
{
    statesPet(R"({"idle":{"sequence":{"items":["idle"],"advance":"click","once":true}}})");
    auto controller = statesController();
    connectDaemon(*controller);
    FakeSurface *pet = surface(u"p1"_s);
    pet->m_callbacks.started();
    const int token = pet->view.restartToken;
    stateClock = 100;
    controller->tickStates();
    QVERIFY(!pet->view.paused);
    stateClock = 1000; // (one pass: 10 frames at 30 fps) the last frame stays
    controller->tickStates();
    QVERIFY(pet->view.paused);
    QCOMPARE(pet->view.restartToken, token);
    click(pet);
    QVERIFY(!pet->view.paused); // it plays again, from the first frame
    QVERIFY(pet->view.restartToken != token);
    QCOMPARE(pet->view.file, fileOf(u"idle"_s)); // (the same file: nothing to load)
}

void TstOverlayController::aControllerGoneBeforeTheDaemonAnswersIsNotCalled()
{
    {
        auto controller = makeController();
        controller->start();
        daemon.availabilityChanged(true);
        // the answers (ApiVersion, Settings, ListPets ...) are queued on the daemon's side; the controller goes first
        delete controller.release(); // (without the drain the fixture's deleter does)
    }
    const int calls = daemon.calls.size();
    settle(); // nothing may call the destroyed controller: it asks the daemon for nothing more and makes no window
    QCOMPARE(daemon.calls.size(), calls);
    QVERIFY(surfaces.empty());
}

void TstOverlayController::aWindowLostMidDragTakesTheGrabWithIt()
{
    statesPet(R"({"grabbed":{"variant":"grab"}})");
    auto controller = statesController();
    connectDaemon(*controller);
    FakeSurface *pet = surface(u"p1"_s);
    stateClock = 100;
    send(pet, grab());
    QCOMPARE(pet->view.file, fileOf(u"grab"_s));
    // the output goes while the pet is held: the window is dropped, no Release will come
    controller->setOutputs({u"HDMI-A-1"_s}, u"HDMI-A-1"_s);
    QCOMPARE(controller->surfaceCount(), 0);
    QCOMPARE(controller->describe().at(0).toObject().value(u"state"_s).toString(), u"idle"_s);
    // it comes back: a new window, idle, and the next grab works
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s);
    pet = surface(u"p1"_s);
    QVERIFY(pet);
    QCOMPARE(pet->view.file, fileOf(u"idle"_s));
    stateClock = 5000;
    send(pet, grab());
    QCOMPARE(pet->view.file, fileOf(u"grab"_s));
    // the pet is moved to another output while held: the new window starts idle
    QJsonObject moved = withStates(placement(u"p1"_s, u"anima:1"_s, u"HDMI-A-1"_s), R"({"grabbed":{"variant":"grab"}})");
    daemon.pets = QJsonArray{moved};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_VERIFY(surfaceOn(u"p1"_s, u"HDMI-A-1"_s));
    QCOMPARE(surfaceOn(u"p1"_s, u"HDMI-A-1"_s)->view.file, fileOf(u"idle"_s));
    QCOMPARE(controller->describe().at(0).toObject().value(u"state"_s).toString(), u"idle"_s);
}

void TstOverlayController::aHiddenPausedOrArrangedPetWaitsForNoSequence()
{
    statesPet(R"({"idle":{"sequence":{"items":["idle","grab","move"],"holdMs":1000}}})");
    auto controller = statesController();
    connectDaemon(*controller);
    FakeSurface *pet = surface(u"p1"_s);
    QVERIFY(controller->statesTimerActive());
    // hidden: no timer, nothing advances however long it lasts
    daemon.petsHidden = true;
    daemon.emitSignal(u"PetsHiddenChanged"_s, {true});
    QTRY_VERIFY(surface(u"p1"_s)->view.hidden);
    QVERIFY(!controller->statesTimerActive());
    const QString file = pet->view.file;
    stateClock = 10000;
    controller->tickStates();
    QCOMPARE(pet->view.file, file);
    QVERIFY(!controller->statesTimerActive());
    // shown again: it goes on from where it was, counted from now (no catching up on the ten seconds)
    daemon.petsHidden = false;
    daemon.emitSignal(u"PetsHiddenChanged"_s, {false});
    QTRY_VERIFY(!surface(u"p1"_s)->view.hidden);
    QVERIFY(controller->statesTimerActive());
    stateClock = 10999;
    controller->tickStates();
    QCOMPARE(pet->view.file, file);
    stateClock = 11000;
    controller->tickStates();
    QVERIFY(pet->view.file != file);
    // the session locks, and the arrange mode: the same
    const QString second = pet->view.file;
    controller->setSessionPaused(true);
    QVERIFY(!controller->statesTimerActive());
    stateClock = 50000;
    controller->tickStates();
    QCOMPARE(pet->view.file, second);
    controller->setSessionPaused(false);
    daemon.arrange = true;
    daemon.emitSignal(u"PetsArrangeChanged"_s, {true});
    QTRY_VERIFY(surface(u"p1"_s)->view.arrange);
    QVERIFY(!controller->statesTimerActive());
}

void TstOverlayController::releasedIsCountedFromTheFirstFrameOfItsPicture()
{
    // released is one pass of its animation (10 frames at 30 fps: 333 ms); the window needs 300 ms to show the first frame
    statesPet(R"({"grabbed":{"variant":"grab"},"released":{"variant":"rel"}})");
    auto controller = statesController();
    connectDaemon(*controller);
    FakeSurface *pet = surface(u"p1"_s);
    pet->m_callbacks.started();
    stateClock = 1000;
    send(pet, grab());
    stateClock = 1050;
    pet->m_callbacks.started();
    stateClock = 2000;
    send(pet, release());
    QCOMPARE(pet->view.file, fileOf(u"rel"_s));
    stateClock = 2300;
    pet->m_callbacks.started(); // the first frame of the released picture
    stateClock = 2400;
    controller->tickStates();
    QCOMPARE(pet->view.file, fileOf(u"rel"_s)); // (counted from the order to load it, it would be over at 2333)
    stateClock = 2632;
    controller->tickStates();
    QCOMPARE(pet->view.file, fileOf(u"rel"_s));
    stateClock = 2633;
    controller->tickStates();
    QCOMPARE(pet->view.file, fileOf(u"idle"_s));
}

void TstOverlayController::aStateWhoseFileIsBlockedNeverDropsTheWindowFromItsOwnCallback()
{
    // The file of the grabbed state crashed the overlay three times: pressing the pet must not destroy the window that is still delivering the press.
    const QString stateDir = tmp.filePath(u"state/blocked"_s);
    testutil::writeFile(fileOf(u"grab"_s), "x");
    for (int run = 0; run < CrashLedger::kMaxCrashes; ++run) {
        CrashLedger crashed(stateDir);
        crashed.load();
        crashed.markOpening(u"x"_s, fileOf(u"grab"_s)); // (left behind: a crash)
    }
    CrashLedger blocked(stateDir);
    blocked.load();
    QVERIFY(blocked.blocked(fileOf(u"grab"_s)));
    ledger = &blocked;
    statesPet(R"({"grabbed":{"variant":"grab"}})");
    auto controller = statesController();
    connectDaemon(*controller);
    FakeSurface *pet = surface(u"p1"_s);
    QVERIFY(pet);
    const int serial = pet->serial;
    const auto guard = qScopeGuard([&] { ledger = nullptr; });
    pet->m_callbacks.started();
    stateClock = 100;
    pet->m_callbacks.pointerEvent(grab());       // inside the window's callback
    QCOMPARE(surface(u"p1"_s), pet);              // the window is still there
    QCoreApplication::processEvents();           // the next turn: what the machine said is applied, and the window with the blocked file goes
    // the window that held the grab is gone and a new one shows what the pet shows at rest (the grab went with the window, so the blocked file is not asked for)
    FakeSurface *now = surface(u"p1"_s);
    QVERIFY(now && now->serial != serial);
    QCOMPARE(now->view.file, fileOf(u"idle"_s));
    QCOMPARE(controller->describe().at(0).toObject().value(u"state"_s).toString(), u"idle"_s);
    ledger = nullptr;
}

void TstOverlayController::thePictureOfTheNextStateIsPreparedWithinTheBudgetAndSwappedIn()
{
    // idle 400x300 (shown 200 wide: 200x150, 10 frames: 1.2 MB), grabbed 300x300 (200x200: 1.6 MB), released falls back to idle
    statesPet(R"({"grabbed":{"variant":"grab"}})");
    auto controller = statesController();
    controller->setFrameCacheBudget(4'000'000);
    connectDaemon(*controller);
    FakeSurface *pet = surface(u"p1"_s);
    QVERIFY(pet->view.preloadKey.isEmpty()); // nothing yet: the pet's own frames are not in
    framesAreIn(pet);
    QTRY_VERIFY(!surface(u"p1"_s)->view.preloadKey.isEmpty());
    const QString grabKey = fileOf(u"grab"_s) + u"@200x200"_s;
    QCOMPARE(pet->view.preloadKey, grabKey);
    QCOMPARE(pet->view.preloadFile, fileOf(u"grab"_s));
    QCOMPARE(pet->view.preloadSize, QSize(200, 200));
    QVERIFY(pet->view.preloadBudget >= 1'600'000 && pet->view.preloadBudget <= 2'000'000);
    QCOMPARE(controller->describe().at(0).toObject().value(u"preloadBytes"_s).toDouble(), 0.0); // (asked, not ready)
    // the window has it ready
    pet->m_callbacks.preloadDone(true, 1'600'000, grabKey, QString());
    QTRY_COMPARE(controller->describe().at(0).toObject().value(u"preloadBytes"_s).toDouble(), 1'600'000.0);
    // an answer for another key is not taken for it
    pet->m_callbacks.preloadDone(true, 5, u"other"_s, QString());
    QCOMPARE(controller->describe().at(0).toObject().value(u"preloadBytes"_s).toDouble(), 1'600'000.0);
    // the pet is grabbed: the window is handed the key that was prepared (it swaps), not a new one
    stateClock = 100;
    send(pet, grab());
    QCOMPARE(pet->view.file, fileOf(u"grab"_s));
    QCOMPARE(pet->view.spriteKey, grabKey);
    QVERIFY(pet->view.preloadKey.isEmpty()); // (its own frames are being swapped in: nothing is prepared meanwhile)
    pet->m_callbacks.spriteDone(true, 1'600'000, QString()); // the swap is done
    // and what comes after a grab is prepared: released shows idle (it has no entry)
    QTRY_VERIFY(!surface(u"p1"_s)->view.preloadKey.isEmpty());
    QCOMPARE(pet->view.preloadKey, fileOf(u"idle"_s) + u"@200x150"_s);
    // the budget is never exceeded: what the frames take plus what is prepared
    pet->m_callbacks.preloadDone(true, 1'200'000, pet->view.preloadKey, QString());
    QTRY_COMPARE(controller->describe().at(0).toObject().value(u"preloadBytes"_s).toDouble(), 1'200'000.0);
    const QJsonObject o = controller->describe().at(0).toObject();
    QVERIFY(o.value(u"cacheBytes"_s).toDouble() + o.value(u"preloadBytes"_s).toDouble() <= double(controller->frameCacheBudget()));
    // released shows idle, which is what was prepared
    send(pet, release());
    QCOMPARE(pet->view.file, fileOf(u"idle"_s)); // (released falls back to idle)
}

void TstOverlayController::aPetWithoutStatesOrWithoutRoomPreparesNothing()
{
    // not enough room for both sets: the picture that plays has its place, the other is not prepared (1.2 MB + 1.6 MB do not fit in 2.7 MB)
    statesPet(R"({"grabbed":{"variant":"grab"}})");
    auto controller = statesController();
    controller->setFrameCacheBudget(2'700'000);
    connectDaemon(*controller);
    FakeSurface *pet = surface(u"p1"_s);
    framesAreIn(pet);
    settle();
    QVERIFY(pet->view.preloadKey.isEmpty());
    QVERIFY(pet->view.spriteSize.isValid());
    // room appears: it is prepared
    controller->setFrameCacheBudget(3'000'000);
    QVERIFY(!pet->view.preloadKey.isEmpty());
    // room goes: the window is told to drop it
    controller->setFrameCacheBudget(2'700'000);
    QVERIFY(pet->view.preloadKey.isEmpty());
    // no cache at all: nothing
    controller->setFrameCacheBudget(0);
    QVERIFY(pet->view.preloadKey.isEmpty() && !pet->view.spriteSize.isValid());
    // a pet without states never prepares anything, whatever the budget
    surfaces.clear();
    daemon.items[u"anima:1"_s] = animatedItem(u"anima:1"_s, original);
    daemon.pets = QJsonArray{placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s)};
    auto plain = statesController();
    plain->setFrameCacheBudget(64 << 20);
    connectDaemon(*plain);
    framesAreIn(surface(u"p1"_s));
    settle();
    QVERIFY(surface(u"p1"_s)->view.preloadKey.isEmpty());
    QVERIFY(!plain->describe().at(0).toObject().contains(u"preloadBytes"_s));
}

void TstOverlayController::aFailedPreloadIsNotRetriedAndAnotherPictureDecodesAsAlways()
{
    statesPet(R"({"grabbed":{"variant":"grab"},"released":{"variant":"rel"}})");
    auto controller = statesController();
    controller->setFrameCacheBudget(8'000'000);
    connectDaemon(*controller);
    FakeSurface *pet = surface(u"p1"_s);
    framesAreIn(pet);
    QTRY_VERIFY(!surface(u"p1"_s)->view.preloadKey.isEmpty());
    const QString key = pet->view.preloadKey;
    pet->m_callbacks.preloadDone(false, 0, key, u"cannot decode"_s);
    QTRY_VERIFY(surface(u"p1"_s)->view.preloadKey.isEmpty()); // not asked for again
    settle();
    QVERIFY(pet->view.preloadKey.isEmpty());
    // the pet goes to a picture that was not prepared: it decodes as it always did (a new key, one decode)
    stateClock = 10;
    send(pet, grab());
    QCOMPARE(pet->view.spriteKey, fileOf(u"grab"_s) + u"@200x200"_s);
    pet->m_callbacks.spriteDone(true, 1'600'000, QString());
    // released is the likely next one now; moving falls back to grabbed (the same picture) and is skipped
    QTRY_VERIFY(!surface(u"p1"_s)->view.preloadKey.isEmpty());
    QCOMPARE(pet->view.preloadKey, fileOf(u"rel"_s) + u"@200x150"_s);
    // the pet leaves the cache (the daemon says the item has no more frames): the surface is told to drop it
    controller->setFrameCacheBudget(0);
    QVERIFY(pet->view.preloadKey.isEmpty());
}

void TstOverlayController::aFollowerWhoseWindowWasLostMidDragWalksAgain()
{
    daemon.pets = QJsonArray{follower(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s))};
    ManualPointerSource source(PointerSource::Level::Global);
    auto controller = statesController();
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    controller->setPointerSource(&source);
    connectDaemon(*controller);
    drive(source, &stateClock, {1000, 95}, 600);
    send(surface(u"p1"_s), grab());
    const double held = surface(u"p1"_s)->view.placement.x;
    drive(source, &stateClock, {1000, 95}, 600);
    QCOMPARE(surface(u"p1"_s)->view.placement.x, held); // held: it stands still
    // the output goes while it is held: no Release will come; when the window is back the follower walks again
    controller->setOutputs({u"HDMI-A-1"_s}, u"HDMI-A-1"_s, {}, twoOutputs());
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    QVERIFY(surface(u"p1"_s));
    drive(source, &stateClock, {1500, 95}, 1500);
    QVERIFY(surface(u"p1"_s)->view.placement.x > held);
}

void TstOverlayController::aPlaceTheDaemonRefusedOrChangedIsNotHeldForEver()
{
    QJsonObject stay = follower(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), 160, false);
    daemon.pets = QJsonArray{stay};
    ManualPointerSource source(PointerSource::Level::Global);
    auto controller = statesController();
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    controller->setPointerSource(&source);
    connectDaemon(*controller);
    drive(source, &stateClock, {1000, 95}, 8000);
    QVERIFY(surface(u"p1"_s)->view.placement.x > 500);
    // the daemon refuses the store of where it stopped: the pet goes back to the stored place at once
    daemon.failUpdatePet = true;
    QJsonObject off = placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s);
    off.insert(u"tracking"_s, QJsonObject{{u"mode"_s, u"off"_s}, {u"returnHome"_s, false}});
    daemon.pets = QJsonArray{off};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_COMPARE(surface(u"p1"_s)->view.placement.x, 10.0);
    // a place the daemon changed (someone moved the pet meanwhile) is not held either: the stored one rules after a few seconds
    daemon.failUpdatePet = false;
    surfaces.clear();
    daemon.pets = QJsonArray{follower(placement(u"p2"_s, u"anima:1"_s, u"DP-1"_s), 160, false)};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_VERIFY(surface(u"p2"_s));
    drive(source, &stateClock, {1000, 95}, 8000);
    QJsonObject off2 = placement(u"p2"_s, u"anima:1"_s, u"DP-1"_s);
    off2.insert(u"tracking"_s, QJsonObject{{u"mode"_s, u"off"_s}, {u"returnHome"_s, false}});
    off2.insert(u"x"_s, 321.0); // the daemon stored another x than the one asked
    daemon.pets = QJsonArray{off2};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_VERIFY(daemon.callCount(u"UpdatePet"_s) >= 1);
    QTest::qWait(3200);
    QCOMPARE(surface(u"p2"_s)->view.placement.x, 321.0);
}

void TstOverlayController::aFollowerDoesNotMakeWindowsChangeThirtyTimesASecond()
{
    daemon.pets = QJsonArray{follower(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s))};
    ManualPointerSource source(PointerSource::Level::Global);
    auto controller = statesController();
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    controller->setPointerSource(&source);
    connectDaemon(*controller);
    QSignalSpy changed(controller.get(), &OverlayController::stateChanged);
    drive(source, &stateClock, {1000, 95}, 3000); // about 90 steps
    QVERIFY(surface(u"p1"_s)->view.placement.x > 300);
    QVERIFY2(changed.count() <= 6, qPrintable(QString::number(changed.count()))); // walking began, and maybe ended: the list is not a stream of positions
}

void TstOverlayController::aParkedFollowerAsksNothingOfThePointerSource()
{
    daemon.pets = QJsonArray{follower(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s))};
    ManualPointerSource source(PointerSource::Level::Global);
    auto controller = statesController();
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    controller->setPointerSource(&source);
    connectDaemon(*controller);
    QVERIFY(source.isWanted());
    controller->setOutputFullscreen({{u"DP-1"_s, u"mpv"_s}}, true);
    QVERIFY(!source.isWanted()); // the user is playing: nothing watches the cursor
    controller->setOutputFullscreen({}, true);
    QVERIFY(source.isWanted());
}

void TstOverlayController::thePreviewIsLimitedAndNeverBreaksADrag()
{
    statesPet(R"({"grabbed":{"variant":"grab"},"released":{"variant":"rel"},"moving":{"variant":"move"}})");
    auto controller = statesController();
    connectDaemon(*controller);
    FakeSurface *pet = surface(u"p1"_s);
    stateClock = 1000;
    QVERIFY(controller->previewState(u"p1"_s, u"grabbed"_s));
    QVERIFY(!controller->previewState(u"p1"_s, u"grabbed"_s)); // the same one again: nothing
    stateClock = 1100;
    QVERIFY(!controller->previewState(u"p1"_s, u"released"_s)); // under a quarter of a second after the last
    stateClock = 1300;
    QVERIFY(controller->previewState(u"p1"_s, u"released"_s));
    // the pet is released by itself after its pass; a dodge step or the follower's walk shows at once, they do not wait out the three seconds
    stateClock = 2000;
    controller->tickStates();
    QCOMPARE(pet->view.file, fileOf(u"idle"_s));
    send(pet, dodge(220));
    QCOMPARE(pet->view.file, fileOf(u"move"_s));
    // a real drag is not interrupted by the GUI, and what the user does goes on
    stateClock = 5000;
    send(pet, grab());
    QVERIFY(!controller->previewState(u"p1"_s, u"released"_s));
    QCOMPARE(pet->view.file, fileOf(u"grab"_s));
    send(pet, drag(30));
    QCOMPARE(pet->view.file, fileOf(u"move"_s));
    send(pet, release());
    QCOMPARE(pet->view.file, fileOf(u"rel"_s));
    // a dodge step ends a preview
    stateClock = 9000;
    QVERIFY(controller->previewState(u"p1"_s, u"grabbed"_s));
    QCOMPARE(pet->view.file, fileOf(u"grab"_s));
    stateClock = 9300;
    send(pet, dodge(220));
    QCOMPARE(pet->view.file, fileOf(u"move"_s));
    stateClock = 9520;
    controller->tickStates();
    QCOMPARE(pet->view.file, fileOf(u"idle"_s)); // (not three seconds of grabbed)
}

void TstOverlayController::aLockedPetDoesNotFollowAndAHeldOneStandsStill()
{
    QJsonObject locked = follower(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s));
    locked.insert(u"locked"_s, true);
    daemon.pets = QJsonArray{locked, follower(placement(u"p2"_s, u"anima:1"_s, u"DP-1"_s, 300))};
    ManualPointerSource source(PointerSource::Level::Global);
    auto controller = statesController();
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    controller->setPointerSource(&source);
    connectDaemon(*controller);
    drive(source, &stateClock, {1500, 95}, 2000);
    QCOMPARE(surface(u"p1"_s)->view.placement.x, 10.0);
    const double walked = surface(u"p2"_s)->view.placement.x;
    QVERIFY(walked > 300);
    // p2 is grabbed: it stands still while the cursor goes on; the drop is its place and it follows from there
    send(surface(u"p2"_s), grab());
    drive(source, &stateClock, {100, 95}, 1000);
    QCOMPARE(surface(u"p2"_s)->view.placement.x, walked);
    send(surface(u"p2"_s), release());
    surface(u"p2"_s)->m_callbacks.edited(QJsonObject{{u"x"_s, 700.0}, {u"y"_s, 400.0}});
    QJsonObject dropped = follower(placement(u"p2"_s, u"anima:1"_s, u"DP-1"_s, 300));
    dropped.insert(u"x"_s, 700.0);
    dropped.insert(u"y"_s, 400.0);
    daemon.pets = QJsonArray{locked, dropped};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_COMPARE(surface(u"p2"_s)->view.placement.x, 700.0);
    QCOMPARE(surface(u"p2"_s)->view.placement.y, 400.0);
    drive(source, &stateClock, {1800, 475}, 3000);
    QVERIFY(surface(u"p2"_s)->view.placement.x > 700);
}

void TstOverlayController::aFollowerParksOverAFullscreenWindowAndIsHomeWhenHiddenOrArranged()
{
    daemon.pets = QJsonArray{follower(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s))};
    ManualPointerSource source(PointerSource::Level::Global);
    auto controller = statesController();
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    controller->setPointerSource(&source);
    connectDaemon(*controller);
    drive(source, &stateClock, {1000, 95}, 600);
    const double at = surface(u"p1"_s)->view.placement.x;
    QVERIFY(at > 10);
    controller->setOutputFullscreen({{u"DP-1"_s, u"mpv"_s}}, true);
    QVERIFY(controller->describe().at(0).toObject().value(u"degraded"_s).toArray().contains(QString::fromLatin1(statesdegrade::kParkedOverFullscreen)));
    drive(source, &stateClock, {1000, 95}, 1500);
    QCOMPARE(surface(u"p1"_s)->view.placement.x, at);
    controller->setOutputFullscreen({}, true); // it walks again
    drive(source, &stateClock, {1000, 95}, 1500);
    QVERIFY(surface(u"p1"_s)->view.placement.x > at);
    // arranging shows the stored place at once
    daemon.arrange = true;
    daemon.emitSignal(u"PetsArrangeChanged"_s, {true});
    QTRY_COMPARE(surface(u"p1"_s)->view.placement.x, 10.0);
    daemon.arrange = false;
    daemon.emitSignal(u"PetsArrangeChanged"_s, {false});
    QTRY_VERIFY(!surface(u"p1"_s)->view.arrange);
    drive(source, &stateClock, {1000, 95}, 600);
    QVERIFY(surface(u"p1"_s)->view.placement.x > 10);
    controller->setSessionPaused(true);
    QCOMPARE(surface(u"p1"_s)->view.placement.x, 10.0);
}

void TstOverlayController::theBehavioursOfAPlacementReachTheView()
{
    QJsonObject p = placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s);
    daemon.pets = QJsonArray{p};
    auto controller = makeController();
    connectDaemon(*controller);
    // The defaults of a daemon that says nothing: what a pet did before the behaviours existed.
    const PetPlacement defaults = surface(u"p1"_s)->view.placement;
    QVERIFY(defaults.clickThrough && !defaults.locked && defaults.keepAbove && !defaults.hoverHide && !defaults.focusFade && !defaults.smartDodge);
    QCOMPARE(defaults.rotation, 0.0);
    // And every field, when it says them.
    p.insert(u"clickThrough"_s, false);
    p.insert(u"locked"_s, true);
    p.insert(u"keepAbove"_s, false);
    p.insert(u"hoverHide"_s, true);
    p.insert(u"smartDodge"_s, true);
    p.insert(u"rotation"_s, 33.5);
    p.insert(u"flipH"_s, true);
    p.insert(u"flipV"_s, true);
    daemon.pets = QJsonArray{p};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_VERIFY(!surface(u"p1"_s)->view.placement.clickThrough);
    const PetPlacement got = surface(u"p1"_s)->view.placement;
    QVERIFY(got.locked && !got.keepAbove && got.hoverHide && got.smartDodge && !got.focusFade && got.flipH && got.flipV);
    QCOMPARE(got.rotation, 33.5);
}

// A placement with a behaviour that has a use for hit masks.
QJsonObject withBehaviour(QJsonObject p, const QString &key, bool value)
{
    p.insert(key, value);
    return p;
}

void TstOverlayController::aPetOnTheVideoPathAsksForItsMasksOnlyWhenItNeedsThem()
{
    FakeMaskLoader loader;
    QJsonObject p = placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s);
    daemon.pets = QJsonArray{p};
    auto controller = makeController();
    controller->setMaskLoader(&loader);
    connectDaemon(*controller);
    FakeSurface *pet = surface(u"p1"_s);
    pet->m_callbacks.started();
    // The default placement (click-through, no behaviour) has no use for masks: no pass, nothing for the window.
    QVERIFY(loader.requests.empty());
    QCOMPARE(pet->view.maskBudget, qint64(0));
    QVERIFY(!pet->view.masks);
    // `locked` alone does not need them either.
    daemon.pets = QJsonArray{withBehaviour(p, u"locked"_s, true)};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_VERIFY(surface(u"p1"_s)->view.placement.locked);
    QVERIFY(loader.requests.empty());
    // Each of the others does, once the file plays.
    int expected = 0;
    for (const QString &key : {u"hoverHide"_s, u"focusFade"_s, u"smartDodge"_s}) {
        daemon.pets = QJsonArray{p};
        daemon.emitSignal(u"PetsChanged"_s);
        settle();
        daemon.pets = QJsonArray{withBehaviour(p, key, true)};
        daemon.emitSignal(u"PetsChanged"_s);
        ++expected;
        QTRY_COMPARE(int(loader.requests.size()), expected);
    }
    // Taking input on the opaque pixels (clickThrough off) needs them too.
    daemon.pets = QJsonArray{p};
    daemon.emitSignal(u"PetsChanged"_s);
    settle();
    daemon.pets = QJsonArray{withBehaviour(p, u"clickThrough"_s, false)};
    daemon.emitSignal(u"PetsChanged"_s);
    ++expected;
    QTRY_COMPARE(int(loader.requests.size()), expected);
    const auto request = loader.requests.back();
    QCOMPARE(request.key, u"p1"_s);
    QCOMPARE(request.item, u"anima:1"_s);
    QCOMPARE(request.variant, u"idle"_s);
    QCOMPARE(request.file, original);
    QCOMPARE(qint64(request.budget), controller->maskBudgetFor());
    // It is asked for once, however often the view is applied again.
    controller->setSessionPaused(true);
    controller->setSessionPaused(false);
    QCOMPARE(int(loader.requests.size()), expected);
    // The answer reaches the window; one for another file does not.
    request.done(u"/not/the/file"_s, someMasks());
    QVERIFY(!surface(u"p1"_s)->view.masks);
    const auto masks = someMasks();
    request.done(original, masks);
    QCOMPARE(surface(u"p1"_s)->view.masks, masks);
    // The behaviour goes off again: the masks are dropped and the job cancelled.
    loader.cancelled.clear();
    daemon.pets = QJsonArray{p};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_VERIFY(!surface(u"p1"_s)->view.masks);
    QVERIFY(loader.cancelled.contains(u"p1"_s));
    // The pet goes: its job is cancelled.
    daemon.pets = QJsonArray{withBehaviour(p, u"hoverHide"_s, true)};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_COMPARE(int(loader.requests.size()), expected + 1);
    loader.cancelled.clear();
    daemon.pets = QJsonArray{};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_VERIFY(loader.cancelled.contains(u"p1"_s));
}

void TstOverlayController::aPassForAnOldFileIsCancelled()
{
    FakeMaskLoader loader;
    QJsonObject p = withBehaviour(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), u"hoverHide"_s, true);
    daemon.pets = QJsonArray{p};
    auto controller = makeController();
    controller->setMaskLoader(&loader);
    connectDaemon(*controller);
    surface(u"p1"_s)->m_callbacks.started();
    QCOMPARE(int(loader.requests.size()), 1);
    // The transcode finishes: the pet plays another file; the pass for the first is cancelled, the answer to it ignored.
    daemon.items[u"anima:1"_s] = item(original, u"done"_s, transcoded);
    daemon.emitSignal(u"TranscodeChanged"_s, {u"anima:1"_s});
    QTRY_VERIFY(surface(u"p1"_s)->view.file == transcoded);
    QVERIFY(loader.cancelled.contains(u"p1"_s));
    loader.requests.front().done(original, someMasks());
    QVERIFY(!surface(u"p1"_s)->view.masks);
}

void TstOverlayController::aPetOnTheFrameCacheMakesMasksOnlyWhenItNeedsThem()
{
    FakeMaskLoader loader;
    QJsonObject p = placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s);
    daemon.pets = QJsonArray{p};
    daemon.items[u"anima:1"_s] = animatedItem(u"anima:1"_s, original);
    auto controller = makeController();
    controller->setMaskLoader(&loader);
    controller->setFrameCacheBudget(64 << 20);
    connectDaemon(*controller);
    surface(u"p1"_s)->m_callbacks.started();
    QTRY_VERIFY(!surface(u"p1"_s)->view.spriteKey.isEmpty());
    // Defaults: the decode makes no masks (a zero budget means no MaskBuilder), the key is the plain one, no pass is asked for.
    QCOMPARE(surface(u"p1"_s)->view.maskBudget, qint64(0));
    QCOMPARE(surface(u"p1"_s)->view.spriteKey, original + u"@200x150"_s);
    QVERIFY(loader.requests.empty());
    surface(u"p1"_s)->m_callbacks.spriteDone(true, 1200000, QString());
    // A behaviour that has a use for masks is turned on: the frames are asked for again, now with masks (another key), within the pet's share.
    daemon.pets = QJsonArray{withBehaviour(p, u"hoverHide"_s, true)};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_COMPARE(surface(u"p1"_s)->view.spriteKey, original + u"@200x150+m"_s);
    QCOMPARE(surface(u"p1"_s)->view.maskBudget, controller->maskBudgetFor());
    QVERIFY(surface(u"p1"_s)->view.maskBudget > 0);
    QVERIFY(loader.requests.empty()); // (the frames bring their masks)
    // Losing the frame cache sends it to the video path: now the pass is asked for.
    controller->setFrameCacheBudget(0);
    QCOMPARE(int(loader.requests.size()), 1);
}

void TstOverlayController::theDefaultSetupCostsNothing()
{
    // Five pets, click-through and no behaviour (what every existing pet is): no mask job, no mask budget, no pointer source made.
    FakeMaskLoader loader;
    int made = 0;
    LazyPointerSource source(PointerSource::Level::Global, [&made]() -> std::unique_ptr<PointerSource> {
        ++made;
        return std::make_unique<ManualPointerSource>();
    });
    daemon.pets = QJsonArray{};
    for (int i = 0; i < 5; ++i)
        daemon.pets.append(placement(u"p%1"_s.arg(i), u"anima:1"_s, u"DP-1"_s, 10 + 30 * i));
    daemon.items[u"anima:1"_s] = animatedItem(u"anima:1"_s, original);
    auto controller = makeController();
    controller->setMaskLoader(&loader);
    controller->setPointerSource(&source);
    controller->setFrameCacheBudget(3'000'000); // room for some of them: the others play through the video path
    connectDaemon(*controller);
    for (FakeSurface *s : surfaces) {
        s->m_callbacks.started();
        s->m_callbacks.spriteDone(true, 600000, QString());
    }
    QTest::qWait(100);
    controller->setSessionPaused(true);
    controller->setSessionPaused(false);
    QVERIFY(loader.requests.empty());
    for (FakeSurface *s : surfaces) {
        QCOMPARE(s->view.maskBudget, qint64(0));
        QVERIFY(!s->view.masks);
        QVERIFY(!s->view.spriteKey.endsWith(u"+m"_s));
    }
    QCOMPARE(made, 0);
    QVERIFY(!source.created());
    QCOMPARE(int(surfaces.size()), 5);
}

void TstOverlayController::thePetsShareTheMaskBudgetAndKeepTheirUnion()
{
    daemon.pets = QJsonArray{withBehaviour(placement(u"a"_s, u"anima:1"_s, u"DP-1"_s), u"hoverHide"_s, true),
                             withBehaviour(placement(u"b"_s, u"anima:1"_s, u"DP-1"_s), u"hoverHide"_s, true),
                             withBehaviour(placement(u"c"_s, u"anima:1"_s, u"DP-1"_s), u"hoverHide"_s, true),
                             withBehaviour(placement(u"d"_s, u"anima:1"_s, u"DP-1"_s), u"hoverHide"_s, true)};
    auto controller = makeController();
    connectDaemon(*controller);
    QCOMPARE(controller->maskBudgetFor(), qint64(kMaskBudgetBytes) / 4);
    QCOMPARE(surface(u"a"_s)->view.maskBudget, qint64(kMaskBudgetBytes) / 4);
    controller->setMaskBudget(4000);
    QCOMPARE(controller->maskBudgetFor(), qint64(2048 + kMaskOverheadBytes)); // never less than the union
    QCOMPARE(surface(u"d"_s)->view.maskBudget, qint64(2048 + kMaskOverheadBytes));
    controller->setMaskBudget(8'000'000);
    QCOMPARE(controller->maskBudgetFor(), qint64(2'000'000));
    // The pets that stay are told when some go.
    daemon.pets = QJsonArray{withBehaviour(placement(u"a"_s, u"anima:1"_s, u"DP-1"_s), u"hoverHide"_s, true),
                             withBehaviour(placement(u"b"_s, u"anima:1"_s, u"DP-1"_s), u"hoverHide"_s, true)};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_COMPARE(controller->maskBudgetFor(), qint64(4'000'000));
    QTRY_COMPARE(surface(u"a"_s)->view.maskBudget, qint64(4'000'000));
}

void TstOverlayController::removingTheLastPetThatNeededThePointerStopsTheSource()
{
    daemon.pets = QJsonArray{placement(u"a"_s, u"anima:1"_s, u"DP-1"_s), placement(u"b"_s, u"anima:1"_s, u"DP-1"_s)};
    ManualPointerSource source(PointerSource::Level::Global);
    auto controller = makeController();
    controller->setPointerSource(&source);
    connectDaemon(*controller);
    surface(u"a"_s)->wantsPointer = true;
    surface(u"a"_s)->m_callbacks.pointerWantChanged();
    QVERIFY(source.isWanted());
    // Pet b goes: a still needs it.
    daemon.pets = QJsonArray{placement(u"a"_s, u"anima:1"_s, u"DP-1"_s)};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_COMPARE(controller->surfaceCount(), 1);
    QVERIFY(source.isWanted());
    // The last pet that needed it goes: nothing runs in the compositor any more.
    daemon.pets = QJsonArray{};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_COMPARE(controller->surfaceCount(), 0);
    QVERIFY(!source.isWanted());
    // The same when its output goes away.
    daemon.pets = QJsonArray{placement(u"c"_s, u"anima:1"_s, u"HDMI-A-1"_s)};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_COMPARE(controller->surfaceCount(), 1);
    surface(u"c"_s)->wantsPointer = true;
    surface(u"c"_s)->m_callbacks.pointerWantChanged();
    QVERIFY(source.isWanted());
    controller->setOutputs({u"DP-1"_s}, u"DP-1"_s);
    QTRY_COMPARE(controller->surfaceCount(), 0);
    QVERIFY(!source.isWanted());
}

void TstOverlayController::theGlobalPointerRunsOnlyWhileAPetNeedsItAndReachesOnlyThosePets()
{
    daemon.pets = QJsonArray{placement(u"a"_s, u"anima:1"_s, u"DP-1"_s), placement(u"b"_s, u"anima:1"_s, u"DP-1"_s)};
    ManualPointerSource source(PointerSource::Level::Global);
    auto controller = makeController();
    controller->setPointerSource(&source);
    QCOMPARE(int(controller->pointerLevel()), int(PointerSource::Level::Global));
    connectDaemon(*controller);
    QCOMPARE(int(surface(u"a"_s)->view.pointer), int(PointerSource::Level::Global));
    QVERIFY(!source.isWanted()); // no hide, fade or dodge: nothing runs
    // Pet a starts to need the pointer (the window says so).
    surface(u"a"_s)->wantsPointer = true;
    surface(u"a"_s)->m_callbacks.pointerWantChanged();
    QVERIFY(source.isWanted());
    source.moveTo({100, 200});
    QCOMPARE(surface(u"a"_s)->pointers.size(), 1);
    QCOMPARE(surface(u"a"_s)->pointers.first(), QPointF(100, 200));
    QCOMPARE(surface(u"b"_s)->pointers.size(), 0); // b does not need it
    surface(u"a"_s)->wantsPointer = false;
    surface(u"a"_s)->m_callbacks.pointerWantChanged();
    QVERIFY(!source.isWanted());
    source.moveTo({1, 1});
    QCOMPARE(surface(u"a"_s)->pointers.size(), 1);
}

void TstOverlayController::aGlobalSourceThatFailsMakesTheSessionLocal()
{
    ManualPointerSource source(PointerSource::Level::Global);
    auto controller = makeController();
    controller->setPointerSource(&source);
    connectDaemon(*controller);
    surface(u"p1"_s)->wantsPointer = true;
    surface(u"p1"_s)->m_callbacks.pointerWantChanged();
    QVERIFY(source.isWanted());
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"the global pointer is unavailable"_s));
    emit source.unavailable(u"KWin refused the script"_s);
    QCOMPARE(int(controller->pointerLevel()), int(PointerSource::Level::Local));
    QCOMPARE(int(surface(u"p1"_s)->view.pointer), int(PointerSource::Level::Local));
    QVERIFY(!source.isWanted());
    // A local source from the start.
    ManualPointerSource local(PointerSource::Level::Local);
    controller->setPointerSource(&local);
    QCOMPARE(int(surface(u"p1"_s)->view.pointer), int(PointerSource::Level::Local));
    QVERIFY(!local.isWanted()); // (the local level needs no source running)
}

void TstOverlayController::describeReportsThePointerInput()
{
    auto controller = makeController();
    connectDaemon(*controller);
    QVERIFY(!controller->describe().at(0).toObject().contains(u"hitMask"_s)); // a surface that says nothing adds nothing
    FakeSurface *pet = surface(u"p1"_s);
    pet->status.pointer = u"local"_s;
    pet->status.hitMask = u"frame"_s;
    pet->status.maskBytes = 4321;
    pet->status.hidden = true;
    pet->status.degraded = {degrade::kPointerLocal, degrade::kHideClicksSwallowed};
    const QJsonObject window = controller->describe().at(0).toObject();
    QCOMPARE(window.value(u"pointer"_s).toString(), u"local"_s);
    QCOMPARE(window.value(u"hitMask"_s).toString(), u"frame"_s);
    QCOMPARE(window.value(u"maskBytes"_s).toInt(), 4321);
    QVERIFY(window.value(u"hidden"_s).toBool());
    QCOMPARE(window.value(u"degraded"_s).toArray(), (QJsonArray{u"pointer: local"_s, u"hide under cursor: clicks swallowed"_s}));
}

void TstOverlayController::thePetsSoundFollowsTheViewTheWrapsAndTheClicks()
{
    qint64 t = 7'000'000'000;
    FakePlayer player;
    PetAudioController audio(&player, [&t] { return t; });
    daemon.items[u"anima:1"_s] = animatedItem(u"anima:1"_s, original);
    daemon.settingsAnswer = true; // (an earlier test may have turned it off)
    daemon.settings = QJsonObject{{u"petsVolume"_s, 30}};
    daemon.pets = QJsonArray{withSound(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), u"animation_loop"_s)};
    auto controller = makeController();
    controller->setAudio(&audio);
    connectDaemon(*controller);
    FakeSurface *s = surface(u"p1"_s);
    QVERIFY(s);
    QVERIFY(s->view.placement.audio.active());
    QCOMPARE(s->view.placement.audio.volume, 50);
    QVERIFY(player.lines.isEmpty()); // nothing until the animation wraps: no process either
    QVERIFY(!audio.playerRunning());
    // the window reports a wrap
    s->m_callbacks.wrapped(t - 5'000'000, 0);
    QCOMPARE(player.with("play").size(), 1);
    QVERIFY(player.with("play")[0].contains(u" 500 0 "_s)); // the pet's volume, not looped
    QTRY_VERIFY2(player.with("master").contains(u"master 300"_s), qPrintable(player.lines.join(u"|"_s))); // the daemon's setting
    // describe() says what the sound does
    const QJsonObject described = controller->describe().at(0).toObject().value(u"audio"_s).toObject();
    QCOMPARE(described.value(u"mode"_s).toString(), u"animation_loop"_s);
    QCOMPARE(described.value(u"state"_s).toString(), u"idle"_s);
    // a click does nothing in this mode
    player.lines.clear();
    s->m_callbacks.clicked();
    QVERIFY(player.lines.isEmpty());
    // hiding every pet silences them; a wrap while hidden is ignored
    daemon.emitSignal(u"PetsHiddenChanged"_s, {true});
    QCOMPARE(player.with("stop").size(), 1);
    s->m_callbacks.wrapped(t, 1);
    QCOMPARE(player.with("play").size(), 0);
    daemon.emitSignal(u"PetsHiddenChanged"_s, {false});
    // the same pet in the click mode, taking the pointer
    daemon.pets = QJsonArray{withSound(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), u"click"_s, false)};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_COMPARE(surface(u"p1"_s)->view.placement.audio.mode, PetAudioMode::Click);
    player.lines.clear();
    surface(u"p1"_s)->m_callbacks.clicked();
    QCOMPARE(player.with("play").size(), 1);
    // click-through: the click is not played even if one arrives
    daemon.pets = QJsonArray{withSound(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), u"click"_s, true)};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_VERIFY(surface(u"p1"_s)->view.placement.clickThrough);
    player.lines.clear();
    surface(u"p1"_s)->m_callbacks.clicked();
    QVERIFY(player.with("play").isEmpty());
    // a locked session pauses the pet's sound (a continuous one pauses, others stop)
    daemon.pets = QJsonArray{withSound(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), u"continuous"_s)};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_COMPARE(player.with("play").size(), 1);
    player.lines.clear();
    controller->setSessionPaused(true);
    QCOMPARE(player.with("pause").size(), 1);
    controller->setSessionPaused(false);
    QCOMPARE(player.with("resume").size(), 1);
    // removing the pet stops it
    player.lines.clear();
    daemon.pets = {};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_COMPARE(player.with("stop").size(), 1);
}

namespace {

// A puppet item (docs/pets/puppets.md): one model file, a canvas of 300 x 500, motion groups Idle and TapBody, the expression smile. Its variants are not animation files.
QJsonObject puppetItem(const QString &dir, const QStringList &groups = {u"Idle"_s, u"TapBody"_s})
{
    QJsonArray variants, groupList;
    const auto variant = [&](const QString &name) {
        return QJsonObject{{u"name"_s, name}, {u"file"_s, dir + u"/model.inp"_s}, {u"width"_s, 300}, {u"height"_s, 500}, {u"fps"_s, 60}, {u"frames"_s, 0}};
    };
    variants.append(variant(u"model"_s));
    for (const QString &g : groups) {
        variants.append(variant(u"motion:"_s + g));
        groupList.append(QJsonObject{{u"name"_s, g}, {u"motions"_s, 1}});
    }
    variants.append(variant(u"expression:smile"_s));
    return {{u"id"_s, u"local:puppet:p1"_s}, {u"format"_s, u"puppet"_s}, {u"dir"_s, dir}, {u"entryFile"_s, dir + u"/model.inp"_s},
            {u"puppet"_s, QJsonObject{{u"backend"_s, u"open"_s}, {u"motionGroups"_s, groupList}, {u"expressions"_s, QJsonArray{u"smile"_s}}}}, {u"variants"_s, variants}};
}

QJsonObject puppetPlacement(const QString &petId = u"p1"_s)
{
    QJsonObject p = placement(petId, u"local:puppet:p1"_s, u"DP-1"_s);
    p[u"variant"_s] = u"model"_s;
    return p;
}

} // namespace

void TstOverlayController::aPuppetPetIsShownWithNoneOfThePictureMachinery()
{
    daemon.items[u"local:puppet:p1"_s] = puppetItem(tmp.path());
    daemon.pets = QJsonArray{puppetPlacement()};
    auto controller = makeController();
    controller->setFrameCacheBudget(512 << 20); // (room for frames: a puppet has none)
    connectDaemon(*controller);
    FakeSurface *pet = surface(u"p1"_s);
    QVERIFY(pet);
    const PetView &v = pet->view;
    QVERIFY(v.puppet.active);
    QCOMPARE(v.puppet.dir, tmp.path());
    QCOMPARE(v.puppet.file, u"model.inp"_s);
    QCOMPARE(v.native, QSize(300, 500)); // the pet's aspect is the canvas'
    QVERIFY(v.puppet.group.isEmpty() && v.puppet.expression.isEmpty());
    QVERIFY(!v.spriteSize.isValid());     // no frame cache, no preload, no masks, nothing converted
    QVERIFY(v.spriteKey.isEmpty() && v.preloadKey.isEmpty() && !v.masks);
    QCOMPARE(v.bucket, 0);
    QVERIFY(v.puppet.requireSandbox);     // (the default of importRequireSandbox)
    QCOMPARE(v.puppet.fps, 60);
    const QJsonObject o = controller->describe().at(0).toObject();
    QCOMPARE(o.value(u"mode"_s).toString(), u"video"_s);
    const QJsonObject puppet = o.value(u"puppet"_s).toObject();
    QCOMPARE(puppet.value(u"state"_s).toString(), u"starting"_s); // (no picture yet)
    QCOMPARE(puppet.value(u"crashes"_s).toInt(), 0);
    pet->m_callbacks.started();
    QCOMPARE(controller->describe().at(0).toObject().value(u"puppet"_s).toObject().value(u"state"_s).toString(), u"running"_s);
    // another pet has no such key
    daemon.items[u"anima:1"_s] = item(original);
    daemon.pets = QJsonArray{puppetPlacement(), placement(u"p2"_s, u"anima:1"_s, u"DP-1"_s)};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_COMPARE(controller->describe().size(), 2);
    QVERIFY(!controller->describe().at(1).toObject().contains(u"puppet"_s));
    // the frame rate follows the placement and the power state
    QJsonObject p = puppetPlacement();
    p[u"fps"_s] = 24;
    daemon.pets = QJsonArray{p};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_COMPARE(surface(u"p1"_s)->view.puppet.fps, 24);
    controller->setPowerState(true, false);
    QVERIFY(surface(u"p1"_s)->view.puppet.fps <= 24);
}

void TstOverlayController::aPuppetsStatesPlayItsMotionsAndExpressions()
{
    daemon.items[u"local:puppet:p1"_s] = puppetItem(tmp.path());
    QJsonObject p = puppetPlacement();
    p.insert(u"states"_s, QJsonDocument::fromJson(R"({"grabbed":{"variant":"motion:TapBody"},"released":{"variant":"expression:smile"},"idle":{"variant":"motion:Idle"}})").object());
    daemon.pets = QJsonArray{p};
    auto controller = statesController();
    connectDaemon(*controller);
    FakeSurface *pet = surface(u"p1"_s);
    QVERIFY(pet);
    QCOMPARE(pet->view.puppet.group, u"Idle"_s); // idle's own motion plays from the start
    const int first = pet->view.puppet.playSerial;
    QVERIFY(first > 0);
    stateClock = 1000;
    send(pet, grab());
    QCOMPARE(pet->view.puppet.group, u"TapBody"_s);
    QVERIFY(pet->view.puppet.playSerial > first); // (a new request: the window plays it)
    QCOMPARE(surface(u"p1"_s), pet);              // the same window: the engine is not restarted for a state
    const int second = pet->view.puppet.playSerial;
    stateClock = 2000;
    send(pet, release());
    QCOMPARE(pet->view.puppet.expression, u"smile"_s);
    QVERIFY(pet->view.puppet.group.isEmpty());   // the window ends the motion
    QVERIFY(pet->view.puppet.playSerial > second);
    // one pass of released later (a puppet has no length: 1.2 s), idle again: its motion plays, the expression is off
    stateClock = 3300;
    controller->tickStates();
    QCOMPARE(pet->view.puppet.group, u"Idle"_s);
    QVERIFY(pet->view.puppet.expression.isEmpty());
    // a variant the model does not have plays nothing: another pet, on a model with an Idle group only, whose grabbed state names TapBody
    QJsonObject other = puppetItem(tmp.path(), {u"Idle"_s});
    other[u"id"_s] = u"local:puppet:p2"_s;
    daemon.items[u"local:puppet:p2"_s] = other;
    QJsonObject q = puppetPlacement(u"p2"_s);
    q[u"itemId"_s] = u"local:puppet:p2"_s;
    q.insert(u"states"_s, QJsonDocument::fromJson(R"({"grabbed":{"variant":"motion:TapBody"}})").object());
    daemon.pets = QJsonArray{p, q};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_VERIFY(surface(u"p2"_s));
    FakeSurface *poor = surface(u"p2"_s);
    stateClock = 5000;
    send(poor, grab());
    QCOMPARE(controller->describe().at(1).toObject().value(u"state"_s).toString(), u"grabbed"_s);
    QVERIFY(poor->view.puppet.group.isEmpty());
    QVERIFY(poor->view.puppet.expression.isEmpty());
}

void TstOverlayController::aClickPlaysAPuppetsTapMotion()
{
    // The click itself is judged by the engine (the hit area it falls on, else a tap group): the controller tells the window which tap groups the model has, in the order they are tried, and a click does not
    // make the controller play anything on its own.
    daemon.items[u"local:puppet:p1"_s] = puppetItem(tmp.path());
    daemon.pets = QJsonArray{puppetPlacement()};
    auto controller = makeController();
    connectDaemon(*controller);
    FakeSurface *pet = surface(u"p1"_s);
    QCOMPARE(pet->view.puppet.tapGroups, (QStringList{u"TapBody"_s}));
    const int before = pet->view.puppet.playSerial;
    click(pet);
    for (int i = 0; i < 5; ++i)
        QCoreApplication::processEvents();
    QCOMPARE(pet->view.puppet.playSerial, before);
    QVERIFY(pet->view.puppet.group.isEmpty());
    // both tap groups: TapBody first; none: an empty list
    QJsonObject both = puppetItem(tmp.path(), {u"Tap"_s, u"Idle"_s, u"TapBody"_s});
    both[u"id"_s] = u"local:puppet:p2"_s;
    QJsonObject none = puppetItem(tmp.path(), {u"Idle"_s});
    none[u"id"_s] = u"local:puppet:p3"_s;
    daemon.items[u"local:puppet:p2"_s] = both;
    daemon.items[u"local:puppet:p3"_s] = none;
    QJsonObject second = puppetPlacement(u"p2"_s), third = puppetPlacement(u"p3"_s);
    second[u"itemId"_s] = u"local:puppet:p2"_s;
    third[u"itemId"_s] = u"local:puppet:p3"_s;
    daemon.pets = QJsonArray{puppetPlacement(), second, third};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_VERIFY(surface(u"p3"_s));
    QCOMPARE(surface(u"p2"_s)->view.puppet.tapGroups, (QStringList{u"TapBody"_s, u"Tap"_s}));
    QVERIFY(surface(u"p3"_s)->view.puppet.tapGroups.isEmpty());
}

void TstOverlayController::aPuppetEngineThatCrashesIsCountedRestartedAndBlocked()
{
    const QString stateDir = tmp.filePath(u"state/puppet"_s);
    CrashLedger crashes(stateDir);
    crashes.load();
    ledger = &crashes;
    const auto guard = qScopeGuard([&] { ledger = nullptr; });
    daemon.items[u"local:puppet:p1"_s] = puppetItem(tmp.path());
    daemon.pets = QJsonArray{puppetPlacement()};
    auto controller = makeController();
    connectDaemon(*controller);
    FakeSurface *pet = surface(u"p1"_s);
    QVERIFY(pet);
    const QString model = tmp.path() + u"/model.inp"_s;
    testutil::writeFile(model, "model");
    pet->m_callbacks.started(); // the first picture
    // the first crash: the window restarts the engine by itself; the pet says so and the model is charged once
    pet->m_callbacks.puppetCrashed(false);
    QCOMPARE(crashes.crashes(model), 1);
    QJsonObject o = controller->describe().at(0).toObject();
    QCOMPARE(o.value(u"puppet"_s).toObject().value(u"state"_s).toString(), u"restarting"_s);
    QVERIFY(o.value(u"degraded"_s).toArray().contains(QString::fromLatin1(puppet::kEngineRestarted)));
    QCOMPARE(o.value(u"puppet"_s).toObject().value(u"crashes"_s).toInt(), 1);
    QCOMPARE(surface(u"p1"_s), pet);
    pet->m_callbacks.started(); // it draws again
    o = controller->describe().at(0).toObject();
    QCOMPARE(o.value(u"puppet"_s).toObject().value(u"state"_s).toString(), u"running"_s);
    QVERIFY(!o.value(u"degraded"_s).toArray().contains(QString::fromLatin1(puppet::kEngineRestarted)));
    // a crash after a long run starts the count again
    pet->m_callbacks.puppetCrashed(true);
    QCOMPARE(crashes.crashes(model), 1);
    pet->m_callbacks.puppetCrashed(false);
    QCOMPARE(crashes.crashes(model), 2);
    // the third: the model is blocked, the window goes (and the engine with it), and Windows() says why
    pet->m_callbacks.puppetCrashed(false);
    QCOMPARE(crashes.crashes(model), 3);
    QTRY_COMPARE(controller->surfaceCount(), 0);
    o = controller->describe().at(0).toObject();
    QVERIFY(o.value(u"blocked"_s).toBool());
    QCOMPARE(o.value(u"puppet"_s).toObject().value(u"state"_s).toString(), u"blocked"_s);
    QVERIFY(o.value(u"degraded"_s).toArray().contains(QString::fromLatin1(puppet::kBlockedAfterCrashes)));
    QCOMPARE(o.value(u"puppet"_s).toObject().value(u"crashes"_s).toInt(), 3); // (the window is gone, the count is still the model's)
    // across a restart of the overlay the ledger still blocks it
    CrashLedger again(stateDir);
    again.load();
    QVERIFY(again.blocked(model));
}

void TstOverlayController::aPuppetItemWithoutAModelFileInsideItsFolderIsNamedNotDropped()
{
    QJsonObject broken = puppetItem(tmp.path());
    broken[u"entryFile"_s] = u"/somewhere/else/model.inp"_s;
    daemon.items[u"local:puppet:p1"_s] = broken;
    daemon.pets = QJsonArray{puppetPlacement()};
    auto controller = makeController();
    connectDaemon(*controller);
    QCOMPARE(controller->surfaceCount(), 0);
    const QJsonObject o = controller->describe().at(0).toObject();
    QCOMPARE(o.value(u"puppet"_s).toObject().value(u"state"_s).toString(), u"refused"_s);
    QVERIFY(o.value(u"degraded"_s).toArray().at(0).toString().startsWith(QString::fromLatin1(puppet::kManifestInvalid)));
}

void TstOverlayController::aPuppetEngineThatRefusesIsNamedAndNotCounted()
{
    const QString stateDir = tmp.filePath(u"state/puppet2"_s);
    CrashLedger crashes(stateDir);
    crashes.load();
    ledger = &crashes;
    const auto guard = qScopeGuard([&] { ledger = nullptr; });
    daemon.items[u"local:puppet:p1"_s] = puppetItem(tmp.path());
    daemon.pets = QJsonArray{puppetPlacement()};
    auto controller = makeController();
    connectDaemon(*controller);
    FakeSurface *pet = surface(u"p1"_s);
    const QString model = tmp.path() + u"/model.inp"_s;
    pet->m_callbacks.puppetRefused(QString::fromLatin1(puppet::kNoGpu));
    QJsonObject o = controller->describe().at(0).toObject();
    QCOMPARE(o.value(u"puppet"_s).toObject().value(u"state"_s).toString(), u"nogpu"_s);
    QVERIFY(o.value(u"degraded"_s).toArray().contains(QString::fromLatin1(puppet::kNoGpu)));
    pet->m_callbacks.puppetRefused(u"model refused"_s);
    o = controller->describe().at(0).toObject();
    QCOMPARE(o.value(u"puppet"_s).toObject().value(u"state"_s).toString(), u"refused"_s);
    QVERIFY(o.value(u"degraded"_s).toArray().contains(u"model refused"_s));
    QCOMPARE(crashes.crashes(model), 0);
    QVERIFY(!o.value(u"blocked"_s).toBool());
    QCOMPARE(controller->surfaceCount(), 1);
}

void TstOverlayController::lookIsAPuppetsGazeNotAMirror()
{
    daemon.items[u"local:puppet:p1"_s] = puppetItem(tmp.path());
    QJsonObject p = puppetPlacement();
    p.insert(u"tracking"_s, QJsonObject{{u"mode"_s, u"look"_s}});
    daemon.pets = QJsonArray{p};
    ManualPointerSource source(PointerSource::Level::Global);
    auto controller = statesController();
    controller->setPointerSource(&source);
    connectDaemon(*controller);
    FakeSurface *pet = surface(u"p1"_s);
    QVERIFY(pet->view.puppet.look);          // the engine is told to look
    QVERIFY(!pet->view.placement.flipH);
    stateClock = 1000;
    source.moveTo({-300, 95});                // the cursor is on the left: a pet would mirror, a puppet turns its eyes (the window gets the pointer)
    QVERIFY(!pet->view.placement.flipH);
    // tracking off: no gaze
    p.insert(u"tracking"_s, QJsonObject{{u"mode"_s, u"off"_s}});
    daemon.pets = QJsonArray{p};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_VERIFY(!surface(u"p1"_s)->view.puppet.look);
}

void TstOverlayController::thePuppetPictureSizeFollowsItsSetting()
{
    daemon.items[u"local:puppet:p1"_s] = puppetItem(tmp.path());
    daemon.pets = QJsonArray{puppetPlacement()};
    auto controller = makeController();
    connectDaemon(*controller);
    QCOMPARE(surface(u"p1"_s)->view.puppet.maxPixels, 4194304LL); // the default, until the settings are read
    daemon.settings = QJsonObject{{u"puppetMaxPixels"_s, 2000000}};
    daemon.emitSignal(u"SettingsChanged"_s);
    QTRY_COMPARE(surface(u"p1"_s)->view.puppet.maxPixels, 2000000LL);
    daemon.settings = QJsonObject{{u"puppetMaxPixels"_s, 5}}; // nonsense is held to the range
    daemon.emitSignal(u"SettingsChanged"_s);
    QTRY_COMPARE(surface(u"p1"_s)->view.puppet.maxPixels, 1000000LL);
}

void TstOverlayController::thePuppetSandboxFollowsTheImportSetting()
{
    daemon.items[u"local:puppet:p1"_s] = puppetItem(tmp.path());
    daemon.pets = QJsonArray{puppetPlacement()};
    daemon.settings = QJsonObject{{u"importRequireSandbox"_s, false}};
    auto controller = makeController();
    connectDaemon(*controller);
    QTRY_VERIFY(!surface(u"p1"_s)->view.puppet.requireSandbox);
    daemon.settings = QJsonObject{{u"importRequireSandbox"_s, true}};
    daemon.emitSignal(u"SettingsChanged"_s);
    QTRY_VERIFY(surface(u"p1"_s)->view.puppet.requireSandbox);
}

void TstOverlayController::createsOneSurfacePerPetOnItsOutput()
{
    daemon.pets = QJsonArray{placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), placement(u"p2"_s, u"anima:1"_s, u"HDMI-A-1"_s, 99)};
    auto controller = makeController();
    connectDaemon(*controller);
    QCOMPARE(daemon.startCount, 1);
    QCOMPARE(surfaces.size(), size_t(2));
    FakeSurface *first = surface(u"p1"_s);
    FakeSurface *second = surface(u"p2"_s);
    QVERIFY(first && second);
    QCOMPARE(first->output, u"DP-1"_s);
    QCOMPARE(second->output, u"HDMI-A-1"_s);
    QCOMPARE(first->view.file, original);
    QCOMPARE(first->view.native, QSize(400, 300));
    QCOMPARE(first->view.placement.width, 200.0);
    QCOMPARE(first->view.placement.opacity, 0.8);
    QCOMPARE(second->view.placement.x, 99.0);
    QVERIFY(!first->view.arrange);
    QVERIFY(!first->view.paused);
    QCOMPARE(daemon.callCount(u"GetItem"_s), 1); // one item, however many pets
}

void TstOverlayController::starRunsOnThePrimaryOutput()
{
    daemon.pets = QJsonArray{placement(u"p1"_s, u"anima:1"_s, u"*"_s)};
    auto controller = makeController();
    connectDaemon(*controller);
    QVERIFY(surface(u"p1"_s));
    QCOMPARE(surface(u"p1"_s)->output, u"DP-1"_s);
}

void TstOverlayController::outputsComeAndGo()
{
    daemon.pets = QJsonArray{placement(u"p1"_s, u"anima:1"_s, u"HDMI-A-1"_s), placement(u"p2"_s, u"anima:1"_s, u"*"_s)};
    auto controller = makeController();
    connectDaemon(*controller);
    QCOMPARE(surfaces.size(), size_t(2));
    controller->setOutputs({u"DP-1"_s}, u"DP-1"_s); // HDMI unplugged
    QCOMPARE(surfaces.size(), size_t(1));
    QVERIFY(surface(u"p2"_s));
    QVERIFY2(controller->describe().size() == 2, "the placement is kept");
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"HDMI-A-1"_s); // back, and the primary changed
    QCOMPARE(surfaces.size(), size_t(2));
    QCOMPARE(surface(u"p1"_s)->output, u"HDMI-A-1"_s);
    QCOMPARE(surface(u"p2"_s)->output, u"HDMI-A-1"_s); // "*" moved with the primary output
    controller->setOutputs({}, QString());              // everything off
    QVERIFY(surfaces.empty());
}

void TstOverlayController::playsTheTranscodeWhenDone()
{
    auto controller = makeController();
    connectDaemon(*controller);
    QCOMPARE(surface(u"p1"_s)->view.file, original);

    daemon.items[u"anima:1"_s] = item(original, u"running"_s);
    daemon.emitSignal(u"TranscodeChanged"_s, {u"anima:1"_s});
    QTRY_COMPARE(daemon.callCount(u"GetItem"_s), 2);
    QTest::qWait(20);
    QCOMPARE(surface(u"p1"_s)->view.file, original); // still converting

    daemon.items[u"anima:1"_s] = item(original, u"done"_s, transcoded);
    daemon.emitSignal(u"TranscodeChanged"_s, {u"anima:1"_s});
    QTRY_COMPARE(surface(u"p1"_s)->view.file, transcoded);

    // A copy that vanished from disk falls back to the original.
    daemon.items[u"anima:1"_s] = item(original, u"done"_s, tmp.filePath(u"gone.webm"_s));
    daemon.emitSignal(u"TranscodeChanged"_s, {u"anima:1"_s});
    QTRY_COMPARE(surface(u"p1"_s)->view.file, original);
}

void TstOverlayController::playOriginalKeepsTheOriginal()
{
    daemon.items[u"anima:1"_s] = item(original, u"done"_s, transcoded);
    QJsonObject pet = placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s);
    pet[u"playOriginal"_s] = true;
    daemon.pets = QJsonArray{pet};
    auto controller = makeController();
    connectDaemon(*controller);
    QCOMPARE(surface(u"p1"_s)->view.file, original);
}

void TstOverlayController::arrangeFollowsTheDaemonAndToggles()
{
    daemon.arrange = true; // already on when the overlay starts
    auto controller = makeController();
    connectDaemon(*controller);
    QTRY_VERIFY(surface(u"p1"_s)->view.arrange);
    QVERIFY(surface(u"p1"_s)->inputEnabled());

    daemon.emitSignal(u"PetsArrangeChanged"_s, {false});
    QVERIFY(!surface(u"p1"_s)->view.arrange);
    QVERIFY(!controller->arrange());

    controller->toggleArrange();
    QTRY_COMPARE(daemon.callCount(u"SetPetsArrange"_s), 1);
    QCOMPARE(daemon.lastArgs(u"SetPetsArrange"_s).value(0).toBool(), true);
    daemon.emitSignal(u"PetsArrangeChanged"_s, {true});
    controller->toggleArrange();
    QTRY_COMPARE(daemon.callCount(u"SetPetsArrange"_s), 2);
    QCOMPARE(daemon.lastArgs(u"SetPetsArrange"_s).value(0).toBool(), false);
}

void TstOverlayController::hidingUnmapsEveryPetAndKeepsTheirState()
{
    QJsonObject second = placement(u"p2"_s, u"anima:1"_s, u"DP-1"_s, 300);
    daemon.pets = QJsonArray{placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), second};
    auto controller = makeController();
    connectDaemon(*controller);
    QCOMPARE(controller->surfaceCount(), 2);
    FakeSurface *first = surface(u"p1"_s);
    QVERIFY(!first->view.hidden);
    QVERIFY(!first->view.paused);

    daemon.emitSignal(u"PetsHiddenChanged"_s, {true});
    QVERIFY(controller->petsHidden());
    for (const char *id : {"p1", "p2"}) {
        QVERIFY2(surface(QLatin1String(id))->view.hidden, id);   // unmapped
        QVERIFY2(surface(QLatin1String(id))->view.paused, id);   // and paused: no decode, no frames
    }
    QCOMPARE(controller->surfaceCount(), 2);                       // the surfaces (their frames and places) are kept
    QCOMPARE(surface(u"p1"_s), first);
    QCOMPARE(surface(u"p1"_s)->view.placement.x, 10.0);

    daemon.emitSignal(u"PetsHiddenChanged"_s, {false});
    QVERIFY(!surface(u"p1"_s)->view.hidden && !surface(u"p1"_s)->view.paused);
    QCOMPARE(surface(u"p1"_s), first);
    // a pet paused on its own stays paused when the others are shown again
    QJsonObject stopped = placement(u"p2"_s, u"anima:1"_s, u"DP-1"_s, 300);
    stopped[u"paused"_s] = true;
    daemon.pets = QJsonArray{placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), stopped};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_VERIFY(surface(u"p2"_s)->view.paused);
    daemon.emitSignal(u"PetsHiddenChanged"_s, {true});
    daemon.emitSignal(u"PetsHiddenChanged"_s, {false});
    QVERIFY(surface(u"p2"_s)->view.paused && !surface(u"p1"_s)->view.paused);
}

void TstOverlayController::bothSessionStatesAreReadAgainOnEveryConnection()
{
    daemon.arrange = true;
    daemon.petsHidden = true;
    auto controller = makeController();
    connectDaemon(*controller);
    QTRY_VERIFY(controller->petsHidden());
    QVERIFY(controller->arrange());
    QVERIFY(surface(u"p1"_s)->view.hidden);
    QVERIFY(!surface(u"p1"_s)->view.arrange); // hidden pets are not arranged; the daemon's mode is kept and comes back with them
    daemon.emitSignal(u"PetsHiddenChanged"_s, {false});
    QVERIFY(surface(u"p1"_s)->view.arrange);
    daemon.emitSignal(u"PetsHiddenChanged"_s, {true});

    // the daemon goes away: the states are its own, nothing could end them any more, so the pets come back
    daemon.availabilityChanged(false);
    QVERIFY(!controller->petsHidden());
    QVERIFY(!controller->arrange());
    QVERIFY(!surface(u"p1"_s)->view.hidden);

    // and it returns having restarted (both off) or with the states still on (the overlay restarted): read, not assumed
    daemon.arrange = false;
    daemon.petsHidden = false;
    daemon.availabilityChanged(true);
    QTest::qWait(60);
    QVERIFY(!controller->petsHidden() && !controller->arrange());
    daemon.availabilityChanged(false);
    daemon.petsHidden = true;
    daemon.availabilityChanged(true);
    QTRY_VERIFY(controller->petsHidden());
    QVERIFY(surface(u"p1"_s)->view.hidden);

    // a daemon from before the property: shown
    daemon.availabilityChanged(false);
    daemon.hiddenReadable = false;
    daemon.availabilityChanged(true);
    QTest::qWait(60);
    QVERIFY(!controller->petsHidden());
}

void TstOverlayController::theRuntimeLayerHidesMovesAndReAnimatesOnePet()
{
    QJsonObject two = animatedItem(u"anima:1"_s, u"/tmp/a.apng"_s);
    two[u"variants"_s] = QJsonArray{two[u"variants"_s].toArray().at(0), QJsonObject{{u"name"_s, u"wave"_s}, {u"file"_s, u"/tmp/b.apng"_s}, {u"width"_s, 400}, {u"height"_s, 300}}};
    daemon.items[u"anima:1"_s] = two;
    daemon.pets = QJsonArray{placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), placement(u"p2"_s, u"anima:1"_s, u"DP-1"_s, 300)};
    auto controller = makeController();
    connectDaemon(*controller);
    QCOMPARE(controller->surfaceCount(), 2);
    // p1 is hidden by a timeline: its window is unmapped and paused, p2 is untouched, nothing is said about "all the pets"
    daemon.runtime = u"{\"p1\":{\"hidden\":true}}"_s;
    daemon.emitSignal(u"PetRuntimeChanged"_s, {u"p1"_s});
    QTRY_VERIFY(surface(u"p1"_s)->view.hidden);
    QVERIFY(surface(u"p1"_s)->view.paused);
    QVERIFY(!surface(u"p2"_s)->view.hidden);
    QVERIFY(!controller->petsHidden());
    QVERIFY(controller->describe().at(0).toObject().value(u"runtimeHidden"_s).toBool());
    QVERIFY(!controller->describe().at(1).toObject().contains(u"runtimeHidden"_s));
    // a position, an opacity and a variant on top of the stored placement (which the daemon still says is the old one)
    daemon.runtime = u"{\"p1\":{\"x\":500,\"y\":400,\"opacity\":0.5,\"variant\":\"wave\"},\"p2\":{\"variant\":\"nope\"}}"_s;
    daemon.emitSignal(u"PetRuntimeChanged"_s, {u"p1"_s});
    QTRY_VERIFY(!surface(u"p1"_s)->view.hidden);
    QCOMPARE(surface(u"p1"_s)->view.placement.x, 500.0);
    QCOMPARE(surface(u"p1"_s)->view.placement.y, 400.0);
    QCOMPARE(surface(u"p1"_s)->view.placement.opacity, 0.5);
    QCOMPARE(surface(u"p1"_s)->view.placement.variant, u"wave"_s);
    QCOMPARE(surface(u"p2"_s)->view.placement.variant, u"idle"_s); // a variant the item does not have is not shown
    QCOMPARE(surface(u"p2"_s)->view.placement.x, 300.0);
    // the daemon stores the old values: a new list keeps the layer on top
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_COMPARE(surface(u"p1"_s)->view.placement.x, 500.0);
    // cleared: back to what is stored
    daemon.runtime = u"{}"_s;
    daemon.emitSignal(u"PetRuntimeChanged"_s, {QString()});
    QTRY_COMPARE(surface(u"p1"_s)->view.placement.x, 10.0);
    QCOMPARE(surface(u"p1"_s)->view.placement.variant, u"idle"_s);
    QCOMPARE(surface(u"p1"_s)->view.placement.opacity, 0.8);
}

void TstOverlayController::aMotionIsDrawnByTheOverlayAndEndsOnItsOwn()
{
    daemon.pets = QJsonArray{placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s, 0)};
    auto controller = makeController();
    connectDaemon(*controller);
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const qint64 now = qint64(ts.tv_sec) * 1000 + ts.tv_nsec / 1'000'000;
    daemon.runtime = QStringLiteral(R"({"p1":{"x":300,"y":20,"motion":{"fromX":0,"fromY":20,"toX":300,"toY":20,"startMs":%1,"durationMs":300}}})").arg(now);
    daemon.emitSignal(u"PetRuntimeChanged"_s, {u"p1"_s});
    FakeSurface *s = nullptr;
    QTRY_VERIFY((s = surface(u"p1"_s), s && s->view.placement.x > 0));
    const int appliedAtStart = s->applied;
    QTRY_COMPARE_WITH_TIMEOUT(s->view.placement.x, 300.0, 3000); // it ends where it was going
    QVERIFY(s->applied - appliedAtStart >= 3);                      // and was drawn on the way, not teleported
    // the daemon was told nothing: the overlay animates alone
    QCOMPARE(daemon.callCount(u"UpdatePet"_s), 0);
    // when it is over nothing ticks any more
    const int applied = s->applied;
    QTest::qWait(150);
    QCOMPARE(s->applied, applied);
}

void TstOverlayController::theUsersHandWinsOverTheRuntimeLayer()
{
    daemon.pets = QJsonArray{placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s, 10)};
    auto controller = makeController();
    connectDaemon(*controller);
    daemon.runtime = u"{\"p1\":{\"x\":500,\"y\":400}}"_s;
    daemon.emitSignal(u"PetRuntimeChanged"_s, {u"p1"_s});
    QTRY_COMPARE(surface(u"p1"_s)->view.placement.x, 500.0);
    // the user drags it: the override is dropped here and in the daemon, and the edit is stored
    surface(u"p1"_s)->m_callbacks.edited(QJsonObject{{u"x"_s, 42.0}, {u"y"_s, 43.0}});
    QCOMPARE(daemon.callCount(u"ClearPetRuntime"_s), 1);
    QCOMPARE(daemon.lastArgs(u"ClearPetRuntime"_s).value(0).toString(), u"p1"_s);
    QCOMPARE(daemon.callCount(u"UpdatePet"_s), 1);
    // the daemon's answer to the edit arrives as a new list: the stored position shows, not the old override
    daemon.pets = QJsonArray{placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s, 42)};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_COMPARE(surface(u"p1"_s)->view.placement.x, 42.0);
    // a hidden-only layer is not an override of the position: an edit does not clear it
    daemon.runtime = u"{\"p1\":{\"hidden\":true}}"_s;
    daemon.emitSignal(u"PetRuntimeChanged"_s, {u"p1"_s});
    QTRY_VERIFY(surface(u"p1"_s)->view.hidden);
    surface(u"p1"_s)->m_callbacks.edited(QJsonObject{{u"x"_s, 1.0}});
    QCOMPARE(daemon.callCount(u"ClearPetRuntime"_s), 1);
}

void TstOverlayController::theRuntimeLayerIsTheDaemonsAndGoesWithIt()
{
    daemon.pets = QJsonArray{placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s)};
    daemon.runtime = u"{\"p1\":{\"hidden\":true}}"_s;
    auto controller = makeController();
    connectDaemon(*controller);
    QTRY_VERIFY(surface(u"p1"_s)->view.hidden); // read on connection
    // the daemon goes away: nobody could end the layer any more, so the pets come back
    daemon.availabilityChanged(false);
    QVERIFY(!surface(u"p1"_s)->view.hidden);
    // it returns without a layer (it restarted)
    daemon.runtime = u"{}"_s;
    daemon.availabilityChanged(true);
    QTRY_VERIFY(!surface(u"p1"_s)->view.hidden);
    // a daemon older than revision 18 has no such property
    daemon.runtimeReadable = false;
    daemon.availabilityChanged(false);
    daemon.availabilityChanged(true);
    QTRY_COMPARE(controller->surfaceCount(), 1);
    QVERIFY(!surface(u"p1"_s)->view.hidden);
    // a bad entry costs that pet its entry, not the others theirs
    daemon.runtimeReadable = true;
    daemon.runtime = u"{\"p1\":{\"opacity\":9},\"zz\":3}"_s;
    daemon.emitSignal(u"PetRuntimeChanged"_s, {QString()});
    QTest::qWait(30);
    QCOMPARE(surface(u"p1"_s)->view.placement.opacity, 0.8);
}

void TstOverlayController::aTimelineCanHideAStageButNotMoveIt()
{
    daemon.pets = QJsonArray{asStage(placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), u"output"_s)};
    auto controller = makeController();
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {}, twoOutputs());
    connectDaemon(*controller);
    QVERIFY(surface(u"p1"_s)->view.stage.active);
    const QRectF rect = surface(u"p1"_s)->view.stage.petRect;
    // a move: ignored (a stage fills its screens), and Windows() says so
    daemon.runtime = u"{\"p1\":{\"x\":500,\"y\":400}}"_s;
    daemon.emitSignal(u"PetRuntimeChanged"_s, {u"p1"_s});
    QTRY_VERIFY(controller->describe().at(0).toObject().value(u"runtimeMoveIgnored"_s).toBool());
    QCOMPARE(surface(u"p1"_s)->view.stage.petRect, rect);
    QVERIFY(surface(u"p1"_s)->view.stage.active);
    // a hide: the stage window is unmapped and paused, and the reason is "hidden"
    daemon.runtime = u"{\"p1\":{\"hidden\":true}}"_s;
    daemon.emitSignal(u"PetRuntimeChanged"_s, {u"p1"_s});
    QTRY_VERIFY(surface(u"p1"_s)->view.hidden);
    QVERIFY(surface(u"p1"_s)->view.paused);
    QCOMPARE(controller->describe().at(0).toObject().value(u"stageHidden"_s).toString(), u"hidden"_s);
    // and it comes back
    daemon.runtime = u"{}"_s;
    daemon.emitSignal(u"PetRuntimeChanged"_s, {u"p1"_s});
    QTRY_VERIFY(!surface(u"p1"_s)->view.hidden);
    QCOMPARE(controller->describe().at(0).toObject().value(u"stageHidden"_s).toString(), QString());
}

void TstOverlayController::theShortcutsCallTheDaemon()
{
    auto controller = makeController();
    connectDaemon(*controller);
    controller->onShortcut(u"toggle-hidden"_s);
    QTRY_COMPARE(daemon.callCount(u"SetPetsHidden"_s), 1);
    QCOMPARE(daemon.lastArgs(u"SetPetsHidden"_s).value(0).toBool(), true);
    daemon.emitSignal(u"PetsHiddenChanged"_s, {true});
    controller->onShortcut(u"toggle-hidden"_s);
    QTRY_COMPARE(daemon.callCount(u"SetPetsHidden"_s), 2);
    QCOMPARE(daemon.lastArgs(u"SetPetsHidden"_s).value(0).toBool(), false);
    controller->onShortcut(u"rescue"_s);
    QTRY_COMPARE(daemon.callCount(u"RescuePets"_s), 1);
    controller->onShortcut(u"toggle-arrange"_s);
    QTRY_COMPARE(daemon.callCount(u"SetPetsArrange"_s), 1);
    controller->onShortcut(u"something-else"_s); // ignored
    QTest::qWait(30);
    QCOMPARE(daemon.callCount(u"SetPetsHidden"_s), 2);
    QCOMPARE(daemon.callCount(u"RescuePets"_s), 1);
    QCOMPARE(daemon.callCount(u"SetPetsArrange"_s), 1);
    // with no daemon there is nobody to call
    daemon.availabilityChanged(false);
    controller->onShortcut(u"rescue"_s);
    QTest::qWait(30);
    QCOMPARE(daemon.callCount(u"RescuePets"_s), 1);
}

void TstOverlayController::keepAboveReachesTheWindowAndItsDegradeIsReported()
{
    QJsonObject below = placement(u"p2"_s, u"anima:1"_s, u"DP-1"_s, 300);
    below[u"keepAbove"_s] = false;
    below[u"rotation"_s] = 33.5;
    below[u"flipH"_s] = true;
    daemon.pets = QJsonArray{placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), below};
    auto controller = makeController();
    connectDaemon(*controller);
    // the view carries the fields the window acts on
    QVERIFY(surface(u"p1"_s)->view.placement.keepAbove);
    QVERIFY(!surface(u"p2"_s)->view.placement.keepAbove);
    QCOMPARE(surface(u"p2"_s)->view.placement.rotation, 33.5);
    QVERIFY(surface(u"p2"_s)->view.placement.flipH);
    // and Windows() reports what the window says it does less than asked
    const QJsonArray described = controller->describe();
    QCOMPARE(described.at(0).toObject().value(u"degraded"_s).toArray().size(), 0);
    QCOMPARE(described.at(1).toObject().value(u"degraded"_s).toArray(), (QJsonArray{u"keep above off: always below windows"_s}));
    // turned back on, the name goes
    below[u"keepAbove"_s] = true;
    daemon.pets = QJsonArray{placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), below};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_VERIFY(surface(u"p2"_s)->view.placement.keepAbove);
    QCOMPARE(controller->describe().at(1).toObject().value(u"degraded"_s).toArray().size(), 0);
}

void TstOverlayController::theBehaviourFieldsOfAWrongOrOldPayloadFallBackToTheDefaults()
{
    // a daemon from before revision 12 sends none of them; a bad one sends the wrong type: either way the pet behaves as every pet did
    QJsonObject wrong = placement(u"wrong"_s, u"anima:1"_s, u"DP-1"_s, 300);
    wrong[u"clickThrough"_s] = u"no"_s;
    wrong[u"keepAbove"_s] = 0;
    wrong[u"locked"_s] = u"yes"_s;
    wrong[u"hoverHide"_s] = 1;
    wrong[u"rotation"_s] = u"90"_s;
    wrong[u"flipH"_s] = QJsonArray();
    wrong[u"smartDodge"_s] = QJsonObject();
    QJsonObject wild = placement(u"wild"_s, u"anima:1"_s, u"DP-1"_s, 400);
    wild[u"rotation"_s] = 1e30; // not a sane angle
    QJsonObject negative = placement(u"neg"_s, u"anima:1"_s, u"DP-1"_s, 500);
    negative[u"rotation"_s] = -90;
    daemon.pets = QJsonArray{placement(u"old"_s, u"anima:1"_s, u"DP-1"_s), wrong, wild, negative};
    auto controller = makeController();
    connectDaemon(*controller);
    for (const char *id : {"old", "wrong"}) {
        const PetPlacement &p = surface(QLatin1String(id))->view.placement;
        QVERIFY2(p.clickThrough && p.keepAbove, id);
        QVERIFY2(!p.locked && !p.hoverHide && !p.focusFade && !p.smartDodge && !p.flipH && !p.flipV, id);
        QCOMPARE(p.rotation, 0.0);
    }
    QCOMPARE(surface(u"wild"_s)->view.placement.rotation, 0.0);
    QCOMPARE(surface(u"neg"_s)->view.placement.rotation, 270.0); // wrapped
}

void TstOverlayController::sessionPolicyPausesEveryPet()
{
    QJsonObject paused = placement(u"p2"_s, u"anima:1"_s, u"DP-1"_s);
    paused[u"paused"_s] = true;
    daemon.pets = QJsonArray{placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), paused};
    auto controller = makeController();
    connectDaemon(*controller);
    QVERIFY(!surface(u"p1"_s)->view.paused);
    QVERIFY(surface(u"p2"_s)->view.paused); // paused by its own placement
    controller->setSessionPaused(true);     // locked or screen off
    QVERIFY(surface(u"p1"_s)->view.paused);
    controller->setSessionPaused(false);
    QVERIFY(!surface(u"p1"_s)->view.paused);
    QVERIFY(surface(u"p2"_s)->view.paused);
    // Nothing else (fullscreen apps, for one) pauses a pet: there is no input for it.
}

void TstOverlayController::editsGoToTheDaemon()
{
    auto controller = makeController();
    connectDaemon(*controller);
    FakeSurface *pet = surface(u"p1"_s);
    // The surface reports through the callbacks the controller installed.
    pet->m_callbacks.edited(QJsonObject{{u"x"_s, 5}, {u"y"_s, 6}, {u"width"_s, 120}});
    QTRY_COMPARE(daemon.callCount(u"UpdatePet"_s), 1);
    const QVariantList args = daemon.lastArgs(u"UpdatePet"_s);
    QCOMPARE(args.value(0).toString(), u"p1"_s);
    const QJsonObject sent = QJsonDocument::fromJson(args.value(1).toString().toUtf8()).object();
    QCOMPARE(sent.value(u"x"_s).toInt(), 5);
    QCOMPARE(sent.value(u"width"_s).toInt(), 120);

    pet->m_callbacks.removeRequested();
    QTRY_COMPARE(daemon.callCount(u"RemovePet"_s), 1);
    QCOMPARE(daemon.lastArgs(u"RemovePet"_s).value(0).toString(), u"p1"_s);
}

void TstOverlayController::removedPetsLoseTheirSurface()
{
    daemon.pets = QJsonArray{placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), placement(u"p2"_s, u"anima:1"_s, u"DP-1"_s)};
    auto controller = makeController();
    connectDaemon(*controller);
    QCOMPARE(surfaces.size(), size_t(2));
    daemon.pets = QJsonArray{placement(u"p2"_s, u"anima:1"_s, u"DP-1"_s, 77)};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_COMPARE(surfaces.size(), size_t(1));
    QVERIFY(surface(u"p2"_s));
    QTRY_COMPARE(surface(u"p2"_s)->view.placement.x, 77.0); // and the survivor got the new placement
    QCOMPARE(controller->describe().size(), 1);
}

void TstOverlayController::unknownItemsShowNothing()
{
    daemon.pets = QJsonArray{placement(u"p1"_s, u"anima:404"_s, u"DP-1"_s)};
    auto controller = makeController();
    controller->start();
    daemon.availabilityChanged(true);
    QTRY_VERIFY(daemon.callCount(u"GetItem"_s) >= 1);
    QTest::qWait(30);
    QVERIFY(surfaces.empty());
    QCOMPARE(controller->describe().size(), 1);
    QCOMPARE(controller->describe().at(0).toObject().value(u"shown"_s).toBool(), false);
}

void TstOverlayController::oldDaemonsShowNothing()
{
    daemon.apiVersion = 4;
    auto controller = makeController();
    QSignalSpy incompatible(controller.get(), &OverlayController::incompatibleDaemon);
    controller->start();
    daemon.availabilityChanged(true);
    QTRY_COMPARE(incompatible.count(), 1);
    QCOMPARE(incompatible.at(0).at(0).toUInt(), 4u);
    QVERIFY(surfaces.empty());
    QCOMPARE(daemon.callCount(u"ListPets"_s), 0);
}

void TstOverlayController::petsStayWhenTheDaemonLeaves()
{
    auto controller = makeController();
    connectDaemon(*controller);
    QCOMPARE(surfaces.size(), size_t(1));
    daemon.availabilityChanged(false);
    QCOMPARE(surfaces.size(), size_t(1));
    daemon.pets = QJsonArray{};
    daemon.availabilityChanged(true); // the new daemon has no pets
    QTRY_VERIFY(surfaces.empty());
}

void TstOverlayController::failingFilesAreGivenUp()
{
    auto controller = makeController();
    connectDaemon(*controller);
    for (int i = 0; i < 3; ++i) {
        FakeSurface *pet = surface(u"p1"_s);
        QVERIFY(pet);
        const int serial = pet->serial;
        pet->m_callbacks.failed(u"unrecognized file format"_s);
        if (i < 2)
            QTRY_VERIFY_WITH_TIMEOUT(surface(u"p1"_s) != nullptr && surface(u"p1"_s)->serial != serial, 4000); // retried
    }
    QTRY_VERIFY(surfaces.empty()); // the third failure keeps the placement but drops the window
    QCOMPARE(controller->describe().size(), 1);
    QCOMPARE(controller->describe().at(0).toObject().value(u"failures"_s).toInt(), 3);

    // A different file (the transcode finished) gets another chance.
    daemon.items[u"anima:1"_s] = item(original, u"done"_s, transcoded);
    daemon.emitSignal(u"TranscodeChanged"_s, {u"anima:1"_s});
    QTRY_VERIFY(surface(u"p1"_s) != nullptr);
    QCOMPARE(surface(u"p1"_s)->view.file, transcoded);
}

void TstOverlayController::describeListsTheWindows()
{
    auto controller = makeController();
    connectDaemon(*controller);
    const QJsonArray described = controller->describe();
    QCOMPARE(described.size(), 1);
    const QJsonObject window = described.at(0).toObject();
    QCOMPARE(window.value(u"petId"_s).toString(), u"p1"_s);
    QCOMPARE(window.value(u"output"_s).toString(), u"DP-1"_s);
    QCOMPARE(window.value(u"shown"_s).toBool(), true);
    QCOMPARE(window.value(u"x"_s).toDouble(), 10.0);
    QCOMPARE(window.value(u"interactive"_s).toBool(), false);
    QCOMPARE(window.value(u"file"_s).toString(), original);
}

void TstOverlayController::arrangeEndsWhenTheDaemonVanishes()
{
    daemon.arrange = true;
    auto controller = makeController();
    connectDaemon(*controller);
    QTRY_VERIFY(surface(u"p1"_s)->view.arrange);
    daemon.availabilityChanged(false); // nobody can switch it off now, and a new daemon starts with it off
    QVERIFY(!controller->arrange());
    QVERIFY(!surface(u"p1"_s)->view.arrange);
    QVERIFY(!surface(u"p1"_s)->inputEnabled());
    daemon.arrange = true; // the daemon is back and still (or again) arranging: the real value wins
    daemon.availabilityChanged(true);
    QTRY_VERIFY(surface(u"p1"_s)->view.arrange);
}

void TstOverlayController::transientItemErrorsKeepThePet()
{
    auto controller = makeController();
    connectDaemon(*controller);
    QVERIFY(surface(u"p1"_s));
    // The transcode finishes while the daemon is too busy to answer GetItem: the pet stays as it is.
    daemon.items[u"anima:1"_s] = item(original, u"done"_s, transcoded);
    daemon.transientItemErrors = 1;
    daemon.emitSignal(u"TranscodeChanged"_s, {u"anima:1"_s});
    QTRY_COMPARE(daemon.callCount(u"GetItem"_s), 2);
    QTest::qWait(50);
    QVERIFY(surface(u"p1"_s));
    QCOMPARE(surface(u"p1"_s)->view.file, original);
    // ... and the retry brings the news.
    QTRY_COMPARE_WITH_TIMEOUT(surface(u"p1"_s)->view.file, transcoded, 5000);
    QVERIFY(!surfaces.empty());

    // A definitive "no such item" does remove it.
    daemon.items.erase(u"anima:1"_s);
    daemon.emitSignal(u"TranscodeChanged"_s, {u"anima:1"_s});
    QTRY_VERIFY(surfaces.empty());
}

void TstOverlayController::brokenTranscodeFallsBackToTheOriginal()
{
    daemon.items[u"anima:1"_s] = item(original, u"done"_s, transcoded);
    auto controller = makeController();
    connectDaemon(*controller);
    QCOMPARE(surface(u"p1"_s)->view.file, transcoded);
    surface(u"p1"_s)->m_callbacks.failed(u"unrecognized file format"_s);
    // The original takes over at once, without counting against the pet.
    QTRY_VERIFY(surface(u"p1"_s) != nullptr && surface(u"p1"_s)->view.file == original);
    QCOMPARE(controller->describe().at(0).toObject().value(u"failures"_s).toInt(), 0);
    // The broken copy is not tried again when the transcode state is announced again.
    daemon.emitSignal(u"TranscodeChanged"_s, {u"anima:1"_s});
    QTRY_COMPARE(daemon.callCount(u"GetItem"_s), 2);
    QTest::qWait(50);
    QCOMPARE(surface(u"p1"_s)->view.file, original);
    // A new conversion (another file) is believed again.
    const QString again = tmp.filePath(u"pet2.webm"_s);
    testutil::writeFile(again, "z");
    daemon.items[u"anima:1"_s] = item(original, u"done"_s, again);
    daemon.emitSignal(u"TranscodeChanged"_s, {u"anima:1"_s});
    QTRY_COMPARE(surface(u"p1"_s)->view.file, again);
}

void TstOverlayController::aGoodFrameResetsTheFailures()
{
    auto controller = makeController();
    connectDaemon(*controller);
    for (int round = 0; round < 2; ++round) {
        // Two failures, then a frame: never reaches the limit of three, however long the pet lives.
        for (int i = 0; i < 2; ++i) {
            FakeSurface *pet = surface(u"p1"_s);
            QVERIFY(pet);
            const int serial = pet->serial;
            pet->m_callbacks.failed(u"decoder hiccup"_s);
            QTRY_VERIFY_WITH_TIMEOUT(surface(u"p1"_s) != nullptr && surface(u"p1"_s)->serial != serial, 4000);
        }
        surface(u"p1"_s)->m_callbacks.started();
        QCOMPARE(controller->describe().at(0).toObject().value(u"failures"_s).toInt(), 0);
    }
    QVERIFY(surface(u"p1"_s));
}

void TstOverlayController::aCrashingFileIsBlockedAcrossRestarts()
{
    const QString stateDir = tmp.filePath(u"state/overlay"_s);
    const auto markers = [&] { return QDir(stateDir).entryList({u"opening-*.json"_s}, QDir::Files); };
    // Three runs die while opening the file: the marker is still there when the process is gone.
    for (int run = 0; run < CrashLedger::kMaxCrashes; ++run) {
        CrashLedger runLedger(stateDir);
        runLedger.load();
        QCOMPARE(runLedger.crashes(original), run);
        ledger = &runLedger;
        QByteArray marker;
        {
            auto controller = makeController();
            connectDaemon(*controller);
            QVERIFY2(surface(u"p1"_s), "the file is opened until it is blocked");
            QCOMPARE(markers().size(), 1);
            QFile file(stateDir + u'/' + markers().first());
            QVERIFY(file.open(QIODevice::ReadOnly));
            marker = file.readAll();
        } // a clean shutdown removes the marker...
        QCOMPARE(markers().size(), 0);
        ledger = nullptr;
        testutil::writeFile(stateDir + u"/opening-crashed.json"_s, marker); // ...a crash does not
    }

    CrashLedger last(stateDir);
    last.load();
    QVERIFY(last.blocked(original));
    QCOMPARE(markers().size(), 0); // consumed by load()
    ledger = &last;
    auto controller = makeController();
    connectDaemon(*controller);
    QVERIFY(surfaces.empty());
    const QJsonObject described = controller->describe().at(0).toObject();
    QCOMPARE(described.value(u"shown"_s).toBool(), false);
    QCOMPARE(described.value(u"failures"_s).toInt(), CrashLedger::kMaxCrashes);

    // Another file for the variant (the transcode finished) gets a chance...
    daemon.items[u"anima:1"_s] = item(original, u"done"_s, transcoded);
    daemon.emitSignal(u"TranscodeChanged"_s, {u"anima:1"_s});
    QTRY_VERIFY(surface(u"p1"_s) != nullptr);
    QCOMPARE(surface(u"p1"_s)->view.file, transcoded);
    controller.reset();
    ledger = nullptr;

    // ...and so does the same path once the file itself has changed.
    testutil::writeFile(original, "a different, longer file");
    CrashLedger changed(stateDir);
    changed.load();
    QVERIFY(!changed.blocked(original));
}

void TstOverlayController::theOpeningMarkerFollowsTheWindow()
{
    const QString stateDir = tmp.filePath(u"state2"_s);
    const auto markers = [&] { return QDir(stateDir).entryList({u"opening-*.json"_s}, QDir::Files); };
    CrashLedger runLedger(stateDir);
    runLedger.load();
    ledger = &runLedger;
    auto controller = makeController();
    connectDaemon(*controller);
    QCOMPARE(markers().size(), 1);
    surface(u"p1"_s)->m_callbacks.started(); // the file plays
    QCOMPARE(markers().size(), 0);

    // A transcode arriving opens another file: guarded again, cleared again.
    daemon.items[u"anima:1"_s] = item(original, u"done"_s, transcoded);
    daemon.emitSignal(u"TranscodeChanged"_s, {u"anima:1"_s});
    QTRY_COMPARE(surface(u"p1"_s)->view.file, transcoded);
    QCOMPARE(markers().size(), 1);
    surface(u"p1"_s)->m_callbacks.failed(u"unrecognized file format"_s); // a clean failure is not a crash
    QCOMPARE(markers().size(), 0);

    // Removing the placement closes the window and leaves nothing behind.
    QTRY_VERIFY(surface(u"p1"_s) != nullptr && surface(u"p1"_s)->view.file == original);
    QCOMPARE(markers().size(), 1);
    daemon.pets = QJsonArray{};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_VERIFY(surfaces.empty());
    QCOMPARE(markers().size(), 0);
    ledger = nullptr;
}

void TstOverlayController::resetFailuresGivesABlockedPetAnotherChance()
{
    auto controller = makeController();
    connectDaemon(*controller);
    QVERIFY(!controller->resetFailures(u"nope"_s));
    QSignalSpy changed(controller.get(), &OverlayController::stateChanged);
    for (int i = 0; i < 3; ++i) {
        FakeSurface *pet = surface(u"p1"_s);
        QVERIFY(pet);
        const int serial = pet->serial;
        pet->m_callbacks.failed(u"unrecognized file format"_s);
        if (i < 2)
            QTRY_VERIFY_WITH_TIMEOUT(surface(u"p1"_s) != nullptr && surface(u"p1"_s)->serial != serial, 4000);
    }
    QTRY_VERIFY(surfaces.empty());
    QJsonObject described = controller->describe().at(0).toObject();
    QCOMPARE(described.value(u"blocked"_s).toBool(), true);
    QCOMPARE(described.value(u"error"_s).toString(), u"unrecognized file format"_s);

    QVERIFY(controller->resetFailures(u"p1"_s)); // "Try again"
    QVERIFY(surface(u"p1"_s));
    described = controller->describe().at(0).toObject();
    QCOMPARE(described.value(u"blocked"_s).toBool(), false);
    QCOMPARE(described.value(u"failures"_s).toInt(), 0);
    QVERIFY(!described.contains(u"error"_s));
}

namespace {
QJsonObject itemWithId(const QString &id, const QString &file)
{
    QJsonObject object = item(file);
    object[u"id"_s] = id;
    return object;
}
} // namespace

void TstOverlayController::petsOpenTheirFilesOneAtATime()
{
    const QString second = tmp.filePath(u"other.apng"_s);
    testutil::writeFile(second, "second");
    daemon.pets = QJsonArray{placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), placement(u"p2"_s, u"anima:2"_s, u"DP-1"_s)};
    daemon.items[u"anima:2"_s] = itemWithId(u"anima:2"_s, second);
    const QString stateDir = tmp.filePath(u"serial"_s);
    const auto markers = [&] { return QDir(stateDir).entryList({u"opening-*.json"_s}, QDir::Files); };
    CrashLedger runLedger(stateDir);
    runLedger.load();
    ledger = &runLedger;
    auto controller = makeController();
    connectDaemon(*controller);
    QTRY_COMPARE(surfaces.size(), size_t(1));
    QTest::qWait(50);
    QCOMPARE(surfaces.size(), size_t(1)); // the other waits while this one opens its file
    QCOMPARE(markers().size(), 1);
    const QString firstId = surfaces.front()->petId;
    const QString secondId = firstId == u"p1"_s ? u"p2"_s : u"p1"_s;

    surface(firstId)->m_callbacks.started();
    QTRY_VERIFY(surface(secondId) != nullptr);
    QCOMPARE(markers().size(), 1); // now the second one's
    surface(secondId)->m_callbacks.failed(u"unrecognized file format"_s); // a failure also ends its turn
    QTRY_COMPARE(markers().size(), 0);
    ledger = nullptr;
}

void TstOverlayController::aCrashIsChargedToTheOneFileThatWasOpening()
{
    const QString second = tmp.filePath(u"other.apng"_s);
    testutil::writeFile(second, "second");
    daemon.pets = QJsonArray{placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), placement(u"p2"_s, u"anima:2"_s, u"DP-1"_s)};
    daemon.items[u"anima:2"_s] = itemWithId(u"anima:2"_s, second);
    const QString stateDir = tmp.filePath(u"blame"_s);
    QByteArray marker;
    QString opened;
    {
        CrashLedger runLedger(stateDir);
        runLedger.load();
        ledger = &runLedger;
        auto controller = makeController();
        connectDaemon(*controller);
        QTRY_COMPARE(surfaces.size(), size_t(1));
        QTest::qWait(50);
        opened = surfaces.front()->petId == u"p1"_s ? original : second;
        const QStringList found = QDir(stateDir).entryList({u"opening-*.json"_s}, QDir::Files);
        QCOMPARE(found.size(), 1); // only the file being opened, not the one waiting
        QFile file(stateDir + u'/' + found.first());
        QVERIFY(file.open(QIODevice::ReadOnly));
        marker = file.readAll();
        controller.reset();
        ledger = nullptr;
    }
    testutil::writeFile(stateDir + u"/opening-crashed.json"_s, marker);
    CrashLedger after(stateDir);
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"ended while opening"_s));
    after.load();
    QCOMPARE(after.crashes(opened), 1);
    QCOMPARE(after.crashes(opened == original ? second : original), 0);
}

void TstOverlayController::aSilentWindowIsNotWaitedForForever()
{
    const QString second = tmp.filePath(u"other.apng"_s);
    testutil::writeFile(second, "second");
    daemon.pets = QJsonArray{placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), placement(u"p2"_s, u"anima:2"_s, u"DP-1"_s)};
    daemon.items[u"anima:2"_s] = itemWithId(u"anima:2"_s, second);
    CrashLedger runLedger(tmp.filePath(u"silent"_s));
    runLedger.load();
    ledger = &runLedger;
    auto controller = makeController();
    connectDaemon(*controller);
    QTRY_COMPARE(surfaces.size(), size_t(1));
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"slow to open"_s));
    QTRY_COMPARE_WITH_TIMEOUT(surfaces.size(), size_t(2), 13000); // never started: the others go on after 10 s
    ledger = nullptr;
}

void TstOverlayController::pausedPetsDoNotHoldTheOthersBack()
{
    // The overlay starts while the session is locked (a restart overnight): nothing renders, so no window ever says
    // its file started. Each pet used to hold the next one back for 10 s.
    const QString second = tmp.filePath(u"other.apng"_s);
    testutil::writeFile(second, "second");
    daemon.pets = QJsonArray{placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), placement(u"p2"_s, u"anima:2"_s, u"DP-1"_s)};
    daemon.items[u"anima:2"_s] = itemWithId(u"anima:2"_s, second);
    const QString stateDir = tmp.filePath(u"locked"_s);
    const auto markers = [&] { return QDir(stateDir).entryList({u"opening-*.json"_s}, QDir::Files); };
    CrashLedger runLedger(stateDir);
    runLedger.load();
    ledger = &runLedger;
    const auto resetLedger = qScopeGuard([this] { ledger = nullptr; });
    auto controller = makeController();
    controller->setSessionPaused(true);
    connectDaemon(*controller);
    QTRY_COMPARE(surfaces.size(), size_t(1));
    QElapsedTimer timer;
    timer.start();
    QTRY_COMPARE_WITH_TIMEOUT(surfaces.size(), size_t(2), 4000); // after about a second, not ten
    QVERIFY(timer.elapsed() < 4000);
    QTRY_COMPARE_WITH_TIMEOUT(markers().size(), 1, 2000); // the second pet's turn, which ends the same way
    QTRY_COMPARE_WITH_TIMEOUT(markers().size(), 0, 4000);
    // Unlocked, the first frames arrive late and are taken as usual.
    controller->setSessionPaused(false);
    surface(u"p1"_s)->m_callbacks.started();
    QCOMPARE(controller->describe().at(0).toObject().value(u"failures"_s).toInt(), 0);
}

void TstOverlayController::picksTheSmallestFinishedBucketThatFits()
{
    const QString b256 = tmp.filePath(u"b256.webm"_s);
    const QString b384 = tmp.filePath(u"b384.webm"_s);
    const QString b650 = tmp.filePath(u"b650.webm"_s);
    for (const QString &file : {b256, b384, b650})
        testutil::writeFile(file, "z");
    // 200 px wide at scale 1 needs 200 px: bucket 256.
    daemon.items[u"anima:1"_s] = bucketItem(original, {{384, b384}, {650, b650}}, {{256, QString()}});
    auto controller = makeController();
    connectDaemon(*controller);
    QCOMPARE(surface(u"p1"_s)->view.file, b384); // 256 is still converting: the smallest finished one that fits
    QCOMPARE(surface(u"p1"_s)->view.bucket, 384);
    QCOMPARE(controller->describe().at(0).toObject().value(u"bucket"_s).toInt(), 384);

    daemon.items[u"anima:1"_s] = bucketItem(original, {{256, b256}, {384, b384}, {650, b650}});
    daemon.emitSignal(u"TranscodeChanged"_s, {u"anima:1"_s});
    QTRY_COMPARE(surface(u"p1"_s)->view.file, b256);
    QCOMPARE(controller->describe().at(0).toObject().value(u"bucket"_s).toInt(), 256);

    // Only smaller ones are finished and none fits: the largest there is, not the original.
    daemon.pets = QJsonArray{[&] {
        QJsonObject big = placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s);
        big[u"width"_s] = 600;
        return big;
    }()};
    daemon.items[u"anima:1"_s] = bucketItem(original, {{256, b256}, {384, b384}});
    daemon.emitSignal(u"PetsChanged"_s);
    daemon.emitSignal(u"TranscodeChanged"_s, {u"anima:1"_s});
    QTRY_COMPARE(surface(u"p1"_s)->view.file, b384);

    // Nothing finished: the original, bucket 0.
    daemon.items[u"anima:1"_s] = bucketItem(original, {});
    daemon.emitSignal(u"TranscodeChanged"_s, {u"anima:1"_s});
    QTRY_COMPARE(surface(u"p1"_s)->view.file, original);
    QCOMPARE(controller->describe().at(0).toObject().value(u"bucket"_s).toInt(), 0);

    // A copy that vanished is not a candidate.
    daemon.items[u"anima:1"_s] = bucketItem(original, {{384, tmp.filePath(u"gone.webm"_s)}});
    daemon.emitSignal(u"TranscodeChanged"_s, {u"anima:1"_s});
    QTest::qWait(50);
    QCOMPARE(surface(u"p1"_s)->view.file, original);
}

void TstOverlayController::theOutputScaleRaisesTheNeed()
{
    const QString b256 = tmp.filePath(u"b256.webm"_s);
    const QString b650 = tmp.filePath(u"b650.webm"_s);
    testutil::writeFile(b256, "z");
    testutil::writeFile(b650, "z");
    daemon.items[u"anima:1"_s] = bucketItem(original, {{256, b256}, {650, b650}});
    auto controller = makeController();
    connectDaemon(*controller);
    QCOMPARE(surface(u"p1"_s)->view.file, b256); // 200 px at scale 1
    // 200 logical px at scale 2.5 are 500 device pixels: only the 650 copy is sharp enough.
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {{u"DP-1"_s, 2.5}});
    QCOMPARE(surface(u"p1"_s)->view.file, b650);
    // The scale of another output does not matter.
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {{u"HDMI-A-1"_s, 3.0}});
    QCOMPARE(surface(u"p1"_s)->view.file, b256);
}

void TstOverlayController::aBrokenBucketFallsBackToAnotherOne()
{
    const QString b384 = tmp.filePath(u"b384.webm"_s);
    const QString b650 = tmp.filePath(u"b650.webm"_s);
    testutil::writeFile(b384, "z");
    testutil::writeFile(b650, "z");
    daemon.items[u"anima:1"_s] = bucketItem(original, {{384, b384}, {650, b650}});
    auto controller = makeController();
    connectDaemon(*controller);
    QCOMPARE(surface(u"p1"_s)->view.file, b384);
    surface(u"p1"_s)->m_callbacks.failed(u"unrecognized file format"_s);
    QTRY_VERIFY(surface(u"p1"_s) != nullptr && surface(u"p1"_s)->view.file == b650);
    QCOMPARE(controller->describe().at(0).toObject().value(u"failures"_s).toInt(), 0);
}

void TstOverlayController::aRevision5DaemonHasOneCopyAtTheBiggestSize()
{
    // item() carries no "buckets": its "transcode" copy is the 650 one.
    daemon.items[u"anima:1"_s] = item(original, u"done"_s, transcoded);
    auto controller = makeController();
    connectDaemon(*controller);
    QCOMPARE(surface(u"p1"_s)->view.file, transcoded);
    QCOMPARE(controller->describe().at(0).toObject().value(u"bucket"_s).toInt(), 650);
    QCOMPARE(daemon.callCount(u"SetOutputScales"_s), 0); // revision 5 has no such call
}

void TstOverlayController::sendsTheOutputScalesToARevision6Daemon()
{
    daemon.apiVersion = 6;
    auto controller = makeController();
    connectDaemon(*controller);
    QTRY_COMPARE(daemon.callCount(u"SetOutputScales"_s), 1);
    QCOMPARE(QJsonDocument::fromJson(daemon.lastArgs(u"SetOutputScales"_s).at(0).toString().toUtf8()).object(),
             (QJsonObject{{u"DP-1"_s, 1.0}, {u"HDMI-A-1"_s, 1.0}}));

    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {{u"DP-1"_s, 1.25}, {u"HDMI-A-1"_s, 100.0}});
    QCOMPARE(daemon.callCount(u"SetOutputScales"_s), 2);
    QCOMPARE(QJsonDocument::fromJson(daemon.lastArgs(u"SetOutputScales"_s).at(0).toString().toUtf8()).object(),
             (QJsonObject{{u"DP-1"_s, 1.25}, {u"HDMI-A-1"_s, 4.0}})); // clamped to what the daemon accepts

    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {{u"DP-1"_s, 1.25}, {u"HDMI-A-1"_s, 100.0}});
    QCOMPARE(daemon.callCount(u"SetOutputScales"_s), 2); // nothing changed

    controller->setOutputs({u"DP-1"_s}, u"DP-1"_s, {{u"DP-1"_s, 1.25}}); // hotplug
    QCOMPARE(daemon.callCount(u"SetOutputScales"_s), 3);
    QCOMPARE(QJsonDocument::fromJson(daemon.lastArgs(u"SetOutputScales"_s).at(0).toString().toUtf8()).object(),
             (QJsonObject{{u"DP-1"_s, 1.25}}));

    // A restarted daemon is told again.
    daemon.availabilityChanged(false);
    daemon.availabilityChanged(true);
    QTRY_COMPARE(daemon.callCount(u"SetOutputScales"_s), 4);
}

void TstOverlayController::theFrameCacheIsOffByDefault()
{
    daemon.items[u"anima:1"_s] = animatedItem(u"anima:1"_s, original);
    auto controller = makeController();
    connectDaemon(*controller);
    surface(u"p1"_s)->m_callbacks.started();
    QTest::qWait(50);
    QVERIFY(!surface(u"p1"_s)->view.spriteSize.isValid());
    QVERIFY(surface(u"p1"_s)->view.spriteKey.isEmpty());
    QCOMPARE(controller->describe().at(0).toObject().value(u"mode"_s).toString(), u"video"_s);
    QCOMPARE(controller->describe().at(0).toObject().value(u"cacheBytes"_s).toInt(), 0);
}

void TstOverlayController::spritesAreAdmittedBySavingWithinTheBudget()
{
    // 400x300 at 10 frames: shown 200 wide the frames take 10 x 200 x 150 x 4 = 1.2 MB, shown 300 wide 2.7 MB.
    daemon.pets = QJsonArray{placementWidth(u"small"_s, u"anima:1"_s, 200), placementWidth(u"big"_s, u"anima:1"_s, 300)};
    daemon.items[u"anima:1"_s] = animatedItem(u"anima:1"_s, original);
    auto controller = makeController();
    controller->setFrameCacheBudget(3'000'000);
    connectDaemon(*controller);
    QCOMPARE(controller->frameCacheBudget(), qint64(3'000'000));
    // The bigger pet saves more decoding and goes first; the small one no longer fits next to it.
    QCOMPARE(surface(u"big"_s)->view.spriteSize, QSize(300, 225));
    QVERIFY(!surface(u"small"_s)->view.spriteSize.isValid());

    // With room for both, both are cached; and each is allowed what is left after the other.
    controller->setFrameCacheBudget(4'000'000);
    QCOMPARE(surface(u"big"_s)->view.spriteSize, QSize(300, 225));
    QCOMPARE(surface(u"small"_s)->view.spriteSize, QSize(200, 150));
    QCOMPARE(surface(u"big"_s)->view.spriteBudget, qint64(4'000'000 - 1'200'000));
    QCOMPARE(surface(u"small"_s)->view.spriteBudget, qint64(4'000'000 - 2'700'000));

    // Too little for any: all video.
    controller->setFrameCacheBudget(1'000'000);
    QVERIFY(!surface(u"big"_s)->view.spriteSize.isValid());
    QVERIFY(!surface(u"small"_s)->view.spriteSize.isValid());
}

void TstOverlayController::framesAreDecodedOnlyOnceTheVideoPlays()
{
    daemon.items[u"anima:1"_s] = animatedItem(u"anima:1"_s, original);
    auto controller = makeController();
    controller->setFrameCacheBudget(64 << 20);
    connectDaemon(*controller);
    FakeSurface *pet = surface(u"p1"_s);
    QCOMPARE(pet->view.spriteSize, QSize(200, 150));
    QVERIFY2(pet->view.spriteKey.isEmpty(), "the video has not played yet: nothing is decoded");
    pet->m_callbacks.started();
    QTRY_VERIFY(!surface(u"p1"_s)->view.spriteKey.isEmpty());
    QCOMPARE(surface(u"p1"_s)->view.spriteKey, original + u"@200x150"_s);
}

void TstOverlayController::aFinishedDecodeSwitchesTheMode()
{
    daemon.items[u"anima:1"_s] = animatedItem(u"anima:1"_s, original);
    auto controller = makeController();
    controller->setFrameCacheBudget(64 << 20);
    connectDaemon(*controller);
    surface(u"p1"_s)->m_callbacks.started();
    QTRY_VERIFY(!surface(u"p1"_s)->view.spriteKey.isEmpty());
    QCOMPARE(controller->describe().at(0).toObject().value(u"mode"_s).toString(), u"video"_s); // not yet

    surface(u"p1"_s)->m_callbacks.spriteDone(true, 1200000, QString());
    const QJsonObject window = controller->describe().at(0).toObject();
    QCOMPARE(window.value(u"mode"_s).toString(), u"sprite"_s);
    QCOMPARE(window.value(u"cacheBytes"_s).toDouble(), 1200000.0);
    // Leaving the budget takes the frames away again (the window drops them when the size is empty).
    controller->setFrameCacheBudget(0);
    QVERIFY(!surface(u"p1"_s)->view.spriteSize.isValid());
    QCOMPARE(controller->describe().at(0).toObject().value(u"mode"_s).toString(), u"video"_s);
    QCOMPARE(controller->describe().at(0).toObject().value(u"cacheBytes"_s).toInt(), 0);
}

void TstOverlayController::aFailedDecodeKeepsTheVideoAndIsNotRetried()
{
    daemon.items[u"anima:1"_s] = animatedItem(u"anima:1"_s, original);
    auto controller = makeController();
    controller->setFrameCacheBudget(64 << 20);
    connectDaemon(*controller);
    surface(u"p1"_s)->m_callbacks.started();
    QTRY_VERIFY(!surface(u"p1"_s)->view.spriteKey.isEmpty());
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"cannot cache the frames"_s));
    surface(u"p1"_s)->m_callbacks.spriteDone(false, 0, u"over the frame cache budget"_s);
    QTRY_VERIFY(!surface(u"p1"_s)->view.spriteSize.isValid()); // not admitted for this file and size any more
    QCOMPARE(controller->describe().at(0).toObject().value(u"mode"_s).toString(), u"video"_s);
    QCOMPARE(controller->describe().at(0).toObject().value(u"blocked"_s).toBool(), false);
    QCOMPARE(controller->describe().at(0).toObject().value(u"failures"_s).toInt(), 0); // the video is fine
    QTest::qWait(50);
    QVERIFY(surface(u"p1"_s)->view.spriteKey.isEmpty());

    // "Try again" gives it another chance.
    QVERIFY(controller->resetFailures(u"p1"_s));
    QVERIFY(surface(u"p1"_s)->view.spriteSize.isValid());
}

void TstOverlayController::aResizeRebuildsTheFramesAndKeepsTheOldOnesMeanwhile()
{
    daemon.items[u"anima:1"_s] = animatedItem(u"anima:1"_s, original);
    auto controller = makeController();
    controller->setFrameCacheBudget(64 << 20);
    connectDaemon(*controller);
    surface(u"p1"_s)->m_callbacks.started();
    QTRY_VERIFY(!surface(u"p1"_s)->view.spriteKey.isEmpty());
    surface(u"p1"_s)->m_callbacks.spriteDone(true, 1200000, QString());

    daemon.pets = QJsonArray{placementWidth(u"p1"_s, u"anima:1"_s, 300)};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_COMPARE(surface(u"p1"_s)->view.spriteKey, original + u"@300x225"_s);
    // The old frames are still what is shown until the new ones are in.
    QCOMPARE(controller->describe().at(0).toObject().value(u"mode"_s).toString(), u"sprite"_s);
    QCOMPARE(surface(u"p1"_s)->view.spriteSize, QSize(300, 225));
    surface(u"p1"_s)->m_callbacks.spriteDone(true, 2700000, QString());
    QCOMPARE(controller->describe().at(0).toObject().value(u"cacheBytes"_s).toDouble(), 2700000.0);

    // A bigger screen scale asks for more pixels, but never more than the file has.
    controller->setOutputs({u"DP-1"_s, u"HDMI-A-1"_s}, u"DP-1"_s, {{u"DP-1"_s, 1.5}});
    QTRY_COMPARE(surface(u"p1"_s)->view.spriteKey, original + u"@400x300"_s);
}

void TstOverlayController::decodesTakeTurnsWithTheLedger()
{
    const QString second = tmp.filePath(u"other.apng"_s);
    testutil::writeFile(second, "second");
    daemon.pets = QJsonArray{placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s), placement(u"p2"_s, u"anima:2"_s, u"DP-1"_s)};
    daemon.items[u"anima:1"_s] = animatedItem(u"anima:1"_s, original);
    daemon.items[u"anima:2"_s] = animatedItem(u"anima:2"_s, second);
    const QString stateDir = tmp.filePath(u"sprites"_s);
    const auto markers = [&] { return QDir(stateDir).entryList({u"opening-*.json"_s}, QDir::Files); };
    CrashLedger runLedger(stateDir);
    runLedger.load();
    ledger = &runLedger;
    const auto resetLedger = qScopeGuard([this] { ledger = nullptr; });
    auto controller = makeController();
    controller->setFrameCacheBudget(64 << 20);
    connectDaemon(*controller);
    // Each pet plays its video (a window opens its file when it is its turn) and then decodes its frames. Whatever
    // the order the items arrive in, only one file is being opened or decoded at any time, under one marker.
    QSet<QString> started;
    QSet<QString> done;
    QElapsedTimer timer;
    timer.start();
    while (done.size() < 2 && timer.elapsed() < 8000) {
        QTest::qWait(10);
        int decoding = 0;
        for (FakeSurface *s : surfaces)
            decoding += !s->view.spriteKey.isEmpty() && !done.contains(s->petId);
        QVERIFY(decoding <= 1);
        QVERIFY(markers().size() <= 1);
        const std::vector<FakeSurface *> now = surfaces;
        for (FakeSurface *s : now) {
            if (!started.contains(s->petId)) {
                started.insert(s->petId);
                s->m_callbacks.started();
                break;
            }
            if (!s->view.spriteKey.isEmpty() && !done.contains(s->petId)) {
                QCOMPARE(markers().size(), 1); // the decode is under the marker
                done.insert(s->petId);
                s->m_callbacks.spriteDone(true, 100, QString());
                break;
            }
        }
    }
    QCOMPARE(done.size(), 2);
    QTRY_COMPARE(markers().size(), 0);
}

void TstOverlayController::theBudgetCanShrinkAndStillImagesAreNotCached()
{
    QJsonObject still = animatedItem(u"anima:1"_s, original);
    QJsonObject variant = still[u"variants"_s].toArray().at(0).toObject();
    variant[u"frames"_s] = 1;
    still[u"variants"_s] = QJsonArray{variant};
    daemon.items[u"anima:1"_s] = still;
    auto controller = makeController();
    controller->setFrameCacheBudget(64 << 20);
    connectDaemon(*controller);
    QVERIFY(!surface(u"p1"_s)->view.spriteSize.isValid());
}

void TstOverlayController::mpvSpeedTurnsTheNativeRateIntoThePlacementsRate()
{
    // Miku: 60 fps delays, played at the engine's 30 fps: half speed (twice as long).
    daemon.pets = QJsonArray{placementFps(u"p1"_s, u"anima:1"_s, 30)};
    daemon.items[u"anima:1"_s] = animatedItemAt(u"anima:1"_s, original, 60);
    auto controller = makeController();
    connectDaemon(*controller);
    QCOMPARE(surface(u"p1"_s)->view.file, original);
    QCOMPARE(surface(u"p1"_s)->view.speed, 0.5);
    QCOMPARE(controller->describe().at(0).toObject().value(u"speed"_s).toDouble(), 0.5);
    QCOMPARE(controller->describe().at(0).toObject().value(u"fps"_s).toInt(), 30);

    // Another rate, and back to the file's own timing.
    daemon.pets = QJsonArray{placementFps(u"p1"_s, u"anima:1"_s, 15)};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_COMPARE(surface(u"p1"_s)->view.speed, 0.25);
    daemon.pets = QJsonArray{placementFps(u"p1"_s, u"anima:1"_s, 0)};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_COMPARE(surface(u"p1"_s)->view.speed, 1.0);
    // Faster than the file, and a still image (no native rate) is never scaled.
    daemon.pets = QJsonArray{placementFps(u"p1"_s, u"anima:1"_s, 60)};
    daemon.items[u"anima:1"_s] = animatedItemAt(u"anima:1"_s, original, 24);
    daemon.emitSignal(u"PetsChanged"_s);
    daemon.emitSignal(u"ItemsChanged"_s, {QStringList(), QStringList{u"anima:1"_s}, QStringList()});
    QTRY_COMPARE(surface(u"p1"_s)->view.speed, 2.5);
    daemon.items[u"anima:1"_s] = animatedItemAt(u"anima:1"_s, original, 0);
    daemon.emitSignal(u"ItemsChanged"_s, {QStringList(), QStringList{u"anima:1"_s}, QStringList()});
    QTRY_COMPARE(surface(u"p1"_s)->view.speed, 1.0);
}

void TstOverlayController::aRetimedCopyPlaysAtNormalSpeed()
{
    const QString copy = tmp.filePath(u"retimed.webm"_s);
    testutil::writeFile(copy, "z");
    daemon.pets = QJsonArray{placementFps(u"p1"_s, u"anima:1"_s, 30)};
    QJsonObject item = bucketItem(original, {});
    QJsonObject variant = item[u"variants"_s].toArray().at(0).toObject();
    variant[u"fps"_s] = 60;
    QJsonArray buckets = {QJsonObject{{u"bucket"_s, 384}, {u"fps"_s, 30}, {u"state"_s, u"done"_s}, {u"file"_s, copy}}};
    variant[u"buckets"_s] = buckets;
    item[u"variants"_s] = QJsonArray{variant};
    daemon.items[u"anima:1"_s] = item;
    auto controller = makeController();
    connectDaemon(*controller);
    QCOMPARE(surface(u"p1"_s)->view.file, copy);
    QCOMPARE(surface(u"p1"_s)->view.speed, 1.0); // every frame is already spaced 1/30 s apart in it
}

void TstOverlayController::aCopyRetimedToAnotherRateServesAFixedRateAtTheRatio()
{
    const QString at15 = tmp.filePath(u"at15.webm"_s);
    const QString at30 = tmp.filePath(u"at30.webm"_s);
    testutil::writeFile(at15, "z");
    testutil::writeFile(at30, "z");
    daemon.pets = QJsonArray{placementFps(u"p1"_s, u"anima:1"_s, 30), placementFps(u"p2"_s, u"anima:1"_s, 15)};
    QJsonObject item = bucketItem(original, {});
    QJsonObject variant = item[u"variants"_s].toArray().at(0).toObject();
    variant[u"fps"_s] = 60;
    variant[u"buckets"_s] = QJsonArray{
        QJsonObject{{u"bucket"_s, 384}, {u"fps"_s, 15}, {u"state"_s, u"done"_s}, {u"file"_s, at15}},
        QJsonObject{{u"bucket"_s, 384}, {u"fps"_s, 0}, {u"state"_s, u"done"_s}, {u"file"_s, at30}}};
    item[u"variants"_s] = QJsonArray{variant};
    daemon.items[u"anima:1"_s] = item;
    auto controller = makeController();
    connectDaemon(*controller);
    // The pet at 15 fps gets the copy retimed to 15 (exact); the pet at 30 prefers the copy at the file's own timing (the 60 fps source was halved
    // to 30 in it) over one retimed to another rate, and plays it at the source's pace: 30 over 60.
    QCOMPARE(surface(u"p2"_s)->view.file, at15);
    QCOMPARE(surface(u"p2"_s)->view.speed, 1.0);
    QCOMPARE(surface(u"p1"_s)->view.file, at30);
    QCOMPARE(surface(u"p1"_s)->view.speed, 0.5);
    // A pet at 24 over the same copies: only the copy at the file's timing fits, sped to 24 over 30
    daemon.pets = QJsonArray{placementFps(u"p3"_s, u"anima:1"_s, 24)};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_VERIFY(surface(u"p3"_s));
    QCOMPARE(surface(u"p3"_s)->view.file, at30);
    // the copy keeps every other frame of the 60 fps source, so the pace is the source's: 24 over 60, not over 30 (which would run twice as fast)
    QCOMPARE(surface(u"p3"_s)->view.speed, 0.4);
    QCOMPARE(surface(u"p3"_s)->view.timing, (PetTiming{0, 0.4})); // and the frame cache plays the copy's delays at that ratio
}

void TstOverlayController::aDaemonThatDoesNotRetimeStillGetsTheRightSpeed()
{
    // item() is the old shape: one copy, no rate. A pet at 30 fps over a 60 fps source plays the original sped
    // down; over the copy of a 60 fps source (which that daemon halved to 30) it needs the same ratio, because the copy
    // has every other frame: the pace is the source's.
    daemon.pets = QJsonArray{placementFps(u"p1"_s, u"anima:1"_s, 30)};
    QJsonObject old = item(original, u"done"_s, transcoded);
    QJsonObject variant = old[u"variants"_s].toArray().at(0).toObject();
    variant[u"fps"_s] = 60;
    old[u"variants"_s] = QJsonArray{variant};
    daemon.items[u"anima:1"_s] = old;
    auto controller = makeController();
    connectDaemon(*controller);
    QCOMPARE(surface(u"p1"_s)->view.file, transcoded);
    QCOMPARE(surface(u"p1"_s)->view.speed, 0.5);
}

void TstOverlayController::aCopyRetimedBeforeServesEveryFixedRate()
{
    // The user's shape: pets at 24 fps and a copy retimed to 30 (<bucket>-30-<key>), made by the versions that did that.
    const QString at30 = tmp.filePath(u"retimed30.webm"_s);
    testutil::writeFile(at30, "z");
    QJsonObject item = bucketItem(original, {});
    QJsonObject variant = item[u"variants"_s].toArray().at(0).toObject();
    variant[u"fps"_s] = 24;
    variant[u"buckets"_s] = QJsonArray{QJsonObject{{u"bucket"_s, 384}, {u"fps"_s, 30}, {u"state"_s, u"done"_s}, {u"file"_s, at30}}};
    item[u"variants"_s] = QJsonArray{variant};
    QJsonObject fast = placement(u"speedy"_s, u"anima:1"_s, u"DP-1"_s);
    fast[u"speed"_s] = 2.0;
    daemon.pets = QJsonArray{placementFps(u"p24"_s, u"anima:1"_s, 24), placementFps(u"p30"_s, u"anima:1"_s, 30),
                             placementFps(u"p60"_s, u"anima:1"_s, 60), placementFps(u"file"_s, u"anima:1"_s, 0), fast};
    daemon.items[u"anima:1"_s] = item;
    auto controller = makeController();
    connectDaemon(*controller);
    // uniform spacing of 1/30 s: libmpv's speed is the ratio of the rates, exact for every fixed rate
    QCOMPARE(surface(u"p24"_s)->view.file, at30);
    QCOMPARE(surface(u"p24"_s)->view.speed, 0.8);
    QCOMPARE(surface(u"p30"_s)->view.speed, 1.0);
    QCOMPARE(surface(u"p60"_s)->view.file, at30);
    QCOMPARE(surface(u"p60"_s)->view.speed, 2.0);
    // the frame cache plays the copy at a fixed step, which is the same pace
    QCOMPARE(surface(u"p24"_s)->view.timing, (PetTiming{24, 1}));
    // a pet at the file's timing or at a speed cannot use it: the file's delays are gone from it
    QCOMPARE(surface(u"file"_s)->view.file, original);
    QCOMPARE(surface(u"speedy"_s)->view.file, original);
}

void TstOverlayController::aChangeOfTimingIsLiveAndNeedsNoNewFrames()
{
    daemon.pets = QJsonArray{placementFps(u"p1"_s, u"anima:1"_s, 30)};
    daemon.items[u"anima:1"_s] = animatedItem(u"anima:1"_s, original);
    auto controller = makeController();
    controller->setFrameCacheBudget(64 << 20);
    connectDaemon(*controller);
    surface(u"p1"_s)->m_callbacks.started();
    QTRY_VERIFY(!surface(u"p1"_s)->view.spriteKey.isEmpty());
    QCOMPARE(surface(u"p1"_s)->view.timing, (PetTiming{30, 1}));
    const QString key = surface(u"p1"_s)->view.spriteKey;
    QVERIFY(!key.contains(u"fps"_s)); // the frames are the file's, whatever the pet's timing is
    surface(u"p1"_s)->m_callbacks.spriteDone(true, 1000, QString());

    // Another timing is applied to the frames that are on screen: the same key, nothing to decode again.
    daemon.pets = QJsonArray{placementFps(u"p1"_s, u"anima:1"_s, 15)};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_COMPARE(surface(u"p1"_s)->view.timing, (PetTiming{15, 1}));
    QCOMPARE(surface(u"p1"_s)->view.spriteKey, key);
    QJsonObject fast = placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s);
    fast[u"speed"_s] = 2.0;
    daemon.pets = QJsonArray{fast};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_COMPARE(surface(u"p1"_s)->view.timing, (PetTiming{0, 2}));
    QCOMPARE(surface(u"p1"_s)->view.spriteKey, key);
    QCOMPARE(controller->describe().at(0).toObject().value(u"timing"_s).toString(), u"speed"_s);
}

void TstOverlayController::aSpeedIsTheViewsAndLibmpvsSpeed()
{
    QJsonObject slow = placement(u"p1"_s, u"anima:1"_s, u"DP-1"_s);
    slow[u"speed"_s] = 0.5;
    daemon.pets = QJsonArray{slow, placementFps(u"p2"_s, u"anima:1"_s, 0)};
    daemon.items[u"anima:1"_s] = animatedItemAt(u"anima:1"_s, original, 60);
    auto controller = makeController();
    connectDaemon(*controller);
    QCOMPARE(surface(u"p1"_s)->view.speed, 0.5);       // libmpv plays the file's delays at half speed
    QCOMPARE(surface(u"p1"_s)->view.timing.speed, 0.5); // the frame cache does the same on its own
    QCOMPARE(surface(u"p2"_s)->view.speed, 1.0);
    const QJsonObject described = controller->describe().at(0).toObject();
    QCOMPARE(described.value(u"timing"_s).toString(), u"speed"_s);
    QCOMPARE(described.value(u"timingSpeed"_s).toDouble(), 0.5);
    QCOMPARE(controller->describe().at(1).toObject().value(u"timing"_s).toString(), u"file"_s);
    // (p2 has no "speed" key, like every placement of a daemon older than the field: the file's own speed)
}

void TstOverlayController::theVideoPathIsCappedOnlyWhereTheAnimationIsFaster()
{
    // a 10 fps animation and a 60 fps one, the battery cap of 15: only the fast one gets libmpv's fps filter (which would repeat frames of the other)
    daemon.pets = QJsonArray{placementFps(u"slow"_s, u"anima:1"_s, 0), placementFps(u"fast"_s, u"anima:2"_s, 0),
                             placementFps(u"fixed"_s, u"anima:2"_s, 24), placementFps(u"fixedslow"_s, u"anima:1"_s, 12)};
    daemon.items[u"anima:1"_s] = animatedItemAt(u"anima:1"_s, original, 10);
    daemon.items[u"anima:2"_s] = animatedItemAt(u"anima:2"_s, original, 60);
    auto controller = makeController();
    controller->setPowerState(true, false);
    connectDaemon(*controller);
    QCOMPARE(surface(u"slow"_s)->view.fpsCap, 15);
    QCOMPARE(surface(u"slow"_s)->view.videoFpsCap, 0);
    QCOMPARE(surface(u"fast"_s)->view.videoFpsCap, 15);
    QCOMPARE(surface(u"fixed"_s)->view.videoFpsCap, 15);   // plays at 24
    QCOMPARE(surface(u"fixedslow"_s)->view.videoFpsCap, 0); // plays at 12
}

void TstOverlayController::theFrameRateLimitFollowsTheSettingsAndThePower()
{
    daemon.pets = QJsonArray{placementFps(u"p1"_s, u"anima:1"_s, 0)};
    daemon.items[u"anima:1"_s] = animatedItem(u"anima:1"_s, original);
    auto controller = makeController();
    connectDaemon(*controller);
    QTRY_VERIFY(daemon.callCount(u"Settings"_s) >= 1);
    QCOMPARE(controller->currentFpsCap(), 0); // the defaults: no limit
    QCOMPARE(surface(u"p1"_s)->view.fpsCap, 0);

    // on battery, saving energy: the low cap
    controller->setPowerState(true, false);
    QCOMPARE(surface(u"p1"_s)->view.fpsCap, kPetEnergySaveFps);
    controller->setPowerState(false, true); // the power-saver profile does the same
    QCOMPARE(surface(u"p1"_s)->view.fpsCap, kPetEnergySaveFps);
    controller->setPowerState(false, false);
    QCOMPARE(surface(u"p1"_s)->view.fpsCap, 0);

    // the user's own limit, from the daemon's settings
    daemon.settings = QJsonObject{{u"petMaxFps"_s, 30}, {u"petSaveEnergy"_s, true}};
    daemon.emitSignal(u"SettingsChanged"_s);
    QTRY_COMPARE(surface(u"p1"_s)->view.fpsCap, 30);
    QCOMPARE(surface(u"p1"_s)->view.videoFpsCap, 0); // the item steps at 30 already: a cap of 30 would only resample it
    controller->setPowerState(true, false);
    QCOMPARE(surface(u"p1"_s)->view.fpsCap, kPetEnergySaveFps); // the lower one wins
    QCOMPARE(surface(u"p1"_s)->view.videoFpsCap, kPetEnergySaveFps);
    daemon.settings = QJsonObject{{u"petMaxFps"_s, 10}, {u"petSaveEnergy"_s, true}};
    daemon.emitSignal(u"SettingsChanged"_s);
    QTRY_COMPARE(surface(u"p1"_s)->view.fpsCap, 10); // and so does the user's when it is lower
    QCOMPARE(surface(u"p1"_s)->view.videoFpsCap, 10); // the item steps at 30: libmpv gets the cap, which only drops frames
    // saving energy off: the battery changes nothing
    daemon.settings = QJsonObject{{u"petMaxFps"_s, 30}, {u"petSaveEnergy"_s, false}};
    daemon.emitSignal(u"SettingsChanged"_s);
    QTRY_COMPARE(surface(u"p1"_s)->view.fpsCap, 30);
    QCOMPARE(controller->describe().at(0).toObject().value(u"fpsCap"_s).toInt(), 30);
    // out-of-range numbers from a newer daemon are clamped, a daemon that does not answer leaves the last values
    daemon.settings = QJsonObject{{u"petMaxFps"_s, 100000}, {u"petSaveEnergy"_s, false}};
    daemon.emitSignal(u"SettingsChanged"_s);
    QTRY_COMPARE(surface(u"p1"_s)->view.fpsCap, kMaxPetFps);
    daemon.settingsAnswer = false;
    daemon.emitSignal(u"SettingsChanged"_s);
    QTest::qWait(30);
    QCOMPARE(surface(u"p1"_s)->view.fpsCap, kMaxPetFps);
}

void TstOverlayController::losingTheAdmissionDuringADecodeFreesTheLedgerTurn()
{
    daemon.items[u"anima:1"_s] = animatedItem(u"anima:1"_s, original);
    const QString stateDir = tmp.filePath(u"lost"_s);
    const auto markers = [&] { return QDir(stateDir).entryList({u"opening-*.json"_s}, QDir::Files); };
    CrashLedger runLedger(stateDir);
    runLedger.load();
    ledger = &runLedger;
    const auto resetLedger = qScopeGuard([this] { ledger = nullptr; });
    auto controller = makeController();
    controller->setFrameCacheBudget(64 << 20);
    connectDaemon(*controller);
    surface(u"p1"_s)->m_callbacks.started();
    QTRY_VERIFY(!surface(u"p1"_s)->view.spriteKey.isEmpty());
    QCOMPARE(markers().size(), 1); // the decode is under the marker
    // The budget goes while it decodes: the window drops the decode without a word, and the turn ends here.
    controller->setFrameCacheBudget(0);
    QVERIFY(!surface(u"p1"_s)->view.spriteSize.isValid());
    QTRY_COMPARE(markers().size(), 0);
}

void TstOverlayController::aDecodeMayTakeAQuarterMoreThanItsEstimate()
{
    daemon.items[u"anima:1"_s] = animatedItem(u"anima:1"_s, original);
    auto controller = makeController();
    controller->setFrameCacheBudget(100'000'000); // plenty
    connectDaemon(*controller);
    // 10 frames of 200 x 150 x 4 = 1.2 MB promised: 1.5 MB at most, not the whole budget.
    QCOMPARE(surface(u"p1"_s)->view.spriteBudget, qint64(1'500'000));
    // And a tight budget still caps it.
    controller->setFrameCacheBudget(1'300'000);
    QCOMPARE(surface(u"p1"_s)->view.spriteBudget, qint64(1'300'000));
}

void TstOverlayController::aKnownCutSizeIsWhatAdmissionCounts()
{
    // Two pets of the same file at 200 wide: the whole frames take 1.2 MB each. In 1.6 MB the second does not fit... until the first has been decoded and reported what its cut frames took.
    daemon.pets = QJsonArray{placementWidth(u"a"_s, u"anima:1"_s, 200), placementWidth(u"b"_s, u"anima:1"_s, 200)};
    daemon.items[u"anima:1"_s] = animatedItem(u"anima:1"_s, original);
    auto controller = makeController();
    controller->setFrameCacheBudget(1'600'000);
    connectDaemon(*controller);
    QCOMPARE(surface(u"a"_s)->view.spriteSize, QSize(200, 150));
    QVERIFY(!surface(u"b"_s)->view.spriteSize.isValid()); // (nothing is known yet: the whole frames are counted)
    surface(u"a"_s)->m_callbacks.started();
    QTRY_VERIFY(!surface(u"a"_s)->view.spriteKey.isEmpty());
    surface(u"a"_s)->m_callbacks.spriteDone(true, 500'000, QString());
    QTRY_COMPARE(surface(u"b"_s)->view.spriteSize, QSize(200, 150)); // the same frames cut cost 0.5 MB: both fit
    // never more than the budget: what the two hold, and what the second may take on top of the first
    QVERIFY(surface(u"b"_s)->view.spriteBudget > 0);
    QVERIFY(surface(u"b"_s)->view.spriteBudget <= 1'600'000 - 500'000);
    QVERIFY(surface(u"a"_s)->view.spriteBudget <= 1'600'000 - 500'000 + 500'000);

    // a smaller budget than both cut sets refuses the second again
    controller->setFrameCacheBudget(800'000);
    QVERIFY(!surface(u"b"_s)->view.spriteSize.isValid());
    QVERIFY(controller->describe().at(0).toObject().value(u"cacheBytes"_s).toDouble() <= 800'000);
}

void TstOverlayController::aResizeHoldsTheOldAndNewFramesAgainstTheBudget()
{
    daemon.items[u"anima:1"_s] = animatedItem(u"anima:1"_s, original);
    auto controller = makeController();
    controller->setFrameCacheBudget(4'000'000);
    connectDaemon(*controller);
    surface(u"p1"_s)->m_callbacks.started();
    QTRY_VERIFY(!surface(u"p1"_s)->view.spriteKey.isEmpty());
    surface(u"p1"_s)->m_callbacks.spriteDone(true, 1'200'000, QString()); // 200 wide: 1.2 MB in memory

    // 300 wide needs 2.7 MB more while the 1.2 MB are still shown: 3.9 MB, which fits...
    daemon.pets = QJsonArray{placementWidth(u"p1"_s, u"anima:1"_s, 300)};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_COMPARE(surface(u"p1"_s)->view.spriteSize, QSize(300, 225));
    QCOMPARE(surface(u"p1"_s)->view.spriteBudget, qint64(4'000'000 - 1'200'000)); // what the old frames leave
    surface(u"p1"_s)->m_callbacks.spriteDone(true, 2'700'000, QString());

    // ...but a budget that holds either set alone does not hold both: the next resize gives up the frames.
    controller->setFrameCacheBudget(3'000'000);
    daemon.pets = QJsonArray{placementWidth(u"p1"_s, u"anima:1"_s, 320)};
    daemon.emitSignal(u"PetsChanged"_s);
    QTRY_VERIFY(!surface(u"p1"_s)->view.spriteSize.isValid());
    QCOMPARE(controller->describe().at(0).toObject().value(u"mode"_s).toString(), u"video"_s);
}

QTEST_MAIN(TstOverlayController)
#include "tst_overlaycontroller.moc"
