#include "mcpconfirmdialog.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace glimply
{

McpConfirmDialog::McpConfirmDialog(const QString &summaryText, int timeoutSeconds, QWidget *parent)
    : QDialog(parent)
    , remainingSeconds_(std::max(1, timeoutSeconds))
{
    setWindowTitle(tr("Allow MCP Capture?"));
    setWindowIcon(QIcon::fromTheme(QStringLiteral("security-high")));
    setWindowFlags(windowFlags() | Qt::WindowStaysOnTopHint);
    setModal(false);

    auto *root = new QVBoxLayout(this);
    root->setSpacing(12);

    auto *topRow = new QHBoxLayout;
    topRow->setSpacing(12);
    auto *iconLabel = new QLabel(this);
    const QIcon icon = QIcon::fromTheme(QStringLiteral("security-high"),
                                        QIcon::fromTheme(QStringLiteral("dialog-question")));
    iconLabel->setPixmap(icon.pixmap(40, 40));
    iconLabel->setAlignment(Qt::AlignTop);
    topRow->addWidget(iconLabel);

    auto *textCol = new QVBoxLayout;
    textCol->setSpacing(6);
    auto *summary = new QLabel(summaryText, this);
    summary->setWordWrap(true);
    textCol->addWidget(summary);

    countdownLabel_ = new QLabel(this);
    countdownLabel_->setEnabled(false);
    textCol->addWidget(countdownLabel_);
    topRow->addLayout(textCol, 1);
    root->addLayout(topRow);

    auto *buttons = new QDialogButtonBox(this);
    denyButton_ = buttons->addButton(tr("Deny"), QDialogButtonBox::RejectRole);
    denyButton_->setIcon(QIcon::fromTheme(QStringLiteral("dialog-cancel")));
    denyButton_->setDefault(true);
    auto *allowButton = buttons->addButton(tr("Allow Once"), QDialogButtonBox::AcceptRole);
    allowButton->setIcon(QIcon::fromTheme(QStringLiteral("dialog-ok-apply")));
    root->addWidget(buttons);

    connect(denyButton_, &QPushButton::clicked, this, [this] { finalize(false); });
    connect(allowButton, &QPushButton::clicked, this, [this] { finalize(true); });

    timer_ = new QTimer(this);
    timer_->setInterval(1000);
    connect(timer_, &QTimer::timeout, this, [this] {
        --remainingSeconds_;
        if (remainingSeconds_ <= 0) {
            finalize(false);
            return;
        }
        updateCountdownText();
    });

    updateCountdownText();
    timer_->start();
    setMinimumWidth(420);
}

bool McpConfirmDialog::decidedAllowed() const
{
    return allowed_;
}

void McpConfirmDialog::reject()
{
    finalize(false);
    QDialog::reject();
}

void McpConfirmDialog::updateCountdownText()
{
    countdownLabel_->setText(tr("Auto-denying in %1 s…").arg(remainingSeconds_));
}

void McpConfirmDialog::finalize(bool allowed)
{
    if (finished_) {
        return;
    }
    finished_ = true;
    allowed_ = allowed;
    timer_->stop();
    hide();
    Q_EMIT decided(allowed);
    if (allowed) {
        accept();
    } else {
        done(QDialog::Rejected);
    }
}

} // namespace glimply
