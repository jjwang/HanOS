/**-----------------------------------------------------------------------------

 @file    block.c
 @brief   Block-server client shared by the filesystem servers
 @details
 @verbatim

   Moves sectors through SVC_BLOCK over a memory object. The caller passes the
   absolute LBA and the address used to map the transfer buffer.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <string.h>

#include <protocol.h>
#include <sysfunc.h>

int32_t blk_client_read(int64_t block_ep, uint32_t lba, uint8_t count,
                        uint8_t * dst, uint64_t map_addr)
{
    int64_t memh = sys_mem_alloc((uint64_t) count * BLK_SECTOR);

    if (memh < 0)
        return -1;

    int64_t reply = sys_ep_create();

    if (reply < 0) {
        sys_handle_close(memh);
        return -2;
    }

    int64_t reply_send = sys_handle_dup(reply);
    int64_t memh_send = sys_handle_dup(memh);

    if (reply_send < 0 || memh_send < 0) {
        if (reply_send >= 0)
            sys_handle_close(reply_send);
        if (memh_send >= 0)
            sys_handle_close(memh_send);
        sys_handle_close(reply);
        sys_handle_close(memh);
        return -3;
    }

    sys_ipc_msg_t m;

    memset(&m, 0, sizeof(m));
    m.tag = BLOCK_READ;
    m.words[0] = lba;
    m.words[1] = count;
    m.xfer[0] = reply_send;
    m.xfer[1] = memh_send;
    m.xfer_count = 2;

    if (sys_ipc_send(block_ep, &m) != 0) {
        sys_handle_close(reply_send);
        sys_handle_close(memh_send);
        sys_handle_close(reply);
        sys_handle_close(memh);
        return -4;
    }

    sys_ipc_msg_t rep;
    int32_t rc = -5;

    if (sys_ipc_recv_timeout(reply, &rep, 2000) == 0) {
        if ((int64_t) rep.words[0] != BLOCK_OK) {
            rc = -6;
        } else if (sys_mem_map(memh, map_addr, PROT_READ) != 0) {
            rc = -7;
        } else {
            memcpy(dst, (void *) (uint64_t) map_addr,
                   (uint64_t) count * BLK_SECTOR);
            sys_mem_unmap(memh, map_addr);
            rc = 0;
        }
    }

    sys_handle_close(reply);
    sys_handle_close(memh);
    return rc;
}

int32_t blk_client_write(int64_t block_ep, uint32_t lba, uint8_t count,
                         const uint8_t * src, uint64_t map_addr)
{
    int64_t memh = sys_mem_alloc((uint64_t) count * BLK_SECTOR);

    if (memh < 0)
        return -1;

    if (sys_mem_map(memh, map_addr, PROT_READ | PROT_WRITE) != 0) {
        sys_handle_close(memh);
        return -2;
    }
    memcpy((void *) (uint64_t) map_addr, src,
           (uint64_t) count * BLK_SECTOR);
    sys_mem_unmap(memh, map_addr);

    int64_t reply = sys_ep_create();

    if (reply < 0) {
        sys_handle_close(memh);
        return -3;
    }

    int64_t reply_send = sys_handle_dup(reply);
    int64_t memh_send = sys_handle_dup(memh);

    if (reply_send < 0 || memh_send < 0) {
        if (reply_send >= 0)
            sys_handle_close(reply_send);
        if (memh_send >= 0)
            sys_handle_close(memh_send);
        sys_handle_close(reply);
        sys_handle_close(memh);
        return -3;
    }

    sys_ipc_msg_t m;

    memset(&m, 0, sizeof(m));
    m.tag = BLOCK_WRITE;
    m.words[0] = lba;
    m.words[1] = count;
    m.xfer[0] = reply_send;
    m.xfer[1] = memh_send;
    m.xfer_count = 2;

    if (sys_ipc_send(block_ep, &m) != 0) {
        sys_handle_close(reply_send);
        sys_handle_close(memh_send);
        sys_handle_close(reply);
        sys_handle_close(memh);
        return -4;
    }

    sys_ipc_msg_t rep;
    int32_t rc = -5;

    if (sys_ipc_recv_timeout(reply, &rep, 2000) == 0)
        rc = ((int64_t) rep.words[0] == BLOCK_OK) ? 0 : -6;

    sys_handle_close(reply);
    sys_handle_close(memh);
    return rc;
}
