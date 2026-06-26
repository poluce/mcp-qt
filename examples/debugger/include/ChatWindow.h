#pragma once
#include <QMainWindow>
#include <QLineEdit>
#include <QPushButton>
#include <QComboBox>
#include <QPlainTextEdit>
#include <QLabel>
#include <QRadioButton>
#include <QTextEdit>
#include "AgentController.h"
#include "ToolManager.h"

/**
 * @brief Main GUI Window containing controls to connect to servers, list tools, and inspect communications.
 */
class ChatWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit ChatWindow(AgentController* controller, ToolManager* toolManager, QWidget* parent = nullptr);
    ~ChatWindow() override = default;

private slots:
    void handleConnect();
    void handleDisconnect();
    void handleShutdown();
    void handleCallTool();
    void handleListResources();
    void handleListPrompts();
    void updateToolList();
    void updateToolDescription();
    void appendLog(const QString& msg);
    void updateStatus(const QString& status);

private:
    void setupUi();

    AgentController* m_controller;
    ToolManager* m_toolManager;

    // UI Widgets
    QRadioButton* m_radioStdio;
    QRadioButton* m_radioHttp;
    
    QLineEdit* m_editStdioProgram;
    QLineEdit* m_editStdioArgs;
    QLineEdit* m_editHttpUrl;

    QPushButton* m_btnConnect;
    QPushButton* m_btnDisconnect;
    QPushButton* m_btnShutdown;

    QLabel* m_lblStatus;

    QComboBox* m_comboTools;
    QLabel* m_lblToolDesc;
    QTextEdit* m_editToolArgs;
    QPushButton* m_btnCallTool;
    QPushButton* m_btnListResources;
    QPushButton* m_btnListPrompts;

    QPlainTextEdit* m_txtLog;
};
