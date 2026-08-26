#include "limen/verbs.hpp"


int main()
{
    //  open a context on device "rocep1s0f0"
    limen::Context ctx;

    //  try to copy it
    limen::Context ctx2(ctx);
    ctx2 = ctx;
    return 0;
}