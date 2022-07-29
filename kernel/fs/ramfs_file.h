#pragma once

extern uint8_t helloworld_text_file_start, helloworld_text_file_end;

typedef struct {
    char name[VFS_MAX_NAME_LEN];
    void* data;
    uint64_t size;
} ramfs_file_t;

#define RAMFS_FILE_NUM          2

