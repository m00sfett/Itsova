#include "replaymanager.h"

#include "logging.h"
#include "recordingmetadata.h"
#include "screens.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusReply>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QMediaPlayer>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QTimer>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoSink>
#include <QWidget>
#include <QWindow>

namespace itsova
{

namespace
{

QScreen *findQScreen(const QString &connector)
{
    for (QScreen *s : QGuiApplication::screens()) {
        if (s && s->name() == connector) {
            return s;
        }
    }
    return QGuiApplication::primaryScreen();
}

} // namespace

class ReplayWindow : public QWidget
{
    Q_OBJECT

public:
    ReplayWindow(const ReplayWindowPlacement &placement, bool showCursor, bool showFirstHint)
        : placement_(placement)
        , showFirstHint_(showFirstHint)
    {
        setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        setWindowTitle(tr("Itsova Replay (%1)").arg(placement.connector));
        setAttribute(Qt::WA_OpaquePaintEvent);
        setAutoFillBackground(false);
        applyCursor(showCursor);

        QScreen *screen = findQScreen(placement.connector);
        if (screen) {
            setGeometry(screen->geometry());
        } else {
            setGeometry(placement.screenGeometry);
        }
        winId();
        if (windowHandle() && screen) {
            windowHandle()->setScreen(screen);
        }

        if (showFirstHint_) {
            QTimer::singleShot(3000, this, [this] {
                showFirstHint_ = false;
                update();
            });
        }
    }

    [[nodiscard]] QString connector() const { return placement_.connector; }

    void setFrame(const QImage &frame, const QRectF &resolvedSourceRect, const QRectF &resolvedTargetRect)
    {
        frame_ = frame;
        if (resolvedSourceRect.isValid() && !resolvedSourceRect.isEmpty()) {
            placement_.sourceRect = resolvedSourceRect;
        }
        if (resolvedTargetRect.isValid() && !resolvedTargetRect.isEmpty()) {
            placement_.targetRect = resolvedTargetRect;
        }
        update();
    }

    void applyCursor(bool show)
    {
        setCursor(show ? Qt::ArrowCursor : Qt::BlankCursor);
    }

Q_SIGNALS:
    void stopRequested();
    void toggleCursorRequested();

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), Qt::black);
        if (!frame_.isNull()) {
            const QRectF src = placement_.sourceRect.intersected(QRectF(frame_.rect()));
            if (!src.isEmpty()) {
                p.setRenderHint(QPainter::SmoothPixmapTransform,
                                src.size() != placement_.targetRect.size() * devicePixelRatioF());
                p.drawImage(placement_.targetRect, frame_, src);
            }
        }
        if (showFirstHint_) {
            const QString text = tr("Esc ends · M pointer");
            QFont f = font();
            f.setPointSizeF(std::max(10.0, f.pointSizeF()));
            p.setFont(f);
            const QFontMetrics fm(f);
            const QSize textSize = fm.size(Qt::TextSingleLine, text);
            const QRect pill(std::max(12, (width() - textSize.width() - 28) / 2), 20, textSize.width() + 28,
                             textSize.height() + 14);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 175));
            p.drawRoundedRect(pill, pill.height() / 2.0, pill.height() / 2.0);
            p.setPen(Qt::white);
            p.drawText(pill, Qt::AlignCenter, text);
        }
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape) {
            event->accept();
            Q_EMIT stopRequested();
            return;
        }
        if (event->key() == Qt::Key_M && event->modifiers() == Qt::NoModifier) {
            event->accept();
            Q_EMIT toggleCursorRequested();
            return;
        }
        QWidget::keyPressEvent(event);
    }

private:
    ReplayWindowPlacement placement_;
    QImage frame_;
    bool showFirstHint_ = false;
};

class ReplaySession : public QObject
{
    Q_OBJECT

public:
    ReplaySession(const QString &filePath,
                  const ReplayPlan &initialPlan,
                  const std::optional<RecordingMetadata> &metadata,
                  const QList<ScreenInfo> &screens,
                  const QHash<QString, QString> &monitorNames,
                  const QString &explicitScreen,
                  const QPoint &cursorPos,
                  bool loop,
                  bool showCursor,
                  bool showFirstHint,
                  QObject *parent)
        : QObject(parent)
        , plan_(initialPlan)
        , metadata_(metadata)
        , screens_(screens)
        , monitorNames_(monitorNames)
        , explicitScreen_(explicitScreen)
        , cursorPos_(cursorPos)
        , player_(new QMediaPlayer(this))
        , sink_(new QVideoSink(this))
    {
        bool hintPlaced = false;
        for (const ReplayWindowPlacement &wp : plan_.windows) {
            auto *win = new ReplayWindow(wp, showCursor, showFirstHint && !hintPlaced);
            hintPlaced = true;
            connect(win, &ReplayWindow::stopRequested, this, &ReplaySession::finished);
            connect(win, &ReplayWindow::toggleCursorRequested, this, &ReplaySession::toggleCursorRequested);
            win->showFullScreen();
            windows_.append(win);
        }

        // Muted by omitting QAudioOutput (#104).
        player_->setVideoSink(sink_);
        player_->setLoops(loop ? QMediaPlayer::Infinite : 1);
        connect(sink_, &QVideoSink::videoFrameChanged, this, &ReplaySession::onVideoFrame);
        connect(player_, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
            if (status == QMediaPlayer::EndOfMedia && player_->loops() == 1) {
                Q_EMIT finished();
            }
        });
        connect(player_, &QMediaPlayer::errorOccurred, this,
                [this](QMediaPlayer::Error, const QString &err) {
                    qCWarning(lcRecord) << "replay player error:" << err;
                    Q_EMIT finished();
                });

        player_->setSource(QUrl::fromLocalFile(filePath));
        player_->play();
    }

    ~ReplaySession() override
    {
        if (player_) {
            player_->stop();
        }
        for (ReplayWindow *win : std::as_const(windows_)) {
            if (win) {
                win->close();
                win->deleteLater();
            }
        }
        windows_.clear();
    }

    [[nodiscard]] QStringList connectors() const
    {
        QStringList list;
        for (const ReplayWindow *win : windows_) {
            if (win) {
                list.append(win->connector());
            }
        }
        return list;
    }

    [[nodiscard]] bool usesAnyConnector(const QSet<QString> &set) const
    {
        for (const ReplayWindow *win : windows_) {
            if (win && set.contains(win->connector())) {
                return true;
            }
        }
        return false;
    }

    void setShowCursor(bool show)
    {
        for (ReplayWindow *win : std::as_const(windows_)) {
            if (win) {
                win->applyCursor(show);
            }
        }
    }

    void setLoop(bool loop)
    {
        if (player_) {
            player_->setLoops(loop ? QMediaPlayer::Infinite : 1);
        }
    }

Q_SIGNALS:
    void finished();
    void toggleCursorRequested();

private:
    void onVideoFrame(const QVideoFrame &frame)
    {
        if (!frame.isValid()) {
            return;
        }
        const QImage img = frame.toImage();
        if (img.isNull()) {
            return;
        }
        if (img.size() != resolvedVideoSize_) {
            resolvedVideoSize_ = img.size();
            const ReplayPlan refined =
                planReplay(resolvedVideoSize_, metadata_, screens_, monitorNames_, explicitScreen_, cursorPos_);
            if (refined.windows.size() == windows_.size()) {
                plan_ = refined;
            }
        }
        for (int i = 0; i < windows_.size() && i < plan_.windows.size(); ++i) {
            if (ReplayWindow *win = windows_.at(i)) {
                win->setFrame(img, plan_.windows.at(i).sourceRect, plan_.windows.at(i).targetRect);
            }
        }
    }

    ReplayPlan plan_;
    std::optional<RecordingMetadata> metadata_;
    QList<ScreenInfo> screens_;
    QHash<QString, QString> monitorNames_;
    QString explicitScreen_;
    QPoint cursorPos_;
    QSize resolvedVideoSize_;
    QMediaPlayer *player_ = nullptr;
    QVideoSink *sink_ = nullptr;
    QList<ReplayWindow *> windows_;
};

ReplayManager::ReplayManager(TargetStore &store, QObject *parent)
    : QObject(parent)
    , store_(store)
{
}

ReplayManager::~ReplayManager()
{
    stopAll();
}

QString ReplayManager::startReplay(const QString &filePath,
                                   const QString &explicitScreen,
                                   const QPoint &cursorPos,
                                   QStringList *missingMonitorsOut)
{
    const QFileInfo info(filePath);
    if (!info.exists() || !info.isFile()) {
        return tr("Video file does not exist: %1").arg(filePath);
    }

    const QList<ScreenInfo> screens = currentScreens();
    const std::optional<RecordingMetadata> metadata = readRecordingMetadata(info.absoluteFilePath());
    const ReplayPlan plan =
        planReplay(QSize(1920, 1080), metadata, screens, store_.monitorNames, explicitScreen, cursorPos);
    if (!plan.error.isEmpty()) {
        return plan.error;
    }
    if (plan.windows.isEmpty()) {
        return tr("No target monitor available for replay.");
    }
    if (missingMonitorsOut) {
        *missingMonitorsOut = plan.missingMonitors;
    }

    // Replace any running replay occupying one of the target monitors (#104).
    QSet<QString> targetConnectors;
    for (const ReplayWindowPlacement &wp : plan.windows) {
        targetConnectors.insert(wp.connector);
    }
    const QList<ReplaySession *> existing = sessions_;
    for (ReplaySession *s : existing) {
        if (s->usesAnyConnector(targetConnectors)) {
            removeSession(s);
        }
    }

    const bool showFirstHint = !store_.replayFirstHintShown;
    if (showFirstHint) {
        store_.replayFirstHintShown = true;
        Q_EMIT settingsChanged();
    }

    const bool wasRunning = isRunning();
    auto *session = new ReplaySession(info.absoluteFilePath(), plan, metadata, screens, store_.monitorNames,
                                      explicitScreen, cursorPos, store_.replayLoop, store_.replayShowCursor,
                                      showFirstHint, this);
    connect(session, &ReplaySession::finished, this, [this, session] { removeSession(session); });
    connect(session, &ReplaySession::toggleCursorRequested, this, [this] {
        store_.replayShowCursor = !store_.replayShowCursor;
        setShowCursor(store_.replayShowCursor);
        Q_EMIT settingsChanged();
    });
    sessions_.append(session);
    qCInfo(lcRecord) << "replay started" << info.absoluteFilePath() << "on" << session->connectors();
    updateInhibition();
    if (!wasRunning && isRunning()) {
        Q_EMIT activeChanged(true);
    }
    return {};
}

void ReplayManager::stopAll()
{
    if (sessions_.isEmpty()) {
        return;
    }
    const QList<ReplaySession *> all = sessions_;
    sessions_.clear();
    for (ReplaySession *s : all) {
        s->disconnect(this);
        delete s;
    }
    updateInhibition();
    Q_EMIT activeChanged(false);
}

void ReplayManager::setShowCursor(bool show)
{
    for (ReplaySession *s : std::as_const(sessions_)) {
        s->setShowCursor(show);
    }
}

void ReplayManager::setLoop(bool loop)
{
    for (ReplaySession *s : std::as_const(sessions_)) {
        s->setLoop(loop);
    }
}

void ReplayManager::removeSession(ReplaySession *session)
{
    if (!sessions_.removeOne(session)) {
        return;
    }
    session->disconnect(this);
    session->deleteLater();
    updateInhibition();
    if (sessions_.isEmpty()) {
        Q_EMIT activeChanged(false);
    }
}

void ReplayManager::updateInhibition()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        return;
    }
    const bool want = !sessions_.isEmpty();
    if (want) {
        if (screenSaverCookie_ == 0) {
            QDBusMessage msg = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.ScreenSaver"),
                                                              QStringLiteral("/ScreenSaver"),
                                                              QStringLiteral("org.freedesktop.ScreenSaver"),
                                                              QStringLiteral("Inhibit"));
            msg << QStringLiteral("itsova") << QStringLiteral("Replaying video");
            const QDBusReply<quint32> reply = bus.call(msg, QDBus::Block, 1000);
            if (reply.isValid()) {
                screenSaverCookie_ = reply.value();
            }
        }
        if (powerInhibitCookie_ == 0) {
            QDBusMessage msg = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.PowerManagement.Inhibit"),
                                                              QStringLiteral("/org/freedesktop/PowerManagement/Inhibit"),
                                                              QStringLiteral("org.freedesktop.PowerManagement.Inhibit"),
                                                              QStringLiteral("Inhibit"));
            msg << QStringLiteral("itsova") << QStringLiteral("Replaying video");
            const QDBusReply<quint32> reply = bus.call(msg, QDBus::Block, 1000);
            if (reply.isValid()) {
                powerInhibitCookie_ = reply.value();
            }
        }
    } else {
        if (screenSaverCookie_ != 0) {
            QDBusMessage msg = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.ScreenSaver"),
                                                              QStringLiteral("/ScreenSaver"),
                                                              QStringLiteral("org.freedesktop.ScreenSaver"),
                                                              QStringLiteral("UnInhibit"));
            msg << screenSaverCookie_;
            bus.asyncCall(msg);
            screenSaverCookie_ = 0;
        }
        if (powerInhibitCookie_ != 0) {
            QDBusMessage msg = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.PowerManagement.Inhibit"),
                                                              QStringLiteral("/org/freedesktop/PowerManagement/Inhibit"),
                                                              QStringLiteral("org.freedesktop.PowerManagement.Inhibit"),
                                                              QStringLiteral("UnInhibit"));
            msg << powerInhibitCookie_;
            bus.asyncCall(msg);
            powerInhibitCookie_ = 0;
        }
    }
}

} // namespace itsova

#include "replaymanager.moc"
