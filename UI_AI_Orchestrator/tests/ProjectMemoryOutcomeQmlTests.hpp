#pragma once

#include <QQmlApplicationEngine>
#include <QObject>
#include <QVariant>

// C2-B/S2b-3: real-QML scenarios of the compaction result (ProjectMemoryOperationOutcome inside the real ProjectMemoryPanel):
// the geometry of the block at 620/420/300 logical px with long Unicode, long codes, an incomplete amount and eight bounded
// diagnostics (collapsed and expanded); the ownership rules of the panel (a result of another project is ignored, an accepted
// operation resets only its own result, a refusal does not, the project change clears everything before the reads, a component that
// disappears leaves no connection behind); and the decisive chain on the REAL controller: an operation failure with NotSaved /
// Unknown next to a failed segments read and a failed history read with IDENTICAL code and message, each shown apart and each
// recovering alone. The same function runs in the scoped mode and in the full run; nothing is weakened between them.
void runProjectMemoryOutcomeQmlTests(QQmlApplicationEngine& engine, QObject* hostWindow, const QVariant& colors);
