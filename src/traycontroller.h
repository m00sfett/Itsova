#pragma once

#include <QKeySequence>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>

class KStatusNotifierItem;
class QMenu;
class QTimer;

namespace glimply
{

struct MenuTarget
{
    enum class Section { Group, Area, Monitor };

    Section section = Section::Area;
    QString id;
    QString name;
    QString detail; // e.g. "not connected"
    QKeySequence shortcut;
    bool available = true;
};

class TrayController : public QObject
{
    Q_OBJECT

public:
    using TargetProvider = std::function<QList<MenuTarget>()>;

    explicit TrayController(TargetProvider provider, QObject *parent = nullptr);

    void setLastCaptureAvailable(bool available);
    void setStatusText(const QString &text);
    // Standby: Plasma keeps the icon in the hidden part of the tray (used while the widget is shown).
    void setStandby(bool standby);
    // One short blink. Call only after every frame of a trigger has been captured (A2).
    void blink();

Q_SIGNALS:
    void triggerRequested(const QString &id);
    void openLastCaptureRequested();
    void openFolderRequested();
    void configureRequested();
    void addAreaRequested();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void rebuildMenu();
    void updateIcon();

    TargetProvider provider_;
    KStatusNotifierItem *item_;
    QMenu *menu_;
    QTimer *blinkTimer_;
    bool flashing_ = false;
    bool lastCaptureAvailable_ = false;
};

} // namespace glimply
