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

int main(int argc, char* argv[])
{
    return ayt::test::runTests("AYEditor recovery", argc, argv);
}
