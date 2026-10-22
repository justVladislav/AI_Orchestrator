// A test that fails when the test executables were built with their assert() calls compiled out (NDEBUG).
// The condition has a side effect on purpose: it is evaluated if and only if assert() is active in this executable, which is
// built exactly like every other test executable (cmake/AIOrchestratorTestAsserts.cmake).
#include <cassert>
#include <cstdio>

int main()
{
    int evaluations = 0;
    assert(++evaluations == 1);
    if (evaluations != 1) {
        std::fputs("assert() is compiled out of the test executables (NDEBUG): a green test run would prove nothing\n", stderr);
        return 1;
    }
    std::puts("assert() is active in the test executables");
    return 0;
}
