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
class ExportOptionsWidget;
class HookOverrideWidget;
class IntervalConfigWidget;

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
    // Version, licence and links in a line at the bottom.
    void addFooter();
    QWidget *createGroupsPage();
    QWidget *createStoragePage();
    QWidget *createGeneralPage();
    QWidget *createAutomationPage();

    void refresh();
    void refreshTargets();
    void refreshTargetDetails();
    void refreshGroups();
    void refreshGroupDetails();
    void refreshStorage();
    void refreshAutomation();

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
    void runSelector(const QRect &initialGlobalRect, const QString &areaId);

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
    QLabel *geometryHint_ = nullptr;
    QLabel *areaWarning_ = nullptr;
    QCheckBox *targetCursor_ = nullptr;
    ExportOptionsWidget *targetExport_ = nullptr;
    QSpinBox *targetDelay_ = nullptr;
    IntervalConfigWidget *targetInterval_ = nullptr;
    HookOverrideWidget *targetHook_ = nullptr;
    KKeySequenceWidget *targetShortcut_ = nullptr;
    KKeySequenceWidget *targetIntervalShortcut_ = nullptr;
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
    ExportOptionsWidget *groupExport_ = nullptr;
    QSpinBox *groupDelay_ = nullptr;
    IntervalConfigWidget *groupInterval_ = nullptr;
    HookOverrideWidget *groupHook_ = nullptr;
    KKeySequenceWidget *groupShortcut_ = nullptr;
    KKeySequenceWidget *groupIntervalShortcut_ = nullptr;

    // Storage and general
    QLineEdit *directory_ = nullptr;
    QLineEdit *template_ = nullptr;
    QLabel *templatePreview_ = nullptr;
    QSpinBox *historyLimit_ = nullptr;
    QLabel *historyFeedback_ = nullptr;
    ExportOptionsWidget *storageExport_ = nullptr;
    QCheckBox *autostart_ = nullptr;
    QCheckBox *usePrintKey_ = nullptr;
    QCheckBox *printOpensOverlay_ = nullptr;
    QComboBox *overlayStart_ = nullptr;
    QCheckBox *captureOnRelease_ = nullptr;
    QCheckBox *showSelectionLoupe_ = nullptr;
    KKeySequenceWidget *colorPickerShortcut_ = nullptr;

    // Automation page (#24 hooks, #25 MCP)
    QCheckBox *hookEnabled_ = nullptr;
    QWidget *hookDetails_ = nullptr;
    QLineEdit *hookCommand_ = nullptr;
    QSpinBox *hookTimeout_ = nullptr;
    QCheckBox *hookOnInterval_ = nullptr;
    QPushButton *testHookButton_ = nullptr;
    QLabel *hookTestStatus_ = nullptr;

    QCheckBox *mcpEnabled_ = nullptr;
    QWidget *mcpDetails_ = nullptr;
    QComboBox *mcpAccessMode_ = nullptr;
    QWidget *mcpAllowlistBox_ = nullptr;
    QListWidget *mcpAllowlistList_ = nullptr;
    QLabel *mcpAllowlistEmpty_ = nullptr;
    QLabel *mcpFullWarning_ = nullptr;
    QCheckBox *mcpNotify_ = nullptr;
    QLineEdit *mcpCommandDisplay_ = nullptr;
    QPushButton *mcpCopyButton_ = nullptr;
};

} // namespace glimply
