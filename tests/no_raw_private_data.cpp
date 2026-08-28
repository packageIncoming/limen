#include "limen/cm.hpp"

int main()
{
    limen::Event e;
    e.ack();

    e.get();    //  should not exist, pointer to rdma_cm_event is not accessible
    return 0;
}