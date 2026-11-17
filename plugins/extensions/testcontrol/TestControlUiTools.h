/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TESTCONTROLUITOOLS_H
#define TESTCONTROLUITOOLS_H

#include <QObject>

#include "TestControlMcpServer.h"

/**
 * The MCP tools that operate widgets: list them with their state, click
 * buttons, and set combo boxes, spin boxes, check boxes and line edits. They
 * cover tool options, dockers and dialogs that have no action, such as the
 * Transform Tool's Puppet Warp options or Quick Access.
 *
 * ui_close closes a modeless window by its title. docker_show shows or hides
 * a docker by its ID, for example the Tool Options docker
 * (sharedtooldocker), which a fresh profile may not show.
 *
 * Widgets are searched in the visible top-level windows (main window,
 * floating dockers, popups, dialogs), optionally only in the window with a
 * given title or inside a named widget, and matched by object name, button
 * text, tool tip or class, since many feature widgets have no object name.
 * Buttons are clicked with
 * QAbstractButton::click() and values set through the widgets' setters, so the
 * widgets emit their usual signals; this does not test mouse handling inside
 * the widgets themselves.
 */
class TestControlUiTools : public QObject
{
    Q_OBJECT
public:
    explicit TestControlUiTools(QObject *parent = nullptr);
    ~TestControlUiTools() override;

    void registerTools(TestControlMcpServer *server);

private:
    using Arguments = QJsonObject;
    using Reply = TestControlMcpServer::ToolReply;

    void uiQuery(const Arguments &arguments, const Reply &reply);
    void uiClick(const Arguments &arguments, const Reply &reply);
    void uiSet(const Arguments &arguments, const Reply &reply);
    void uiClose(const Arguments &arguments, const Reply &reply);
    void dockerShow(const Arguments &arguments, const Reply &reply);
};

#endif // TESTCONTROLUITOOLS_H
