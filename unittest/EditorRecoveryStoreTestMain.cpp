#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifdef min
#  undef min
#endif
#ifdef max
#  undef max
#endif

#include "AYTest.h"

#include "Test_EditorRecoveryAndTrash.cpp"

int main()
{
    return ayt::test::runSuite("AYEditor_RecoveryAndTrash");
}
