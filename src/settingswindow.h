#pragma once

#include <KPageDialog>

#include <QPointer>

class KKeySequenceWidget;
class KPageWidgetItem;
class QCheckBox;
class QComboBox;
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
class AreaSelector;

// The only window of glimply. Changes apply immediately; there is nothing to confirm.
class SettingsWindow : public KPageDialog
{
    Q_OBJECT

public:
    explicit SettingsWindow(App *app, QWidget *parent = nullptr);

    void addArea();
    // Shows the page of an area, monitor or group and selects it.
    void showTarget(const QString &id);
    void editArea(const QString &areaId);

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

    void selectArea(const QString &areaId);
    void applyAreaGeometry();
    void renameArea();
    void renameGroup();
    void removeSelectedArea();
    // Assigns a shortcut; asks before taking it away from another glimply target.
    void assignShortcut(const QString &id, const QKeySequence &sequence, KKeySequenceWidget *editor);
    void runSelector(const QString &connector, const QRect &initial, const QString &areaId);

    App *app_;
    QPointer<AreaSelector> selector_;
    bool updating_ = false;
    KPageWidgetItem *targetsPage_ = nullptr;
    KPageWidgetItem *groupsPage_ = nullptr;

    // Targets page
    QTreeWidget *targetTree_ = nullptr;
    QPushButton *removeAreaButton_ = nullptr;
    QWidget *targetDetails_ = nullptr;
    QLabel *targetTitle_ = nullptr;
    QLineEdit *areaName_ = nullptr;
    QLabel *areaNameError_ = nullptr;
    QLabel *monitorLabel_ = nullptr;
    QWidget *geometryRow_ = nullptr;
    QSpinBox *areaX_ = nullptr;
    QSpinBox *areaY_ = nullptr;
    QSpinBox *areaWidth_ = nullptr;
    QSpinBox *areaHeight_ = nullptr;
    QLabel *areaWarning_ = nullptr;
    QCheckBox *targetCursor_ = nullptr;
    KKeySequenceWidget *targetShortcut_ = nullptr;
    QList<QWidget *> areaOnly_;
    QList<QWidget *> nameRow_; // areas and connected monitors
    QLabel *nameHint_ = nullptr;

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
    QCheckBox *usePrintKey_ = nullptr;
    QCheckBox *printOpensOverlay_ = nullptr;
    QComboBox *overlayStart_ = nullptr;
    QCheckBox *captureOnRelease_ = nullptr;
};

} // namespace glimply
