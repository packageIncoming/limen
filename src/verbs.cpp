#include "limen/verbs.hpp"
#include <infiniband/verbs.h>

limen::DeviceList::DeviceList()
{
    this->device_list = ibv_get_device_list(NULL);
    if (this->device_list == nullptr)
    {
        throw VerbsError("ibv_get_device_list",errno);
    }
    
}
ibv_device** limen::DeviceList::get() const noexcept
{
    return this->device_list;
}



limen::Context::Context(const char* device_name)
{

}