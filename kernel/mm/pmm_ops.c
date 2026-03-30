/**-----------------------------------------------------------------------------

 @file    pmm_ops.c
 @brief   Physical memory allocator interface implementation
 @details
 @verbatim

  This file implements the pluggable allocator interface and provides
  a registry for different allocator backends.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <mm/pmm_ops.h>
#include <sys/panic.h>

static const allocator_ops_t *current_allocator = NULL;

void pmm_register_allocator(const allocator_ops_t *ops)
{
    if (ops == NULL) {
        kpanic("pmm_register_allocator: NULL allocator ops");
    }

    if (ops->init == NULL || ops->get == NULL ||
        ops->alloc == NULL || ops->free == NULL) {
        kpanic("pmm_register_allocator: incomplete allocator ops");
    }

    current_allocator = ops;
}

const allocator_ops_t *pmm_get_allocator(void)
{
    if (current_allocator == NULL) {
        kpanic("pmm_get_allocator: no allocator registered");
    }
    return current_allocator;
}
