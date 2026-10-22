#include <exception>
#include <iostream>

int runBackendRuntimeTests();
int runAgentToolExecutionServiceTests();

int main()
{
    // Plain C++ entry point: BackendRuntime needs no Qt application object,
    // plugin search path, or event loop. Keep runtime-composition failures
    // isolated from provider/runtime/persistence test output.
    try {
        runBackendRuntimeTests();
        runAgentToolExecutionServiceTests();
        std::cout << "BackendRuntime tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        // Report the actual contract assertion to CTest instead of allowing a
        // Debug CRT unhandled-exception dialog to block headless verification.
        std::cerr << "BackendRuntime tests failed: " << exception.what() << '\n';
        return 1;
    }
}
