#pragma once

#include <KPageDialog>

#include <QPointer>

class KKeySequenceWidget;
class KPageWidgetItem;
class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QTreeWidget;
class QWidget;

namespace glimply
{

class App;
class RegionSelector;

// The only window of glimply. Changes apply immediately; there is nothing to confirm.
class SettingsWindow : public KPageDialog
{
    Q_OBJECT

public:
    explicit SettingsWindow(App *app, QWidget *parent = nullptr);

    void addRegion();

private:
    QWidget *createTargetsPage();
    QWidget *createGroupsPage();
    QWidget *createStoragePage();
    QWidget *createGeneralPage();

    void refresh();
    void refreshTargets();
    void refreshTargetDetails();
    void refreshGroups();
    void refreshGroupDetails();
    void refreshStorage();

    [[nodiscard]] QString selectedTargetId() const;
    [[nodiscard]] QString selectedGroupId() const;
    void selectTarget(const QString &id);
    void selectGroup(const QString &id);

    void selectRegion(const QString &regionId);
    void applyRegionGeometry();
    void renameRegion();
    void renameGroup();
    void removeSelectedRegion();
    // Assigns a shortcut; asks before taking it away from another glimply target.
    void assignShortcut(const QString &id, const QKeySequence &sequence, KKeySequenceWidget *editor);
    void runSelector(const QString &connector, const QRect &initial, const QString &regionId);

    App *app_;
    QPointer<RegionSelector> selector_;
    bool updating_ = false;
    KPageWidgetItem *targetsPage_ = nullptr;

    // Targets page
    QTreeWidget *targetTree_ = nullptr;
    QPushButton *removeRegionButton_ = nullptr;
    QWidget *targetDetails_ = nullptr;
    QLabel *targetTitle_ = nullptr;
    QLineEdit *regionName_ = nullptr;
    QLabel *regionNameError_ = nullptr;
    QLabel *monitorLabel_ = nullptr;
    QWidget *geometryRow_ = nullptr;
    QSpinBox *regionX_ = nullptr;
    QSpinBox *regionY_ = nullptr;
    QSpinBox *regionWidth_ = nullptr;
    QSpinBox *regionHeight_ = nullptr;
    QLabel *regionWarning_ = nullptr;
    QCheckBox *targetCursor_ = nullptr;
    KKeySequenceWidget *targetShortcut_ = nullptr;
    QList<QWidget *> regionOnly_;

    // Groups page
    QListWidget *groupList_ = nullptr;
    QPushButton *removeGroupButton_ = nullptr;
    QWidget *groupDetails_ = nullptr;
    QLineEdit *groupName_ = nullptr;
    QLabel *groupNameError_ = nullptr;
    QListWidget *groupMembers_ = nullptr;
    QCheckBox *groupCursor_ = nullptr;
    KKeySequenceWidget *groupShortcut_ = nullptr;

    // Storage and general
    QLineEdit *directory_ = nullptr;
    QLineEdit *template_ = nullptr;
    QLabel *templatePreview_ = nullptr;
    QCheckBox *autostart_ = nullptr;
};

} // namespace glimply
