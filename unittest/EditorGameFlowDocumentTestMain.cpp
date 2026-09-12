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

#include "Test_EditorGameFlowDocument.cpp"
#include "Test_EditorGameFlowPreview.cpp"
#include "Test_EditorGameFlowAssetIntegration.cpp"

int main()
{
    return ayt::test::runAllTests("AYEditor GameFlow document");
}
