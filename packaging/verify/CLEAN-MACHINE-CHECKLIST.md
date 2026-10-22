# Clean-machine check of the portable package

The release check needs a Windows that has **no Qt, no Visual Studio and no AI Orchestrator data** - Windows Sandbox, a fresh VM or
another PC. A separate Windows user on the build machine isolates the database but **not** the installed libraries, so it is not enough.

Everything below is done **inside** that clean environment. The package is `AIOrchestrator-<version>-win-x64.zip`; compare its SHA-256
with `AIOrchestrator-<version>-win-x64.zip.sha256` before you start.

## 0. Prepare (about 2 minutes)

1. Copy the ZIP, the `.sha256` file and (for the upgrade check, optional) a **copy** of an older database into the environment.
2. `Get-FileHash <zip> -Algorithm SHA256` must equal the value in the `.sha256` file.
3. Unzip the whole ZIP into a folder, for example `C:\AIO`.
4. Windows PowerShell, in the unzipped folder: `Set-ExecutionPolicy -Scope Process Bypass`.
   The script `packaging\verify\Test-PortablePackage.ps1` is **not** inside the ZIP; copy it from the repository next to the unzipped folder.

## 1. Automatic part (first start, database, second start)

```powershell
.\Test-PortablePackage.ps1 -PackageDir C:\AIO\AIOrchestrator-1.0.0-win-x64 -Interactive
```

It starts the real application twice and writes `package-check-report.txt`. It refuses to run if the profile already has a database.

## 2. Create a project through the interface (while the script waits at "press Enter")

| # | Where | What to do | Expected |
|---|---|---|---|
| 1 | Window title bar / taskbar | Look at the application | The window is titled **AI Orchestrator**, the icon is the application icon (not a blank one) |
| 2 | Left navigation > **Projects** > button **New project** | In the *New project* dialog fill **Name** = exactly **`RC check project`**, the project folder field with an existing folder (for example `C:/AIO-work`, create it first), and **Description** with any text; press **Create project**. If the dialog asks for anything else, write down what | The project appears in the list; no error message |
| 3 | Left navigation > **Settings** > Diagnostics | Read *Application version* and *Database status* | Version reads **1.0.0**; *Database status* does not say "unavailable" |
| 4 | Left navigation > **Settings** > **Data and privacy** > **Verification folder** | Look at the field and the line under it | The field is empty, its grey placeholder shows a folder under the user's Temp, the line says **In use: ...** and the **Use default** button is dimmed |
| 5 | Same row | Type `relative\folder` and press **Apply** | A red line says the folder must be an absolute path; **Use default** stays dimmed (nothing was stored) |
| 6 | Same row | Type `C:\AIO-vf` (a folder that does not exist yet) and press **Apply** (or Enter) | The line says **In use: C:\AIO-vf**; the folder now exists in Explorer; **Use default** is enabled |
| 7 | Same row | Type a path of more than 64 characters (for example `C:\` plus 70 letters) and press **Apply** | A red line says the path is too long (at most 64 characters) and what to do; the folder from step 6 stays in force |
| 8a | Left navigation > **Projects** > **New project** (**mandatory, Unicode**) | In Explorer create the folder `C:\AIO-work\папка-測試` (Cyrillic and Chinese letters; copy the name from here). Create a project named `RC unicode project` with that folder | The project appears in the list with its folder path shown unchanged; **the application stays open** (before the fix of `73661b3` a folder name the Windows ANSI code page cannot hold ended the application) |
| 8 | Left navigation > **Providers and models** | Open the page (do **not** enter a real key unless you want to) | The page opens; adding a provider of kind OpenAI/Anthropic offers the built-in model catalog |

Then press Enter in the PowerShell window: the script closes the application, checks that the project name is in the database file,
starts the application again and asks:

| # | Where | Expected |
|---|---|---|
| 9 | Projects page after the restart | **`RC check project`** is listed (answer `y` in PowerShell) |
| 9a | Projects page after the restart (**mandatory, Unicode**) | **`RC unicode project`** is listed with the same folder path |
| 10 | Settings > Data and privacy > Verification folder after the restart | The field still holds `C:\AIO-vf` and the line says **In use: C:\AIO-vf** (the setting is stored in the database) |

## 3. Upgrade of an existing database (optional, separate clean profile)

Run the script **instead of** step 1, on a clean profile, with the copy of the older database:

```powershell
.\Test-PortablePackage.ps1 -PackageDir C:\AIO\AIOrchestrator-1.0.0-win-x64 -ExistingDb C:\AIO\old-copy.sqlite -ExpectedSchema 70
```

Expected: `schema version 70` after the first normal close, and the old projects are visible in the window (open Projects after the run).

The hand-off folder contains `upgrade-fixture-v67.sqlite`: a SYNTHETIC schema-67 database made with the code of commit 0cd9ff4 (one project **Upgrade fixture project**, one completed Mock Run **Fixture run**, one provider and one model; no real data). Use it as the copy: after the run the Projects page lists **Upgrade fixture project** and the Runs page lists **Fixture run** as completed (its two artifact files are not part of the fixture, so opening one reports that the file is missing - that is expected). The same upgrade was already verified on the build machine (`upgrade-v67-to-v70-report.txt`).

## 3b. Verification setup of a project (needs Visual Studio with the C++ workload and a Qt MSVC 64-bit kit on that computer)

Start the application **from the Start menu or the portable folder - not** from a developer prompt (the point is that it must work without one).

| # | Where | What to do | Expected |
|---|---|---|---|
| 11 | Projects > select a project | Look at the **Verification setup** block | An unverified project says **Not checked yet**; an already checked project shows its saved status. Looking alone starts no setup |
| 12 | Projects > **New project** > *Verification setup* | With **existing presets**, press **Test configuration**. With **create local preset**, first create the project, then use **Check again** in its card (there is no Test configuration button in create mode) | A real configure runs (it can take a minute). **Ready / Configured; N targets confirmed**, or a red line with the reason (for example the Windows SDK is missing) - never "confirmed" without a configure |
| 13 | Create the project, select it, press **Check again** in **Verification setup** | Wait | **Ready — configured, N build targets confirmed**, with the preset pair shown; closing and reopening the application shows the same |
| 14 | Start a **Manifest** Run on that project (the Mock workflow does not use the setup) | Look at Run Details | The Run is not stopped for the setup and no second configure is started (the build folder's `CMakeCache.txt` keeps its modification time) |
| 15 | Add a line to the project's root `CMakeLists.txt`, open Projects, press **Check again** | | It configures again and is Ready again; **editing only a `.qml` file does not make it ask again** |
| 16 | Rename the preset's build folder or delete it (`build\local-qt-debug`), start a Manifest Run | | The Run first repairs the build folder (no provider cost) and then plans; if it cannot, the Run fails at once with the reason, **before any provider call** |

## 4. What to send back

`package-check-report.txt` (every line starts with PASS / FAIL / INFO), and for any FAIL a screenshot of the window or the text of the error.
Record in the report header which environment it was (Sandbox / VM / PC, Windows build) - the script prints the Windows build itself.
