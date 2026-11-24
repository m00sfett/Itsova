#pragma once

#include <QDialog>
#include <QString>

class QLabel;
class QPushButton;
class QTimer;

namespace glimply
{

// Topmost native KDE/Breeze confirmation dialog shown when MCP access mode is ConfirmPerCapture.
// Defaults to Deny on Escape/close or after 30 s auto-deny timeout.
class McpConfirmDialog : public QDialog
{
    Q_OBJECT

public:
    explicit McpConfirmDialog(const QString &summaryText, int timeoutSeconds = 30, QWidget *parent = nullptr);

    [[nodiscard]] bool decidedAllowed() const;

Q_SIGNALS:
    void decided(bool allowed);

protected:
    void reject() override;

private:
    void finalize(bool allowed);
    void updateCountdownText();

    QLabel *countdownLabel_ = nullptr;
    QPushButton *denyButton_ = nullptr;
    QTimer *timer_ = nullptr;
    int remainingSeconds_ = 30;
    bool finished_ = false;
    bool allowed_ = false;
};

} // namespace glimply
