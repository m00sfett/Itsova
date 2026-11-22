#include "settingswindow.h"

#include "app.h"
#include "imageoutput.h"
#include "areaselector.h"
#include "screens.h"
#include "shortcutmanager.h"

#include <KKeySequenceWidget>
#include <KPageWidgetItem>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScreen>
#include <QSpinBox>
#include <QSplitter>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace glimply
{
namespace
{

constexpr int idRole = Qt::UserRole;
// The settings window must be gone before the frozen screen picture is taken.
constexpr int hideBeforeSelectDelayMs = 300;

QLabel *makeHint(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setEnabled(false); // disabled text colour is the KDE convention for secondary hints
    label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    return label;
}

QLabel *makeError(QWidget *parent)
{
    auto *label = new QLabel(parent);
    label->setWordWrap(true);
    QPalette pal = label->palette();
    pal.setColor(QPalette::WindowText, QColor(0xda, 0x44, 0x53)); // Breeze negative text
    label->setPalette(pal);
    label->hide();
    return label;
}

} // namespace

SettingsWindow::SettingsWindow(App *app, QWidget *parent)
    : KPageDialog(parent)
    , app_(app)
{
    setWindowTitle(tr("Configure glimply"));
    setWindowIcon(QIcon::fromTheme(QStringLiteral("io.github.m00sfett.glimply"),
                                   QIcon::fromTheme(QStringLiteral("camera-photo"))));
    setFaceType(KPageDialog::List);
    setStandardButtons(QDialogButtonBox::Close);
    setAttribute(Qt::WA_DeleteOnClose);

    targetsPage_ = addPage(createTargetsPage(), tr("Targets"));
    auto *targets = targetsPage_;
    targets->setHeader(tr("Areas and monitors"));
    targets->setIcon(QIcon::fromTheme(QStringLiteral("transform-crop")));
    groupsPage_ = addPage(createGroupsPage(), tr("Groups"));
    auto *groups = groupsPage_;
    groups->setHeader(tr("Capture several targets with one shortcut"));
    groups->setIcon(QIcon::fromTheme(QStringLiteral("view-group")));
    auto *storage = addPage(createStoragePage(), tr("Storage"));
    storage->setHeader(tr("Where captures are saved"));
    storage->setIcon(QIcon::fromTheme(QStringLiteral("folder-pictures")));
    auto *general = addPage(createGeneralPage(), tr("General"));
    general->setHeader(tr("General"));
    general->setIcon(QIcon::fromTheme(QStringLiteral("configure")));

    connect(app_, &App::targetsChanged, this, &SettingsWindow::refresh);
    connect(app_->shortcuts(), &ShortcutManager::shortcutChanged, this, [this] {
        if (!updating_) {
            refresh();
        }
    });
    resize(860, 600);
    refresh();
}

// ---------------------------------------------------------------- Targets

QWidget *SettingsWindow::createTargetsPage()
{
    auto *page = new QWidget;
    auto *layout = new QHBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *splitter = new QSplitter(page);
    layout->addWidget(splitter);

    auto *listSide = new QWidget(splitter);
    auto *listLayout = new QVBoxLayout(listSide);
    listLayout->setContentsMargins(0, 0, 0, 0);
    targetTree_ = new QTreeWidget(listSide);
    targetTree_->setAccessibleName(tr("Targets"));
    targetTree_->setHeaderLabels({tr("Target"), tr("Shortcut")});
    targetTree_->setRootIsDecorated(true);
    targetTree_->setUniformRowHeights(true);
    targetTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    targetTree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    targetTree_->header()->setStretchLastSection(false);
    listLayout->addWidget(targetTree_);
    auto *buttons = new QHBoxLayout;
    auto *addButton = new QPushButton(QIcon::fromTheme(QStringLiteral("list-add")), tr("Add Area…"), listSide);
    addButton->setToolTip(tr("Drag a rectangle on any monitor"));
    removeAreaButton_ = new QPushButton(QIcon::fromTheme(QStringLiteral("list-remove")), tr("Remove"), listSide);
    buttons->addWidget(addButton);
    buttons->addWidget(removeAreaButton_);
    buttons->addStretch();
    listLayout->addLayout(buttons);

    targetDetails_ = new QWidget(splitter);
    auto *form = new QFormLayout(targetDetails_);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setContentsMargins(12, 0, 0, 0);
    targetTitle_ = new QLabel(targetDetails_);
    QFont titleFont = targetTitle_->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.2);
    targetTitle_->setFont(titleFont);
    form->addRow(targetTitle_);

    areaName_ = new QLineEdit(targetDetails_);
    areaName_->setAccessibleName(tr("Area name"));
    areaNameError_ = makeError(targetDetails_);
    auto *nameBox = new QWidget(targetDetails_);
    auto *nameLayout = new QVBoxLayout(nameBox);
    nameLayout->setContentsMargins(0, 0, 0, 0);
    nameLayout->addWidget(areaName_);
    nameLayout->addWidget(areaNameError_);
    nameLayout->addWidget(makeHint(tr("Also used in the file name."), nameBox));
    form->addRow(tr("Name:"), nameBox);
    areaOnly_ = {nameBox, form->labelForField(nameBox)};

    monitorLabel_ = new QLabel(targetDetails_);
    monitorLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("Monitor:"), monitorLabel_);

    geometryRow_ = new QWidget(targetDetails_);
    auto *geometryLayout = new QVBoxLayout(geometryRow_);
    geometryLayout->setContentsMargins(0, 0, 0, 0);
    const auto makeSpin = [this](const QString &prefix, const QString &name) {
        auto *spin = new QSpinBox(geometryRow_);
        spin->setPrefix(prefix);
        spin->setAccessibleName(name);
        spin->setRange(0, 100000);
        spin->setAccelerated(true);
        spin->setKeyboardTracking(false);
        connect(spin, &QSpinBox::valueChanged, this, &SettingsWindow::applyAreaGeometry);
        return spin;
    };
    areaX_ = makeSpin(tr("X: "), tr("Left edge"));
    areaY_ = makeSpin(tr("Y: "), tr("Top edge"));
    areaWidth_ = makeSpin(tr("Width: "), tr("Width"));
    areaHeight_ = makeSpin(tr("Height: "), tr("Height"));
    areaWidth_->setMinimum(1);
    areaHeight_->setMinimum(1);
    auto *positionRow = new QHBoxLayout;
    positionRow->addWidget(areaX_);
    positionRow->addWidget(areaY_);
    auto *sizeRow = new QHBoxLayout;
    sizeRow->addWidget(areaWidth_);
    sizeRow->addWidget(areaHeight_);
    geometryLayout->addLayout(positionRow);
    geometryLayout->addLayout(sizeRow);
    auto *reselect = new QPushButton(QIcon::fromTheme(QStringLiteral("transform-crop")), tr("Select on Screen…"),
                                     geometryRow_);
    auto *reselectRow = new QHBoxLayout;
    reselectRow->addWidget(reselect);
    reselectRow->addStretch();
    geometryLayout->addLayout(reselectRow);
    geometryLayout->addWidget(makeHint(tr("Logical pixels relative to the monitor's top left corner. Captures use "
                                          "the monitor's native resolution."),
                                       geometryRow_));
    form->addRow(tr("Position and size:"), geometryRow_);
    areaOnly_ << geometryRow_ << form->labelForField(geometryRow_);

    areaWarning_ = makeError(targetDetails_);
    form->addRow(QString(), areaWarning_);

    targetCursor_ = new QCheckBox(tr("Include the mouse pointer"), targetDetails_);
    form->addRow(tr("Pointer:"), targetCursor_);

    targetShortcut_ = new KKeySequenceWidget(targetDetails_);
    targetShortcut_->setAccessibleName(tr("Shortcut"));
    targetShortcut_->setCheckForConflictsAgainst(KKeySequenceWidget::GlobalShortcuts
                                                 | KKeySequenceWidget::StandardShortcuts);
    targetShortcut_->setMultiKeyShortcutsAllowed(false);
    form->addRow(tr("Shortcut:"), targetShortcut_);
    form->addRow(QString(), makeHint(tr("Global: works in every application. Also editable in System Settings."),
                                     targetDetails_));

    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);

    connect(targetTree_, &QTreeWidget::currentItemChanged, this, &SettingsWindow::refreshTargetDetails);
    connect(addButton, &QPushButton::clicked, this, &SettingsWindow::addArea);
    connect(removeAreaButton_, &QPushButton::clicked, this, &SettingsWindow::removeSelectedArea);
    connect(areaName_, &QLineEdit::editingFinished, this, &SettingsWindow::renameArea);
    connect(reselect, &QPushButton::clicked, this, [this] {
        if (const Area *area = app_->store().area(selectedTargetId())) {
            runSelector(area->connector, area->rect, area->id);
        }
    });
    connect(targetCursor_, &QCheckBox::toggled, this, [this](bool on) {
        if (updating_) {
            return;
        }
        TargetStore &store = app_->store();
        const QString id = selectedTargetId();
        if (Area *area = store.area(id)) {
            area->includeCursor = on;
        } else if (id == allMonitorsTargetId()) {
            store.allMonitorsCursor = on;
        } else if (isScreenTargetId(id)) {
            const QString connector = connectorFromScreenTargetId(id);
            store.cursorScreens.removeAll(connector);
            if (on) {
                store.cursorScreens.append(connector);
            }
        }
        app_->storeChanged();
    });
    connect(targetShortcut_, &KKeySequenceWidget::keySequenceChanged, this, [this](const QKeySequence &sequence) {
        if (!updating_) {
            assignShortcut(selectedTargetId(), sequence, targetShortcut_);
        }
    });
    return page;
}

void SettingsWindow::refreshTargets()
{
    const QString current = selectedTargetId();
    const TargetStore &store = app_->store();
    const QList<ScreenInfo> screens = currentScreens();
    ShortcutManager *shortcuts = app_->shortcuts();

    QStringList connectors;
    for (const ScreenInfo &screen : screens) {
        connectors.append(screen.connector);
    }
    QStringList disconnected;
    for (const Area &area : store.areas) {
        if (!connectors.contains(area.connector) && !disconnected.contains(area.connector)) {
            disconnected.append(area.connector);
        }
    }

    targetTree_->clear();
    const auto addMonitor = [&](const QString &connector, bool connected) {
        const QString id = screenTargetId(connector);
        auto *item = new QTreeWidgetItem(targetTree_);
        item->setText(0, connected ? connector : tr("%1 (not connected)").arg(connector));
        item->setIcon(0, QIcon::fromTheme(QStringLiteral("video-display")));
        item->setText(1, shortcuts->shortcut(id).toString(QKeySequence::NativeText));
        item->setData(0, idRole, id);
        for (const Area &area : store.areas) {
            if (area.connector != connector) {
                continue;
            }
            auto *child = new QTreeWidgetItem(item);
            child->setText(0, area.name);
            child->setIcon(0, QIcon::fromTheme(QStringLiteral("transform-crop")));
            child->setText(1, shortcuts->shortcut(area.id).toString(QKeySequence::NativeText));
            child->setData(0, idRole, area.id);
        }
        item->setExpanded(true);
    };
    for (const QString &connector : std::as_const(connectors)) {
        addMonitor(connector, true);
    }
    for (const QString &connector : std::as_const(disconnected)) {
        addMonitor(connector, false);
    }
    auto *all = new QTreeWidgetItem(targetTree_);
    all->setText(0, tr("All monitors"));
    all->setIcon(0, QIcon::fromTheme(QStringLiteral("video-display")));
    all->setText(1, shortcuts->shortcut(allMonitorsTargetId()).toString(QKeySequence::NativeText));
    all->setData(0, idRole, allMonitorsTargetId());

    selectTarget(current.isEmpty() && !store.areas.isEmpty() ? store.areas.first().id : current);
    if (!targetTree_->currentItem() && targetTree_->topLevelItemCount() > 0) {
        targetTree_->setCurrentItem(targetTree_->topLevelItem(0));
    }
    refreshTargetDetails();
}

void SettingsWindow::refreshTargetDetails()
{
    const QString id = selectedTargetId();
    const TargetStore &store = app_->store();
    const Area *area = store.area(id);
    const bool isArea = area != nullptr;
    removeAreaButton_->setEnabled(isArea);
    targetDetails_->setEnabled(!id.isEmpty());

    updating_ = true;
    for (QWidget *widget : std::as_const(areaOnly_)) {
        if (widget) {
            widget->setVisible(isArea);
        }
    }
    areaNameError_->hide();
    areaWarning_->hide();

    const QString connector = isArea ? area->connector : connectorFromScreenTargetId(id);
    const QScreen *screen = connector.isEmpty() ? nullptr : findScreen(connector);
    if (id == allMonitorsTargetId()) {
        targetTitle_->setText(tr("All monitors"));
        monitorLabel_->setText(tr("One image of all monitors in their current arrangement, at the highest scale. "
                                  "Gaps are transparent."));
        monitorLabel_->setWordWrap(true);
    } else if (screen) {
        monitorLabel_->setWordWrap(false);
        // Qt only reports the integer buffer scale on Wayland, so the native size is not shown here.
        monitorLabel_->setText(tr("%1 · %2 × %3 (logical)")
                                   .arg(connector)
                                   .arg(screen->geometry().width())
                                   .arg(screen->geometry().height()));
    } else {
        monitorLabel_->setText(tr("%1 · not connected").arg(connector));
    }
    if (isArea) {
        targetTitle_->setText(area->name);
        areaName_->setText(area->name);
        const QSize bounds = screen ? screen->geometry().size() : QSize(100000, 100000);
        areaX_->setMaximum(std::max(0, bounds.width() - 1));
        areaY_->setMaximum(std::max(0, bounds.height() - 1));
        areaWidth_->setMaximum(bounds.width());
        areaHeight_->setMaximum(bounds.height());
        areaX_->setValue(area->rect.x());
        areaY_->setValue(area->rect.y());
        areaWidth_->setValue(area->rect.width());
        areaHeight_->setValue(area->rect.height());
        geometryRow_->setEnabled(screen != nullptr);
        if (!screen) {
            areaWarning_->setText(tr("The monitor is not connected. This area is skipped when triggered."));
            areaWarning_->show();
        } else if (!QRect(QPoint(), screen->geometry().size()).contains(area->rect)) {
            areaWarning_->setText(tr("The area no longer fits on this monitor and is skipped when triggered. "
                                       "Adjust it or select it again."));
            areaWarning_->show();
        } else if (!area->monitorIdentity.isEmpty() && screenInfo(screen).identity() != area->monitorIdentity) {
            areaWarning_->setText(tr("A different monitor is connected to %1 than when this area was defined "
                                       "(%2).")
                                        .arg(connector, area->monitorIdentity));
            areaWarning_->show();
        }
    } else if (screen) {
        targetTitle_->setText(tr("Monitor %1").arg(connector));
    } else if (id != allMonitorsTargetId()) {
        targetTitle_->setText(tr("Monitor %1").arg(connector));
    }

    targetCursor_->setChecked(store.targetIncludesCursor(id));
    targetCursor_->setEnabled(isArea || screen || id == allMonitorsTargetId());
    targetShortcut_->setKeySequence(app_->shortcuts()->shortcut(id));
    targetShortcut_->setEnabled(isArea || screen || id == allMonitorsTargetId());
    updating_ = false;
}

QString SettingsWindow::selectedTargetId() const
{
    const QTreeWidgetItem *item = targetTree_->currentItem();
    return item ? item->data(0, idRole).toString() : QString();
}

void SettingsWindow::selectTarget(const QString &id)
{
    for (QTreeWidgetItemIterator it(targetTree_); *it; ++it) {
        if ((*it)->data(0, idRole).toString() == id) {
            targetTree_->setCurrentItem(*it);
            return;
        }
    }
}

void SettingsWindow::addArea()
{
    setCurrentPage(targetsPage_);
    runSelector(QString(), QRect(), QString());
}

void SettingsWindow::showTarget(const QString &id)
{
    if (app_->store().group(id)) {
        setCurrentPage(groupsPage_);
        selectGroup(id);
    } else {
        setCurrentPage(targetsPage_);
        selectTarget(id);
    }
}

void SettingsWindow::editArea(const QString &areaId)
{
    if (const Area *area = app_->store().area(areaId)) {
        setCurrentPage(targetsPage_);
        selectTarget(areaId);
        runSelector(area->connector, area->rect, area->id);
    }
}

void SettingsWindow::runSelector(const QString &connector, const QRect &initial, const QString &areaId)
{
    if (selector_) {
        return;
    }
    selector_ = new AreaSelector(this);
    connect(selector_, &AreaSelector::selected, this,
            [this, areaId](const QString &selectedConnector, const QRect &rect) {
                selector_->deleteLater();
                TargetStore &store = app_->store();
                QString id = areaId;
                if (Area *area = store.area(areaId)) {
                    area->rect = rect;
                    if (const QScreen *screen = findScreen(selectedConnector)) {
                        area->monitorIdentity = screenInfo(screen).identity();
                    }
                } else if (const QScreen *screen = findScreen(selectedConnector)) {
                    id = store.addArea(screenInfo(screen), rect).id;
                }
                app_->storeChanged();
                show();
                raise();
                activateWindow();
                setCurrentPage(targetsPage_);
                selectTarget(id);
                if (areaId.isEmpty()) {
                    // A fresh area is most useful with a shortcut: start recording right away.
                    targetShortcut_->captureKeySequence();
                }
            });
    connect(selector_, &AreaSelector::cancelled, this, [this] {
        selector_->deleteLater();
        show();
        raise();
        activateWindow();
    });
    hide();
    QTimer::singleShot(hideBeforeSelectDelayMs, selector_, [selector = selector_.data(), connector, initial] {
        selector->start(connector, initial);
    });
}

void SettingsWindow::applyAreaGeometry()
{
    if (updating_) {
        return;
    }
    Area *area = app_->store().area(selectedTargetId());
    const QScreen *screen = area ? findScreen(area->connector) : nullptr;
    if (!area || !screen) {
        return;
    }
    const QSize bounds = screen->geometry().size();
    QRect rect(areaX_->value(), areaY_->value(), areaWidth_->value(), areaHeight_->value());
    // Keep the area on its monitor: shrink the size if the position pushes it over the edge.
    rect.setWidth(std::min(rect.width(), bounds.width() - rect.x()));
    rect.setHeight(std::min(rect.height(), bounds.height() - rect.y()));
    area->rect = rect;
    app_->storeChanged();
}

void SettingsWindow::renameArea()
{
    if (updating_) {
        return;
    }
    Area *area = app_->store().area(selectedTargetId());
    if (!area) {
        return;
    }
    const QString name = areaName_->text().trimmed();
    if (name == area->name) {
        areaNameError_->hide();
        return;
    }
    QStringList connectors;
    for (const ScreenInfo &screen : currentScreens()) {
        connectors.append(screen.connector);
    }
    if (sanitizeFilenameToken(name).isEmpty()) {
        areaNameError_->setText(tr("Enter a name that can be used in a file name."));
        areaNameError_->show();
        return;
    }
    if (!app_->store().isNameAvailable(name, area->id, connectors)) {
        areaNameError_->setText(tr("“%1” is already used by another target or group.").arg(name));
        areaNameError_->show();
        return;
    }
    area->name = name;
    areaNameError_->hide();
    app_->storeChanged();
}

void SettingsWindow::removeSelectedArea()
{
    const QString id = selectedTargetId();
    const Area *area = app_->store().area(id);
    if (!area) {
        return;
    }
    const auto answer = QMessageBox::question(
        this, tr("Remove Area"),
        tr("Remove area “%1”? Its shortcut is cleared and it is removed from all groups. Saved captures are "
           "kept.")
            .arg(area->name),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) {
        return;
    }
    app_->store().remove(id);
    app_->storeChanged({id});
}

void SettingsWindow::assignShortcut(const QString &id, const QKeySequence &sequence, KKeySequenceWidget *editor)
{
    if (id.isEmpty()) {
        return;
    }
    ShortcutManager *shortcuts = app_->shortcuts();
    if (!sequence.isEmpty()) {
        QStringList owners;
        for (const Group &group : app_->store().groups) {
            owners.append(group.id);
        }
        for (const Area &area : app_->store().areas) {
            owners.append(area.id);
        }
        for (const ScreenInfo &screen : currentScreens()) {
            owners.append(screenTargetId(screen.connector));
        }
        owners.append(allMonitorsTargetId());
        for (const QString &owner : std::as_const(owners)) {
            if (owner == id || shortcuts->shortcut(owner) != sequence) {
                continue;
            }
            const auto answer = QMessageBox::question(
                this, tr("Shortcut Already in Use"),
                tr("%1 is already assigned to “%2”. Move it to “%3”?")
                    .arg(sequence.toString(QKeySequence::NativeText), App::targetLabel(app_->store(), owner),
                         App::targetLabel(app_->store(), id)),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
            if (answer != QMessageBox::Yes) {
                updating_ = true;
                editor->setKeySequence(shortcuts->shortcut(id));
                updating_ = false;
                return;
            }
            updating_ = true;
            shortcuts->setShortcut(owner, {});
            updating_ = false;
        }
    }
    updating_ = true;
    shortcuts->setShortcut(id, sequence);
    updating_ = false;
    refresh();
}

// ---------------------------------------------------------------- Groups

QWidget *SettingsWindow::createGroupsPage()
{
    auto *page = new QWidget;
    auto *layout = new QHBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *splitter = new QSplitter(page);
    layout->addWidget(splitter);

    auto *listSide = new QWidget(splitter);
    auto *listLayout = new QVBoxLayout(listSide);
    listLayout->setContentsMargins(0, 0, 0, 0);
    groupList_ = new QListWidget(listSide);
    groupList_->setAccessibleName(tr("Groups"));
    listLayout->addWidget(groupList_);
    auto *buttons = new QHBoxLayout;
    auto *addButton = new QPushButton(QIcon::fromTheme(QStringLiteral("list-add")), tr("Add Group"), listSide);
    removeGroupButton_ = new QPushButton(QIcon::fromTheme(QStringLiteral("list-remove")), tr("Remove"), listSide);
    buttons->addWidget(addButton);
    buttons->addWidget(removeGroupButton_);
    buttons->addStretch();
    listLayout->addLayout(buttons);

    groupDetails_ = new QWidget(splitter);
    auto *form = new QFormLayout(groupDetails_);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setContentsMargins(12, 0, 0, 0);
    groupName_ = new QLineEdit(groupDetails_);
    groupName_->setAccessibleName(tr("Group name"));
    groupNameError_ = makeError(groupDetails_);
    auto *nameBox = new QWidget(groupDetails_);
    auto *nameLayout = new QVBoxLayout(nameBox);
    nameLayout->setContentsMargins(0, 0, 0, 0);
    nameLayout->addWidget(groupName_);
    nameLayout->addWidget(groupNameError_);
    form->addRow(tr("Name:"), nameBox);
    groupMembers_ = new QListWidget(groupDetails_);
    groupMembers_->setAccessibleName(tr("Targets in this group"));
    form->addRow(tr("Targets:"), groupMembers_);
    form->addRow(QString(), makeHint(tr("Each monitor is captured once per trigger, so all files show the same "
                                        "moment and share one timestamp."),
                                     groupDetails_));
    groupCursor_ = new QCheckBox(tr("Include the mouse pointer"), groupDetails_);
    form->addRow(tr("Pointer:"), groupCursor_);
    form->addRow(QString(), makeHint(tr("Applies to every target of this group, regardless of their own setting."),
                                     groupDetails_));
    groupShortcut_ = new KKeySequenceWidget(groupDetails_);
    groupShortcut_->setAccessibleName(tr("Group shortcut"));
    groupShortcut_->setCheckForConflictsAgainst(KKeySequenceWidget::GlobalShortcuts
                                                | KKeySequenceWidget::StandardShortcuts);
    groupShortcut_->setMultiKeyShortcutsAllowed(false);
    form->addRow(tr("Shortcut:"), groupShortcut_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);

    connect(groupList_, &QListWidget::currentItemChanged, this, &SettingsWindow::refreshGroupDetails);
    connect(addButton, &QPushButton::clicked, this, [this] {
        TargetStore &store = app_->store();
        QString name;
        for (int n = 1;; ++n) {
            name = tr("Group %1").arg(n);
            if (store.isNameAvailable(name)) {
                break;
            }
        }
        const QString id = store.addGroup(name).id;
        app_->storeChanged();
        selectGroup(id);
        groupName_->setFocus();
        groupName_->selectAll();
    });
    connect(removeGroupButton_, &QPushButton::clicked, this, [this] {
        const QString id = selectedGroupId();
        const Group *group = app_->store().group(id);
        if (!group) {
            return;
        }
        const auto answer = QMessageBox::question(
            this, tr("Remove Group"),
            tr("Remove group “%1”? Its targets stay; only the group and its shortcut are removed.").arg(group->name),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer == QMessageBox::Yes) {
            app_->store().remove(id);
            app_->storeChanged({id});
        }
    });
    connect(groupName_, &QLineEdit::editingFinished, this, &SettingsWindow::renameGroup);
    connect(groupMembers_, &QListWidget::itemChanged, this, [this] {
        if (updating_) {
            return;
        }
        Group *group = app_->store().group(selectedGroupId());
        if (!group) {
            return;
        }
        QStringList members;
        for (int row = 0; row < groupMembers_->count(); ++row) {
            const QListWidgetItem *item = groupMembers_->item(row);
            if (item->checkState() == Qt::Checked) {
                members.append(item->data(idRole).toString());
            }
        }
        group->targetIds = members;
        app_->storeChanged();
    });
    connect(groupCursor_, &QCheckBox::toggled, this, [this](bool on) {
        if (Group *group = app_->store().group(selectedGroupId()); group && !updating_) {
            group->includeCursor = on;
            app_->storeChanged();
        }
    });
    connect(groupShortcut_, &KKeySequenceWidget::keySequenceChanged, this, [this](const QKeySequence &sequence) {
        if (!updating_) {
            assignShortcut(selectedGroupId(), sequence, groupShortcut_);
        }
    });
    return page;
}

void SettingsWindow::refreshGroups()
{
    const QString current = selectedGroupId();
    updating_ = true;
    groupList_->clear();
    for (const Group &group : app_->store().groups) {
        auto *item = new QListWidgetItem(QIcon::fromTheme(QStringLiteral("view-group")), group.name, groupList_);
        item->setData(idRole, group.id);
        const QKeySequence shortcut = app_->shortcuts()->shortcut(group.id);
        if (!shortcut.isEmpty()) {
            item->setToolTip(shortcut.toString(QKeySequence::NativeText));
        }
    }
    updating_ = false;
    selectGroup(current);
    if (!groupList_->currentItem() && groupList_->count() > 0) {
        groupList_->setCurrentRow(0);
    }
    refreshGroupDetails();
}

void SettingsWindow::refreshGroupDetails()
{
    const Group *group = app_->store().group(selectedGroupId());
    removeGroupButton_->setEnabled(group);
    groupDetails_->setEnabled(group);
    updating_ = true;
    groupNameError_->hide();
    groupMembers_->clear();
    groupName_->setText(group ? group->name : QString());
    if (group) {
        const TargetStore &store = app_->store();
        const auto addMember = [&](const QString &id, const QString &label, const QString &icon) {
            auto *item = new QListWidgetItem(QIcon::fromTheme(icon), label, groupMembers_);
            item->setData(idRole, id);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(group->targetIds.contains(id) ? Qt::Checked : Qt::Unchecked);
        };
        for (const Area &area : store.areas) {
            addMember(area.id, tr("%1 (area on %2)").arg(area.name, area.connector),
                      QStringLiteral("transform-crop"));
        }
        QStringList connectors;
        for (const ScreenInfo &screen : currentScreens()) {
            connectors.append(screen.connector);
        }
        for (const QString &id : group->targetIds) {
            if (isScreenTargetId(id) && !connectors.contains(connectorFromScreenTargetId(id))) {
                connectors.append(connectorFromScreenTargetId(id));
            }
        }
        for (const QString &connector : std::as_const(connectors)) {
            addMember(screenTargetId(connector), tr("Monitor %1").arg(connector), QStringLiteral("video-display"));
        }
        addMember(allMonitorsTargetId(), tr("All monitors"), QStringLiteral("video-display"));
    }
    groupCursor_->setChecked(group && group->includeCursor);
    groupShortcut_->setKeySequence(group ? app_->shortcuts()->shortcut(group->id) : QKeySequence());
    updating_ = false;
}

QString SettingsWindow::selectedGroupId() const
{
    const QListWidgetItem *item = groupList_->currentItem();
    return item ? item->data(idRole).toString() : QString();
}

void SettingsWindow::selectGroup(const QString &id)
{
    for (int row = 0; row < groupList_->count(); ++row) {
        if (groupList_->item(row)->data(idRole).toString() == id) {
            groupList_->setCurrentRow(row);
            return;
        }
    }
}

void SettingsWindow::renameGroup()
{
    if (updating_) {
        return;
    }
    Group *group = app_->store().group(selectedGroupId());
    if (!group) {
        return;
    }
    const QString name = groupName_->text().trimmed();
    if (name == group->name) {
        groupNameError_->hide();
        return;
    }
    if (!app_->store().isNameAvailable(name, group->id)) {
        groupNameError_->setText(name.isEmpty() ? tr("Enter a name.")
                                                : tr("“%1” is already used by another target or group.").arg(name));
        groupNameError_->show();
        return;
    }
    group->name = name;
    groupNameError_->hide();
    app_->storeChanged();
}

// ---------------------------------------------------------------- Storage and general

QWidget *SettingsWindow::createStoragePage()
{
    auto *page = new QWidget;
    auto *outer = new QHBoxLayout(page);
    outer->setContentsMargins(0, 0, 0, 0);
    auto *content = new QWidget(page);
    content->setMaximumWidth(680);
    outer->addWidget(content);
    auto *form = new QFormLayout(content);
    form->setContentsMargins(0, 0, 0, 0);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    directory_ = new QLineEdit(page);
    directory_->setPlaceholderText(TargetStore::defaultCaptureDirectory());
    directory_->setMinimumWidth(360);
    directory_->setAccessibleName(tr("Capture folder"));
    auto *browse = new QPushButton(QIcon::fromTheme(QStringLiteral("document-open-folder")), tr("Browse…"), page);
    auto *directoryRow = new QHBoxLayout;
    directoryRow->addWidget(directory_);
    directoryRow->addWidget(browse);
    form->addRow(tr("Folder:"), directoryRow);
    form->addRow(QString(), makeHint(tr("All captures go into this folder, without subfolders. Leave empty for the "
                                        "default."),
                                     page));

    template_ = new QLineEdit(page);
    template_->setPlaceholderText(TargetStore::defaultFilenameTemplate());
    template_->setAccessibleName(tr("File name template"));
    form->addRow(tr("File name:"), template_);
    templatePreview_ = new QLabel(page);
    templatePreview_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("Example:"), templatePreview_);
    form->addRow(QString(), makeHint(tr("Placeholders: {date} (YYYY-MM-DD), {time} (HH-MM-SS), {area} (target "
                                        "name). Captures are saved as PNG. Existing files are never overwritten: "
                                        "a number is appended instead."),
                                     page));

    const auto applyDirectory = [this] {
        if (updating_) {
            return;
        }
        app_->store().captureDirectory = QDir::cleanPath(directory_->text().trimmed());
        if (directory_->text().trimmed().isEmpty()) {
            app_->store().captureDirectory.clear();
        }
        app_->storeChanged();
    };
    connect(directory_, &QLineEdit::editingFinished, this, applyDirectory);
    connect(browse, &QPushButton::clicked, this, [this, applyDirectory] {
        const QString chosen = QFileDialog::getExistingDirectory(this, tr("Capture Folder"),
                                                                 app_->store().effectiveCaptureDirectory());
        if (!chosen.isEmpty()) {
            directory_->setText(chosen);
            applyDirectory();
        }
    });
    connect(template_, &QLineEdit::textChanged, this, [this] {
        if (updating_) {
            return;
        }
        const QString text = template_->text().trimmed();
        templatePreview_->setText(filenameFor(text.isEmpty() ? TargetStore::defaultFilenameTemplate() : text,
                                              QStringLiteral("DP-1-1"), QDateTime::currentDateTime()));
    });
    connect(template_, &QLineEdit::editingFinished, this, [this] {
        if (!updating_) {
            app_->store().filenameTemplate = template_->text().trimmed();
            app_->storeChanged();
        }
    });
    return page;
}

QWidget *SettingsWindow::createGeneralPage()
{
    auto *page = new QWidget;
    auto *outer = new QHBoxLayout(page);
    outer->setContentsMargins(0, 0, 0, 0);
    auto *content = new QWidget(page);
    content->setMaximumWidth(680);
    outer->addWidget(content);
    auto *form = new QFormLayout(content);
    form->setContentsMargins(0, 0, 0, 0);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    autostart_ = new QCheckBox(tr("Start glimply when I log in"), page);
    form->addRow(tr("Startup:"), autostart_);
    form->addRow(QString(), makeHint(tr("Shortcuts only work while glimply is running."), page));
    connect(autostart_, &QCheckBox::toggled, this, [this](bool on) {
        if (!updating_) {
            app_->store().autostart = on;
            app_->storeChanged();
        }
    });
    return page;
}

void SettingsWindow::refreshStorage()
{
    updating_ = true;
    const TargetStore &store = app_->store();
    directory_->setText(store.captureDirectory);
    template_->setText(store.filenameTemplate);
    templatePreview_->setText(
        filenameFor(store.effectiveFilenameTemplate(), QStringLiteral("DP-1-1"), QDateTime::currentDateTime()));
    autostart_->setChecked(store.autostart);
    updating_ = false;
}

void SettingsWindow::refresh()
{
    refreshTargets();
    refreshGroups();
    refreshStorage();
}

} // namespace glimply
