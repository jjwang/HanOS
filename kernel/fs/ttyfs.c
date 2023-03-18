#include <fs/ttyfs.h>
#include <fs/filebase.h>
#include <lib/kmalloc.h>
#include <lib/klog.h>
#include <lib/klib.h>
#include <lib/string.h>
#include <lib/memutils.h>
#include <lib/errno.h>
#include <sys/panic.h>
#include <sys/hpet.h>
#include <sys/cmos.h>
#include <sys/mm.h>
#include <proc/eventbus.h>
#include <device/display/term.h>

/* This is a linux extension */
#define TCGETS          0x5401
#define TCSETS          0x5402
#define TIOCGPGRP       0x540F
#define TIOCSPGRP       0x5410
#define TIOCGWINSZ      0x5413
#define TIOCSWINSZ      0x5414
#define TIOCGSID        0x5429

/* Bitwise constants for c_lflag in struct termios_t */
#define ECHO            0x0001
#define ECHOE           0x0002
#define ECHOK           0x0004
#define ECHONL          0x0008
#define ICANON          0x0010
#define IEXTEN          0x0020
#define ISIG            0x0040
#define NOFLSH          0x0080
#define TOSTOP          0x0100

/* Indices for the c_cc array in struct termios_t */
#define NCCS            11
#define VEOF            0
#define VEOL            1
#define VERASE          2
#define VINTR           3
#define VKILL           4
#define VMIN            5
#define VQUIT           6
#define VSTART          7
#define VSTOP           8
#define VSUSP           9
#define VTIME           10

#define NCCS            11

typedef uint32_t cc_t;
typedef uint32_t speed_t;
typedef uint32_t tcflag_t;

typedef struct {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t     c_cc[NCCS];
    speed_t  ibaud;
    speed_t  obaud;
} termios_t;

#define TTY_BUFFER_SIZE     4096

/* Filesystem information */
vfs_fsinfo_t ttyfs = {
    .name = "ttyfs",
    .istemp = true,
    .filelist = {0},
    .open = ttyfs_open,
    .mount = ttyfs_mount,
    .mknode = ttyfs_mknode,
    .sync = ttyfs_sync,
    .refresh = ttyfs_refresh,
    .read = ttyfs_read,
    .getdent = ttyfs_getdent,
    .write = ttyfs_write,
    .ioctl = ttyfs_ioctl
};

vfs_handle_t ttyfh = VFS_INVALID_HANDLE;
static lock_t tty_lock;

/* Identifying information for a node */
typedef struct {
    char    ibuff[TTY_BUFFER_SIZE];
    int64_t ibegin;
    int64_t icursor;
    int64_t isize;
    termios_t termios;
} ttyfs_ident_t;

static ttyfs_ident_t* create_ident()
{
    ttyfs_ident_t *id = (ttyfs_ident_t*)kmalloc(sizeof(ttyfs_ident_t));

    id->ibegin  = 0;
    id->icursor = 0;
    id->isize   = 0;

    memset(id->ibuff, 0, TTY_BUFFER_SIZE);
    memset(&(id->termios), 0, sizeof(termios_t));

    id->termios.c_lflag = (ISIG | ICANON | ECHO);
    id->termios.c_cc[VINTR] = 0x03;

    return id;
}

void ttyfs_init(void)
{
    /* Do nothing */
}

int64_t ttyfs_ioctl(vfs_inode_t *this, int64_t request, int64_t arg)
{
    ttyfs_ident_t *id = this->ident;
    int64_t ret = -1;

    lock_lock(&tty_lock);

    if (request == TIOCGWINSZ) {        /* 0x5413 */
        winsize_t *ws = (winsize_t*)arg;
        term_get_winsize(ws);
        ret = 0;
    } else if (request == TIOCSWINSZ) { /* 0x5413 */
        winsize_t *ws = (winsize_t*)arg;
        if (term_set_winsize(ws)) {
            ret = 0;
        }
    } else if (request == TIOCGPGRP) {  /* 0x540F */
        /* Get current process group */
        /* Do nothing */
    } else if (request == TCGETS) {     /* 0x5401 */
        termios_t *t = (termios_t*)arg;
        *t = id->termios;
        klogd("TTYFS: get termios\n");
    } else if (request == TCSETS) {     /* 0x5402 */
        termios_t *t = (termios_t*)arg;
        id->termios = *t;
        klogd("TTYFS: set termios\n");
    }

    lock_release(&tty_lock);

    if (ret < 0) cpu_set_errno(EINVAL);

    return ret;
}

vfs_tnode_t* ttyfs_open(vfs_inode_t *this, const char *path)
{
    (void)this;

    return vfs_path_to_node(path, CREATE, VFS_NODE_FOLDER);;
}

int64_t ttyfs_read(vfs_inode_t* this, size_t offset, size_t len, void *buff)
{
    ttyfs_ident_t *id = this->ident;

    lock_lock(&tty_lock);

    /* Do not care about offset in current implementation */
    (void)offset;

    /* If read less than len bytes, wait until there are enough data */
    while (id->isize < (int64_t)len) {
        event_para_t para = 0;
        if (eb_subscribe(sched_get_tid(), EVENT_KEY_PRESSED, &para)) {
            /* We maximumly backtrace half of TTY_BUFFER_SIZE to determine
             * whether the backspace key should be accepted or not
             */
            int64_t dlen = 0;
            int64_t iend = (id->icursor + id->isize) % TTY_BUFFER_SIZE;
            id->ibegin = MAX(id->ibegin, id->icursor - TTY_BUFFER_SIZE / 2)
                         % TTY_BUFFER_SIZE;
            for (int64_t k = id->ibegin; ; k++) {
                int64_t index = (id->ibegin + k) % TTY_BUFFER_SIZE;
                if (index == iend) break;
                dlen += ((id->ibuff[index] != '\b') ? 1 : -1);
            }
            uint8_t keycode = para & 0xFF;
            if ((keycode && keycode != '\b') || (keycode == '\b' && dlen > 0)) {
                id->ibuff[iend] = keycode; 
                id->isize += 1;
                if (id->isize >= TTY_BUFFER_SIZE) {
                    kpanic("TTYFS: input buffer overflow\n");
                }
            }
        }
    }

    /* OK, data is enough! Read data from input buffer */
    int64_t rlen = MIN((int64_t)len, id->isize);

    for (int64_t i = 0; i < rlen; i++) {
        int64_t index = (id->icursor + i) % TTY_BUFFER_SIZE;
        ((char*)buff)[i] = id->ibuff[index];

        cursor_visible = CURSOR_HIDE;
        term_set_cursor(' ');
        term_refresh(TERM_MODE_CLI);

        kprintf("%c", id->ibuff[index]);

        cursor_visible = CURSOR_INVISIBLE;
    }

    /* Update start and size of input buffer */
    id->icursor += rlen;
    id->icursor %= TTY_BUFFER_SIZE;
    id->isize   -= rlen;

    lock_release(&tty_lock);

    return rlen;
}

int64_t ttyfs_write(vfs_inode_t* this, size_t offset, size_t len,
                    const void* buff)
{
    ttyfs_ident_t *id = this->ident;
    int64_t wlen = 0;

    lock_lock(&tty_lock);

    /* Do not care about offset in current implementation */
    (void)offset;

    /* Reset the input buffer */
    id->ibegin  = 0;
    id->icursor = 0;
    id->isize   = 0;
    
    /* Output to the terminal */
    char *msg = (char*)kmalloc(len + 1);
    if (msg != NULL) {
        msg[len] = '\0';
        memcpy(msg, buff, len);

        cursor_visible = CURSOR_HIDE;
        term_set_cursor(' ');
        term_refresh(TERM_MODE_CLI);

        kprintf("%s", msg);

        cursor_visible = CURSOR_INVISIBLE;

        kmfree(msg);
        wlen = len;
    }

    lock_release(&tty_lock);

    return wlen;
}

int64_t ttyfs_sync(vfs_inode_t* this)
{
    (void)this;

    return 0;
}

int64_t ttyfs_refresh(vfs_inode_t* this)
{
    (void)this;

    return 0;
}

int64_t ttyfs_getdent(vfs_inode_t* this, size_t pos, vfs_dirent_t* dirent)
{
    (void)this;
    (void)pos;
    (void)dirent;

    return -1; 
}

int64_t ttyfs_mknode(vfs_tnode_t* this)
{
    this->inode->ident = create_ident();
    return 0;
}

vfs_inode_t* ttyfs_mount(vfs_inode_t* at)
{
    (void)at;

    klogi("TTYFS: mount to 0x%x and load all files from system assets\n", at);
    vfs_inode_t* ret = vfs_alloc_inode(VFS_NODE_MOUNTPOINT, 0777, 0, &ttyfs,
                                       NULL);
    ret->ident = create_ident();

    return ret;
}
