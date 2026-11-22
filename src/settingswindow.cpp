#include "settingswindow.h"

#include "app.h"
#include "imageoutput.h"
#include "regionselector.h"
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
    targets->setHeader(tr("Regions and monitors"));
    targets->setIcon(QIcon::fromTheme(QStringLiteral("transform-crop")));
    auto *groups = addPage(createGroupsPage(), tr("Groups"));
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
    auto *addButton = new QPushButton(QIcon::fromTheme(QStringLiteral("list-add")), tr("Add Region…"), listSide);
    addButton->setToolTip(tr("Drag a rectangle on any monitor"));
    removeRegionButton_ = new QPushButton(QIcon::fromTheme(QStringLiteral("list-remove")), tr("Remove"), listSide);
    buttons->addWidget(addButton);
    buttons->addWidget(removeRegionButton_);
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

    regionName_ = new QLineEdit(targetDetails_);
    regionName_->setAccessibleName(tr("Region name"));
    regionNameError_ = makeError(targetDetails_);
    auto *nameBox = new QWidget(targetDetails_);
    auto *nameLayout = new QVBoxLayout(nameBox);
    nameLayout->setContentsMargins(0, 0, 0, 0);
    nameLayout->addWidget(regionName_);
    nameLayout->addWidget(regionNameError_);
    nameLayout->addWidget(makeHint(tr("Also used in the file name."), nameBox));
    form->addRow(tr("Name:"), nameBox);
    regionOnly_ = {nameBox, form->labelForField(nameBox)};

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
        connect(spin, &QSpinBox::valueChanged, this, &SettingsWindow::applyRegionGeometry);
        return spin;
    };
    regionX_ = makeSpin(tr("X: "), tr("Left edge"));
    regionY_ = makeSpin(tr("Y: "), tr("Top edge"));
    regionWidth_ = makeSpin(tr("Width: "), tr("Width"));
    regionHeight_ = makeSpin(tr("Height: "), tr("Height"));
    regionWidth_->setMinimum(1);
    regionHeight_->setMinimum(1);
    auto *positionRow = new QHBoxLayout;
    positionRow->addWidget(regionX_);
    positionRow->addWidget(regionY_);
    auto *sizeRow = new QHBoxLayout;
    sizeRow->addWidget(regionWidth_);
    sizeRow->addWidget(regionHeight_);
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
    form->addRow(tr("Area:"), geometryRow_);
    regionOnly_ << geometryRow_ << form->labelForField(geometryRow_);

    regionWarning_ = makeError(targetDetails_);
    form->addRow(QString(), regionWarning_);

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
    connect(addButton, &QPushButton::clicked, this, &SettingsWindow::addRegion);
    connect(removeRegionButton_, &QPushButton::clicked, this, &SettingsWindow::removeSelectedRegion);
    connect(regionName_, &QLineEdit::editingFinished, this, &SettingsWindow::renameRegion);
    connect(reselect, &QPushButton::clicked, this, [this] {
        if (const Region *region = app_->store().region(selectedTargetId())) {
            runSelector(region->connector, region->rect, region->id);
        }
    });
    connect(targetCursor_, &QCheckBox::toggled, this, [this](bool on) {
        if (updating_) {
            return;
        }
        TargetStore &store = app_->store();
        const QString id = selectedTargetId();
        if (Region *region = store.region(id)) {
            region->includeCursor = on;
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
    for (const Region &region : store.regions) {
        if (!connectors.contains(region.connector) && !disconnected.contains(region.connector)) {
            disconnected.append(region.connector);
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
        for (const Region &region : store.regions) {
            if (region.connector != connector) {
                continue;
            }
            auto *child = new QTreeWidgetItem(item);
            child->setText(0, region.name);
            child->setIcon(0, QIcon::fromTheme(QStringLiteral("transform-crop")));
            child->setText(1, shortcuts->shortcut(region.id).toString(QKeySequence::NativeText));
            child->setData(0, idRole, region.id);
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

    selectTarget(current.isEmpty() && !store.regions.isEmpty() ? store.regions.first().id : current);
    if (!targetTree_->currentItem() && targetTree_->topLevelItemCount() > 0) {
        targetTree_->setCurrentItem(targetTree_->topLevelItem(0));
    }
    refreshTargetDetails();
}

void SettingsWindow::refreshTargetDetails()
{
    const QString id = selectedTargetId();
    const TargetStore &store = app_->store();
    const Region *region = store.region(id);
    const bool isRegion = region != nullptr;
    removeRegionButton_->setEnabled(isRegion);
    targetDetails_->setEnabled(!id.isEmpty());

    updating_ = true;
    for (QWidget *widget : std::as_const(regionOnly_)) {
        if (widget) {
            widget->setVisible(isRegion);
        }
    }
    regionNameError_->hide();
    regionWarning_->hide();

    const QString connector = isRegion ? region->connector : connectorFromScreenTargetId(id);
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
    if (isRegion) {
        targetTitle_->setText(region->name);
        regionName_->setText(region->name);
        const QSize area = screen ? screen->geometry().size() : QSize(100000, 100000);
        regionX_->setMaximum(std::max(0, area.width() - 1));
        regionY_->setMaximum(std::max(0, area.height() - 1));
        regionWidth_->setMaximum(area.width());
        regionHeight_->setMaximum(area.height());
        regionX_->setValue(region->rect.x());
        regionY_->setValue(region->rect.y());
        regionWidth_->setValue(region->rect.width());
        regionHeight_->setValue(region->rect.height());
        geometryRow_->setEnabled(screen != nullptr);
        if (!screen) {
            regionWarning_->setText(tr("The monitor is not connected. This region is skipped when triggered."));
            regionWarning_->show();
        } else if (!QRect(QPoint(), screen->geometry().size()).contains(region->rect)) {
            regionWarning_->setText(tr("The region no longer fits on this monitor and is skipped when triggered. "
                                       "Adjust it or select it again."));
            regionWarning_->show();
        } else if (!region->monitorIdentity.isEmpty() && screenInfo(screen).identity() != region->monitorIdentity) {
            regionWarning_->setText(tr("A different monitor is connected to %1 than when this region was defined "
                                       "(%2).")
                                        .arg(connector, region->monitorIdentity));
            regionWarning_->show();
        }
    } else if (screen) {
        targetTitle_->setText(tr("Monitor %1").arg(connector));
    } else if (id != allMonitorsTargetId()) {
        targetTitle_->setText(tr("Monitor %1").arg(connector));
    }

    targetCursor_->setChecked(store.targetIncludesCursor(id));
    targetCursor_->setEnabled(isRegion || screen || id == allMonitorsTargetId());
    targetShortcut_->setKeySequence(app_->shortcuts()->shortcut(id));
    targetShortcut_->setEnabled(isRegion || screen || id == allMonitorsTargetId());
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

void SettingsWindow::addRegion()
{
    setCurrentPage(targetsPage_);
    runSelector(QString(), QRect(), QString());
}

void SettingsWindow::runSelector(const QString &connector, const QRect &initial, const QString &regionId)
{
    if (selector_) {
        return;
    }
    selector_ = new RegionSelector(this);
    connect(selector_, &RegionSelector::selected, this,
            [this, regionId](const QString &selectedConnector, const QRect &rect) {
                selector_->deleteLater();
                TargetStore &store = app_->store();
                QString id = regionId;
                if (Region *region = store.region(regionId)) {
                    region->rect = rect;
                    if (const QScreen *screen = findScreen(selectedConnector)) {
                        region->monitorIdentity = screenInfo(screen).identity();
                    }
                } else if (const QScreen *screen = findScreen(selectedConnector)) {
                    id = store.addRegion(screenInfo(screen), rect).id;
                }
                app_->storeChanged();
                show();
                raise();
                activateWindow();
                setCurrentPage(targetsPage_);
                selectTarget(id);
                if (regionId.isEmpty()) {
                    // A fresh region is most useful with a shortcut: start recording right away.
                    targetShortcut_->captureKeySequence();
                }
            });
    connect(selector_, &RegionSelector::cancelled, this, [this] {
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

void SettingsWindow::applyRegionGeometry()
{
    if (updating_) {
        return;
    }
    Region *region = app_->store().region(selectedTargetId());
    const QScreen *screen = region ? findScreen(region->connector) : nullptr;
    if (!region || !screen) {
        return;
    }
    const QSize area = screen->geometry().size();
    QRect rect(regionX_->value(), regionY_->value(), regionWidth_->value(), regionHeight_->value());
    // Keep the region on its monitor: shrink the size if the position pushes it over the edge.
    rect.setWidth(std::min(rect.width(), area.width() - rect.x()));
    rect.setHeight(std::min(rect.height(), area.height() - rect.y()));
    region->rect = rect;
    app_->storeChanged();
}

void SettingsWindow::renameRegion()
{
    if (updating_) {
        return;
    }
    Region *region = app_->store().region(selectedTargetId());
    if (!region) {
        return;
    }
    const QString name = regionName_->text().trimmed();
    if (name == region->name) {
        regionNameError_->hide();
        return;
    }
    QStringList connectors;
    for (const ScreenInfo &screen : currentScreens()) {
        connectors.append(screen.connector);
    }
    if (sanitizeFilenameToken(name).isEmpty()) {
        regionNameError_->setText(tr("Enter a name that can be used in a file name."));
        regionNameError_->show();
        return;
    }
    if (!app_->store().isNameAvailable(name, region->id, connectors)) {
        regionNameError_->setText(tr("“%1” is already used by another target or group.").arg(name));
        regionNameError_->show();
        return;
    }
    region->name = name;
    regionNameError_->hide();
    app_->storeChanged();
}

void SettingsWindow::removeSelectedRegion()
{
    const QString id = selectedTargetId();
    const Region *region = app_->store().region(id);
    if (!region) {
        return;
    }
    const auto answer = QMessageBox::question(
        this, tr("Remove Region"),
        tr("Remove region “%1”? Its shortcut is cleared and it is removed from all groups. Saved captures are "
           "kept.")
            .arg(region->name),
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
        for (const Region &region : app_->store().regions) {
            owners.append(region.id);
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
        for (const Region &region : store.regions) {
            addMember(region.id, tr("%1 (region on %2)").arg(region.name, region.connector),
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
    form->addRow(QString(), makeHint(tr("Placeholders: {date} (YYYY-MM-DD), {time} (HH-MM-SS), {region} (target "
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
