#pragma once

#include <QQmlApplicationEngine>
#include <QObject>
#include <QVariant>

#include <filesystem>
#include <string>

// B3-III: real-QML scenarios of the compaction recording coverage.
//
// runCompactionCoverageQmlTests (scoped and full run alike): the REAL ProjectCostCard / ProjectMemoryPanel / ProjectMemoryStartupRecovery
// over the REAL controllers and Application services, with SCRIPTED intent ports (so a read failure is a real failure of the read, not a
// QML assignment): the coverage next to an unchanged amount, a real zero and an unavailable read, a project with only unfinished
// protocols, the separate unfinished-protocol section and its own error slot (a coverage error, a records error, a segments error and
// the operation's own failure with the SAME code and message never overwrite each other), the database-scope startup report (Partial /
// Complete / absent, Details by a real tap), and the geometry of all of it at 620/420/300 logical px with a long error, 8 causes and 8
// write causes. A second experiment, labelled apart, uses a REAL BackendBridge over a disposable SQLite database that was seeded BEFORE
// its construction, so the owner's startup really marked the intent Unresolved.
void runCompactionCoverageQmlTests(QQmlApplicationEngine& engine, QObject* hostWindow, const QVariant& colors);

// The REAL OverviewPage of the full smoke test (its real BackendBridge and disposable database): an Open intent is seeded through a
// second connection AFTER the live startup, and the page shows the coverage next to the unchanged amount; a real ProjectMemoryPanel over
// the very same bridge lists the protocol. Call it when no modal dialog is open.
void checkCompactionCoverageOverviewPage(QQmlApplicationEngine& engine, QObject* hostWindow, const QVariant& colors, QObject* overviewPage,
    const std::filesystem::path& databasePath, const std::string& projectId);
