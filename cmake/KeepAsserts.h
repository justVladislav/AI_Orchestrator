// Force-included (/FI, -include) into every TEST executable by cmake/AIOrchestratorTestAsserts.cmake, before any other header.
// The tests check their results with assert(); a Release configuration defines NDEBUG, which would compile every one of those
// checks out and leave a green run that proves nothing. The PRODUCT code is not touched: only test translation units get this.
#ifdef NDEBUG
#undef NDEBUG
#endif
