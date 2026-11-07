#pragma once

#include <QQmlApplicationEngine>
#include <QObject>
#include <QVariant>

#include <filesystem>
#include <string>

// A1-min: real-QML scenarios of the project cost estimate. The REAL ProjectCostCard and OverviewProviderChart over the REAL
// ProjectCostController (and the real services over scripted sources) show, as VISIBLE text: a compaction-only project, a mixed one,
// one made only of records without price data (not "empty"), an unknown, a partial and a known-zero price, an empty project, an
// unreadable estimate after a success (no old sum, the original code), and recovery; the breakdown of the two sources, the coverage
// caption and the explanation without hover; and the geometry of all of it at 620/420/300 logical px with a long amount and a long
// error text. The same function runs in the scoped mode and in the full run; nothing is weakened between them.
void runProjectCostQmlTests(QQmlApplicationEngine& engine, QObject* hostWindow, const QVariant& colors);

// The REAL OverviewPage (the full smoke test's page, over its real BackendBridge and an empty disposable database): its cost card is the
// new one and shows the empty-project state of a readable estimate.
void checkProjectCostOverviewPage(QObject* overviewPage);

// The same page with POSITIVE data (call it when no modal dialog is open): a stored compaction record is added through a second
// connection; the page shows the breakdown, maps the provider/model rows, draws the chart row whole and, after a real tap on the row,
// opens the provider details.
void checkProjectCostOverviewPagePositive(QObject* overviewPage, const std::filesystem::path& databasePath, const std::string& projectId);
