#include "traycontroller.h"

#include "targets.h"
#include "trayicon.h"

#include <KStatusNotifierItem>

#include <QAction>
#include <QEvent>
#include <QApplication>
#include <QIcon>
#include <QInputDialog>
#include <QMenu>
#include <QPalette>
#include <QTimer>

namespace glimply
{
namespace
{

// The menu fades out after a click. Wait until it is gone so it never appears in the capture.
constexpr int menuCloseDelayMs = 350;
constexpr int blinkDurationMs = 220;

QString sectionIcon(MenuTarget::Section section)
{
    switch (section) {
    case MenuTarget::Section::Group:
        return QStringLiteral("view-group");
    case MenuTarget::Section::Area:
        return QStringLiteral("transform-crop");
    case MenuTarget::Section::Monitor:
        return QStringLiteral("video-display");
    }
    return {};
}

} // namespace

TrayController::TrayController(TargetProvider provider, QObject *parent)
    : QObject(parent)
    , provider_(std::move(provider))
    , item_(new KStatusNotifierItem(QStringLiteral("glimply"), this))
    , menu_(new QMenu)
    , blinkTimer_(new QTimer(this))
    , statusText_(tr("Ready"))
{
    item_->setCategory(KStatusNotifierItem::ApplicationStatus);
    item_->setStatus(KStatusNotifierItem::Active);
    item_->setTitle(tr("glimply"));
    item_->setToolTipTitle(tr("glimply"));
    item_->setToolTipSubTitle(statusText_);
    item_->setStandardActionsEnabled(false); // the menu adds its own Quit
    item_->setContextMenu(menu_); // the item takes ownership
    item_->setIsMenu(true); // a click opens the menu

    blinkTimer_->setSingleShot(true);
    blinkTimer_->setInterval(blinkDurationMs);
    connect(blinkTimer_, &QTimer::timeout, this, [this] {
        flashing_ = false;
        updateIcon();
    });
    connect(menu_, &QMenu::aboutToShow, this, &TrayController::rebuildMenu);
    updateIcon();
    rebuildMenu();
}

void TrayController::setLastCaptureAvailable(bool available)
{
    lastCaptureAvailable_ = available;
}

void TrayController::setDelayState(int seconds, bool keepActive)
{
    delaySeconds_ = seconds;
    keepDelayActive_ = keepActive;
}

void TrayController::setCountdownState(bool active, int remainingSeconds)
{
    countdownActive_ = active;
    countdownRemaining_ = remainingSeconds;
    updateToolTip();
}

void TrayController::setActiveIntervals(const QList<ActiveIntervalStatus> &active)
{
    activeIntervals_ = active;
    updateIcon();
    updateToolTip();
}

void TrayController::setStatusText(const QString &text)
{
    statusText_ = text;
    updateToolTip();
}

void TrayController::setStandby(bool standby)
{
    item_->setStatus(standby ? KStatusNotifierItem::Passive : KStatusNotifierItem::Active);
}

void TrayController::blink()
{
    flashing_ = true;
    updateIcon();
    blinkTimer_->start();
}

void TrayController::updateIcon()
{
    const TrayIconState state = flashing_ ? TrayIconState::Flash
                              : !activeIntervals_.isEmpty() ? TrayIconState::IntervalActive
                                                            : TrayIconState::Normal;
    const TrayIconState staticState =
        !activeIntervals_.isEmpty() ? TrayIconState::IntervalActive : TrayIconState::Normal;
    item_->setIconByPixmap(trayIcon(state));
    item_->setToolTipIconByPixmap(trayIcon(staticState));
}

void TrayController::updateToolTip()
{
    if (countdownActive_) {
        item_->setToolTipSubTitle(tr("Capturing in %1 s…").arg(countdownRemaining_));
        return;
    }
    if (!activeIntervals_.isEmpty()) {
        QStringList names;
        names.reserve(activeIntervals_.size());
        for (const ActiveIntervalStatus &status : activeIntervals_) {
            names.append(tr("%1 (%2 s)").arg(status.displayName).arg(status.config.seconds));
        }
        item_->setToolTipSubTitle(tr("Intervals active: %1").arg(names.join(QStringLiteral(", "))));
        return;
    }
    item_->setToolTipSubTitle(statusText_);
}

void TrayController::rebuildMenu()
{
    for (QAction *action : menu_->actions()) {
        menu_->removeAction(action);
        action->deleteLater();
    }
    const auto add = [&](QAction *action) {
        menu_->addAction(action);
        return action;
    };
    const auto addSeparator = [&] {
        auto *separator = new QAction(menu_);
        separator->setSeparator(true);
        return add(separator);
    };
    // The exported tray menu (DBusMenu) has no section headers; separators and icons group the
    // entries instead.
    bool first = true;
    const auto addSection = [&] {
        if (!first) {
            addSeparator();
        }
        first = false;
    };

    if (countdownActive_) {
        auto *cancelCountdown = add(new QAction(QIcon::fromTheme(QStringLiteral("dialog-cancel")),
                                                tr("Cancel Countdown (%1 s)").arg(countdownRemaining_), menu_));
        connect(cancelCountdown, &QAction::triggered, this, &TrayController::cancelCountdownRequested);
        first = false;
    }

    if (!activeIntervals_.isEmpty()) {
        addSection();
        for (const ActiveIntervalStatus &status : activeIntervals_) {
            auto *stopAction = add(new QAction(QIcon::fromTheme(QStringLiteral("media-playback-stop")),
                                               tr("Stop Interval: %1 (%2 saved)")
                                                   .arg(status.displayName)
                                                   .arg(status.captureCount),
                                               menu_));
            connect(stopAction, &QAction::triggered, this,
                    [this, id = status.targetId] { Q_EMIT stopIntervalRequested(id); });
        }
        if (activeIntervals_.size() > 1) {
            auto *stopAll = add(new QAction(QIcon::fromTheme(QStringLiteral("media-playback-stop")),
                                            tr("Stop All Intervals"), menu_));
            connect(stopAll, &QAction::triggered, this, &TrayController::stopAllIntervalsRequested);
        }
    }

    const QList<MenuTarget> targets = provider_();
    bool hasArea = false;
    for (const MenuTarget::Section section :
         {MenuTarget::Section::Group, MenuTarget::Section::Area, MenuTarget::Section::Monitor}) {
        QList<MenuTarget> entries;
        for (const MenuTarget &target : targets) {
            if (target.section == section) {
                entries.append(target);
            }
        }
        if (section == MenuTarget::Section::Area) {
            hasArea = !entries.isEmpty();
        }
        if (entries.isEmpty() && section != MenuTarget::Section::Area) {
            continue;
        }
        addSection();
        for (const MenuTarget &target : std::as_const(entries)) {
            QString text = target.name;
            if (!target.detail.isEmpty()) {
                text = tr("%1 (%2)").arg(target.name, target.detail);
            }
            auto *action = add(new QAction(QIcon::fromTheme(sectionIcon(section)), text, menu_));
            // Exported as the DBusMenu shortcut property and shown in the shortcut column. The global
            // shortcut itself belongs to KGlobalAccel; this copy only acts while the menu has focus.
            action->setShortcut(target.shortcut);
            action->setShortcutContext(Qt::WidgetShortcut);
            action->setEnabled(target.available);
            connect(action, &QAction::triggered, this, [this, id = target.id] {
                QTimer::singleShot(menuCloseDelayMs, this, [this, id] { Q_EMIT triggerRequested(id); });
            });
        }
        if (section == MenuTarget::Section::Area && !hasArea) {
            auto *empty = add(new QAction(tr("No areas yet"), menu_));
            empty->setEnabled(false);
        }
    }

    addSeparator();
    // DBusMenu cannot edit text inline, so renaming a monitor opens a small dialog.
    auto *rename = new QMenu(tr("Rename Monitor"), menu_);
    rename->setIcon(QIcon::fromTheme(QStringLiteral("edit-rename")));
    for (const MenuTarget &target : targets) {
        if (target.section == MenuTarget::Section::Monitor && isScreenTargetId(target.id)) {
            auto *action = rename->addAction(QIcon::fromTheme(QStringLiteral("video-display")),
                                             tr("%1…").arg(target.name));
            connect(action, &QAction::triggered, this, [this, id = target.id] {
                QTimer::singleShot(menuCloseDelayMs, this, [this, id] { Q_EMIT renameMonitorRequested(id); });
            });
        }
    }
    rename->setEnabled(!rename->isEmpty());
    add(rename->menuAction());
    auto *addArea = add(new QAction(QIcon::fromTheme(QStringLiteral("list-add")), tr("Add Area…"), menu_));
    connect(addArea, &QAction::triggered, this, &TrayController::addAreaRequested);
    auto *pickColor = add(new QAction(QIcon::fromTheme(QStringLiteral("color-picker")), tr("Pick Color"), menu_));
    connect(pickColor, &QAction::triggered, this, [this] {
        QTimer::singleShot(menuCloseDelayMs, this, &TrayController::pickColorRequested);
    });

    const QString delayTitle = delaySeconds_ > 0 ? tr("Delay (%1 s)").arg(delaySeconds_) : tr("Delay");
    auto *delayMenu = new QMenu(delayTitle, menu_);
    delayMenu->setIcon(QIcon::fromTheme(QStringLiteral("chronometer")));
    const auto addDelayPreset = [&](const QString &label, int secs) {
        QAction *preset = delayMenu->addAction(label);
        preset->setCheckable(true);
        preset->setChecked(delaySeconds_ == secs);
        connect(preset, &QAction::triggered, this, [this, secs] {
            Q_EMIT delayChangedRequested(secs, secs > 0 ? keepDelayActive_ : false);
        });
    };
    addDelayPreset(tr("No delay"), 0);
    delayMenu->addSeparator();
    addDelayPreset(tr("3 seconds"), 3);
    addDelayPreset(tr("5 seconds"), 5);
    addDelayPreset(tr("10 seconds"), 10);
    QAction *customDelay = delayMenu->addAction(tr("Custom…"));
    connect(customDelay, &QAction::triggered, this, [this] {
        bool ok = false;
        const int chosen = QInputDialog::getInt(nullptr, tr("Capture Delay"), tr("Seconds:"),
                                              delaySeconds_ > 0 ? delaySeconds_ : 3, 1, 60, 1, &ok);
        if (ok) {
            Q_EMIT delayChangedRequested(chosen, keepDelayActive_);
        }
    });
    delayMenu->addSeparator();
    QAction *keepDelay = delayMenu->addAction(tr("Keep delay active"));
    keepDelay->setCheckable(true);
    keepDelay->setChecked(keepDelayActive_);
    connect(keepDelay, &QAction::toggled, this, [this](bool on) {
        if (on) {
            Q_EMIT delayChangedRequested(delaySeconds_ > 0 ? delaySeconds_ : 3, true);
        } else {
            Q_EMIT delayChangedRequested(0, false);
        }
    });
    add(delayMenu->menuAction());

    auto *intervalsMenu = new QMenu(tr("Intervals"), menu_);
    intervalsMenu->setIcon(QIcon::fromTheme(QStringLiteral("chronometer")));
    for (const MenuTarget &target : targets) {
        const QString text = target.intervalActive
                                 ? tr("Stop %1").arg(target.name)
                                 : tr("Start %1 (%2 s)").arg(target.name).arg(target.intervalSeconds);
        QAction *action = intervalsMenu->addAction(
            QIcon::fromTheme(QString::fromLatin1(target.intervalActive ? "media-playback-stop" : "media-playback-start")),
            text);
        action->setEnabled(target.intervalActive || target.available);
        connect(action, &QAction::triggered, this, [this, id = target.id, active = target.intervalActive] {
            if (active) {
                Q_EMIT stopIntervalRequested(id);
            } else {
                QTimer::singleShot(menuCloseDelayMs, this, [this, id] { Q_EMIT toggleIntervalRequested(id); });
            }
        });
    }
    if (!activeIntervals_.isEmpty()) {
        intervalsMenu->addSeparator();
        QAction *stopAll = intervalsMenu->addAction(QIcon::fromTheme(QStringLiteral("media-playback-stop")),
                                                    tr("Stop All Intervals"));
        connect(stopAll, &QAction::triggered, this, &TrayController::stopAllIntervalsRequested);
    }
    add(intervalsMenu->menuAction());

    auto *openLast = add(new QAction(QIcon::fromTheme(QStringLiteral("document-open")), tr("Open Last Capture"), menu_));
    openLast->setEnabled(lastCaptureAvailable_);
    connect(openLast, &QAction::triggered, this, &TrayController::openLastCaptureRequested);
    auto *editLast = add(new QAction(QIcon::fromTheme(QStringLiteral("document-edit")), tr("Edit Last Capture"), menu_));
    editLast->setEnabled(lastCaptureAvailable_);
    connect(editLast, &QAction::triggered, this, &TrayController::editLastCaptureRequested);
    auto *openFolder = add(new QAction(QIcon::fromTheme(QStringLiteral("folder-pictures")), tr("Open Capture Folder"), menu_));
    connect(openFolder, &QAction::triggered, this, &TrayController::openFolderRequested);
    addSeparator();
    auto *configure = add(new QAction(QIcon::fromTheme(QStringLiteral("configure")), tr("Configure glimply…"), menu_));
    connect(configure, &QAction::triggered, this, &TrayController::configureRequested);
    auto *quit = add(new QAction(QIcon::fromTheme(QStringLiteral("application-exit")), tr("Quit"), menu_));
    connect(quit, &QAction::triggered, qApp, &QCoreApplication::quit);
}

} // namespace glimply
