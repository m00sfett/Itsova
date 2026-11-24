#pragma once

#include <QDBusContext>
#include <QObject>
#include <QString>

class QDBusServiceWatcher;

namespace glimply
{

class App;

// Session bus interface for the Plasma widget (and scripts): lists targets and runs the actions
// the tray offers. Registered at /Control on glimply's unique service name.
class ControlService : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.m00sfett.glimply.Control")

public:
    explicit ControlService(App *app);

    [[nodiscard]] bool widgetActive() const;

public Q_SLOTS:
    // JSON array of {targetId, kind (group|area|monitor|all), name, detail, shortcut, available}.
    Q_SCRIPTABLE QString Targets() const;
    Q_SCRIPTABLE bool HasLastCapture() const;
    Q_SCRIPTABLE void Trigger(const QString &id);
    Q_SCRIPTABLE void AddArea();
    Q_SCRIPTABLE void EditArea(const QString &id);
    // Returns an error message, or an empty string on success.
    Q_SCRIPTABLE QString Rename(const QString &id, const QString &name);
    Q_SCRIPTABLE void Remove(const QString &id);
    Q_SCRIPTABLE void OpenLastCapture();
    Q_SCRIPTABLE void EditLastCapture();
    Q_SCRIPTABLE void OpenCaptureFolder();
    Q_SCRIPTABLE void Configure(const QString &targetId);
    Q_SCRIPTABLE int DelaySeconds() const;
    Q_SCRIPTABLE bool KeepDelayActive() const;
    Q_SCRIPTABLE bool CountdownActive() const;
    Q_SCRIPTABLE void SetDelay(int seconds, bool keepActive);
    Q_SCRIPTABLE void CancelCountdown();
    // The widget calls this when it loads. While it is present, the separate tray icon steps back.
    Q_SCRIPTABLE void RegisterWidget();

Q_SIGNALS:
    Q_SCRIPTABLE void Changed();
    Q_SCRIPTABLE void Captured(bool success);
    void widgetActiveChanged(bool active);

private:
    App *app_;
    QDBusServiceWatcher *widgetWatcher_;
};

} // namespace glimply
