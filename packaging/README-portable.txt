AI Orchestrator @VERSION@  -  Windows x64 portable package
=========================================================

Copyright (C) 2026 justVladislav. Application licence: GNU GPL version 3 only.
See LICENSE, COPYRIGHT, THIRD-PARTY-NOTICES.txt and LICENCES for full terms.
Dependencies retain their own licences. Corresponding source archives must be
provided alongside this ZIP at the same download location. See SOURCE-INFO.txt.

Requirements
  * Windows 10 (version 1809 or later) or Windows 11, 64-bit.
  * Nothing else: Qt, Visual Studio or the Visual C++ Redistributable do not have to be installed (the runtime is in this folder).

Start
  1. Unzip the whole archive into a folder of your choice (keep its contents together).
  2. Start appUI_AI_Orchestrator.exe.
  3. Check the version: Settings > Diagnostics > Application version must read @VERSION@.

Where your data lives (never inside this folder)
  * Database : %APPDATA%\AIOrchestrator\AIOrchestrator\orchestrator.sqlite  (and the folders next to it)
  * API keys : in the Windows Credential Manager of your Windows user - not in this folder, not in the database file.
  Deleting this folder does not delete your data; delete the folder above yourself if you want it gone.

First steps
  Providers: add a provider, enter its API key (it is stored for your Windows user), choose models from the built-in catalog.
  Projects: create a project and point it at a working folder. Runs call the provider's API and spend real money.
  Project verification: use an MSVC x64 CMake preset. Visual Studio C++ tools, Windows SDK and the project's dependencies
  must be installed; the application supplies the child-process environment, not the compiler or Qt SDK.
  Review proposals and verification results before Apply. Nothing is applied automatically.

Updating to a newer version
  1. Close the application.
  2. Copy %APPDATA%\AIOrchestrator\AIOrchestrator (at least orchestrator.sqlite) somewhere safe.
  3. Unzip the new version into a NEW folder (do not unzip over the old one) and start it. A newer version upgrades the database
     on its first start; keep the copy until you are sure everything is in place.

Problems
  BUILD-INFO\BUILD-INFO.txt tells exactly which source commit, compiler, Qt and runtime this package was built from.
  When reporting a problem, include that file and the application version.
