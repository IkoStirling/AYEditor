#pragma once

#include <cstdio>

#if defined(_DEBUG) && defined(_MSC_VER)
#  include <crtdbg.h>

// Set AY_EDITOR_HEAP_CHECK_ALWAYS=1 at compile time to break on every alloc/free (noisy).
// Link demo/AYEditorDebugHeapEarly.cpp so the debug heap is active before static init.
#  if defined(AY_EDITOR_HEAP_CHECK_ALWAYS)
#    define AY_EDITOR_HEAP_DEBUG_INIT()                                                   \
        _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF | _CRTDBG_CHECK_ALWAYS_DF)
#  else
#    define AY_EDITOR_HEAP_DEBUG_INIT()                                                   \
        _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF)
#  endif

#  define AY_EDITOR_HEAP_CHECK(label)                                                       \
      do {                                                                                  \
          if (!_CrtCheckMemory()) {                                                         \
              std::fprintf(stderr, "[HeapCheck] FAIL at %s\n", (label));                    \
              _CrtDbgBreak();                                                               \
          }                                                                                 \
      } while (0)

#  define AY_EDITOR_TRACE(label)                                                          \
      std::fprintf(stderr, "[EditorTrace] %s\n", (label))
#else
#  define AY_EDITOR_HEAP_DEBUG_INIT() ((void)0)
#  define AY_EDITOR_HEAP_CHECK(label) ((void)0)
#  define AY_EDITOR_TRACE(label) ((void)0)
#endif
