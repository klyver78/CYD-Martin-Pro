#pragma once
#include <cstddef>
struct UpdateClass { void onProgress(void (*)(size_t, size_t)) {} };
extern UpdateClass Update;
