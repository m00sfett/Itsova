#include "traycontroller.h"

#include "trayicon.h"

#include <KStatusNotifierItem>

#include <QAction>
#include <QEvent>
#include <QApplication>
#include <QIcon>
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
{
    item_->setCategory(KStatusNotifierItem::ApplicationStatus);
    item_->setStatus(KStatusNotifierItem::Active);
    item_->setTitle(tr("glimply"));
    item_->setToolTipTitle(tr("glimply"));
    item_->setToolTipSubTitle(tr("Ready"));
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
    qApp->installEventFilter(this);
    updateIcon();
    rebuildMenu();
}

void TrayController::setLastCaptureAvailable(bool available)
{
    lastCaptureAvailable_ = available;
}

void TrayController::setStatusText(const QString &text)
{
    item_->setToolTipSubTitle(text);
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
    const QColor color = QApplication::palette().color(QPalette::WindowText);
    const QIcon icon = trayIcon(color, flashing_);
    item_->setIconByPixmap(icon);
    item_->setToolTipIconByPixmap(trayIcon(color));
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
    auto *addArea = add(new QAction(QIcon::fromTheme(QStringLiteral("list-add")), tr("Add Area…"), menu_));
    connect(addArea, &QAction::triggered, this, &TrayController::addAreaRequested);
    auto *openLast = add(new QAction(QIcon::fromTheme(QStringLiteral("document-open")), tr("Open Last Capture"), menu_));
    openLast->setEnabled(lastCaptureAvailable_);
    connect(openLast, &QAction::triggered, this, &TrayController::openLastCaptureRequested);
    auto *openFolder = add(new QAction(QIcon::fromTheme(QStringLiteral("folder-pictures")), tr("Open Capture Folder"), menu_));
    connect(openFolder, &QAction::triggered, this, &TrayController::openFolderRequested);
    addSeparator();
    auto *configure = add(new QAction(QIcon::fromTheme(QStringLiteral("configure")), tr("Configure glimply…"), menu_));
    connect(configure, &QAction::triggered, this, &TrayController::configureRequested);
    auto *quit = add(new QAction(QIcon::fromTheme(QStringLiteral("application-exit")), tr("Quit"), menu_));
    connect(quit, &QAction::triggered, qApp, &QCoreApplication::quit);
}

bool TrayController::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == qApp && event->type() == QEvent::ApplicationPaletteChange) {
        updateIcon();
    }
    return QObject::eventFilter(watched, event);
}

} // namespace glimply
