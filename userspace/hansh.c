#include <stddef.h>
#include <stdint.h>

#include <libc/stdio.h>
#include <libc/string.h>
#include <libc/sysfunc.h>

/* Parsed command representation */
/* Currently we only support EXEC */
#define EXEC    1
#define REDIR   2
#define PIPE    3
#define LIST    4
#define BACK    5

#define MAXARGS 10

#define CMD_MAX_LEN     100
#define CMD_PROMPT      "\033[36m$ \033[0m"

static command_help_t help_msg[] = { 
    {"<help> cd",       "Change current directoy."},
    {"<help> mem",      "Display memory usage information."},
};

struct cmd {
    int type;
};

struct execcmd {
    int type;
    char *argv[MAXARGS];
    char *eargv[MAXARGS];
};

struct redircmd {
    int type;
    struct cmd *cmd;
    char *file;
    char *efile;
    int mode;
    int fd;
};

struct pipecmd {
    int type;
    struct cmd *left;
    struct cmd *right;
};

struct listcmd {
    int type;
    struct cmd *left;
    struct cmd *right;
};

struct backcmd {
    int type;
    struct cmd *cmd;
};

int fork1(void);  /* Fork but panics on failure. */
struct cmd *parsecmd(char*);

/* Execute cmd.  Never returns. */
void runcmd(struct cmd *cmd)
{
    int p[2] = {0};
    char pathname[CMD_MAX_LEN] = {0};
    struct backcmd *bcmd;
    struct execcmd *ecmd;
    struct listcmd *lcmd;
    struct pipecmd *pcmd;
    struct redircmd *rcmd;

    if(cmd == 0) {
        sys_exit(1);
    }

    switch(cmd->type){
    default:
        sys_panic("runcmd");

    case EXEC:
        ecmd = (struct execcmd*)cmd;
        if(ecmd->argv[0] == 0)
            sys_exit(1);
        strcpy(pathname, "");
        if (pathname[0] != '/') {
            strcpy(pathname, "/bin/");
        }
        strcat(pathname, ecmd->argv[0]);
        if (sys_exec(pathname, ecmd->argv) < 0) {
            printf("exec ");
            printf(ecmd->argv[0]);
            printf(" failed\n");
        }
        break;

  case PIPE:
        pcmd = (struct pipecmd*)cmd;
        if(sys_pipe(p) < 0)
            sys_panic("pipe");
        sys_libc_log("hansh: start to fork pipe processes for left and right tasks\n");
        if(fork1() == 0) {
            /* Child process */
            sys_dup(STDOUT, 0, p[1]);
            runcmd(pcmd->left);
            sys_close(p[1]);
            /* Never run below code */
            sys_exit(0);
        }
        if(fork1() == 0) {
            /* Child process */
            sys_dup(STDIN, 0, p[0]);
            runcmd(pcmd->right);
            sys_close(p[0]);
            /* Never run below code */
            sys_exit(0);
        }
        sys_wait(-1);
        sys_wait(-1);
        break;

    case LIST:
        lcmd = (struct listcmd*)cmd;
        if(fork1() == 0)
            runcmd(lcmd->left);
        sys_wait(-1);
        runcmd(lcmd->right);
        break;
  }
  sys_exit(0);
}

int getcmd(char *buf, int nbuf)
{
    int i;
    sys_write(STDOUT, CMD_PROMPT, strlen(CMD_PROMPT));
    memset(buf, 0, nbuf);
    for (i = 0; ; ) {
        if (sys_read(STDIN, &buf[i], 1) != 1) {
            break;
        }
        if (buf[i] == '\b') {
            if (i > 0) {
                buf[i - 1] = '\0';
                i--;
            }
            buf[i] = '\0';
            continue;
        }
        if (i >= nbuf - 1) break;
        if (buf[i] == (char)EOF) break;
        if (buf[i] == '\n') {
            buf[i] = '\0';
            break;
        }
        i++;
    }
    if(buf[0] == (char)EOF)
        return -1;
    return 0;
}

void main(void)
{
    /* static char buf[100]; */
    char *buf = (char*)sys_malloc(CMD_MAX_LEN);
    int fd;

    /* TODO: Ensure that three file descriptors are open. */

    /* Read and run input commands. */
    while(getcmd(buf, CMD_MAX_LEN) >= 0){
        if(buf[0] == 'c' && buf[1] == 'd' && buf[2] == ' ') {
            /* Chdir must be called by the parent, not the child. */
            if (buf[strlen(buf) - 1] == '\n') {
                buf[strlen(buf) - 1] = 0;  /* chop \n */
            }
            if(sys_chdir(buf + 3) < 0)
                printf("cd: cannot change folder to \"%s\"\n", buf + 3);
            continue;
        } else if(buf[0] == 'm' && buf[1] == 'e' && buf[2] == 'm'
                  && buf[3] == '\0')
        {
            if(sys_meminfo() < 0)
                printf("mem: cannot display memory usage information\n"); 
            continue;
        }

        if(buf[0] == 0) continue;

        if(fork1() == 0) {
            runcmd(parsecmd(buf));
            sys_exit(0);
        }
        sys_wait(-1);
        sys_libc_log("hansh: exit from current command and wait for next one");
    }
    printf("exit: ending sh\n");
    sys_exit(0);
}

int fork1(void)
{
    int pid;

    pid = sys_fork();
    if(pid == -1)
        sys_panic("fork");
    return pid;
}

/**--------------------------------------------------------------------------**/

struct cmd* execcmd(void)
{
    struct execcmd *cmd;

    cmd = sys_malloc(sizeof(*cmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = EXEC;
    return (struct cmd*)cmd;
}

struct cmd *redircmd(
    struct cmd *subcmd, char *file, char *efile, int mode, int fd)
{
    struct redircmd *cmd;

    cmd = sys_malloc(sizeof(*cmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = REDIR;
    cmd->cmd = subcmd;
    cmd->file = file;
    cmd->efile = efile;
    cmd->mode = mode;
    cmd->fd = fd;
    return (struct cmd*)cmd;
}

struct cmd *pipecmd(struct cmd *left, struct cmd *right)
{
    struct pipecmd *cmd;

    cmd = sys_malloc(sizeof(*cmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = PIPE;
    cmd->left = left;
    cmd->right = right;
    return (struct cmd*)cmd;
}

struct cmd *listcmd(struct cmd *left, struct cmd *right)
{
    struct listcmd *cmd;

    cmd = sys_malloc(sizeof(*cmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = LIST;
    cmd->left = left;
    cmd->right = right;
    return (struct cmd*)cmd;
}

struct cmd *backcmd(struct cmd *subcmd)
{
    struct backcmd *cmd;

    cmd = sys_malloc(sizeof(*cmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = BACK;
    cmd->cmd = subcmd;
    return (struct cmd*)cmd;
}

/**--------------------------------------------------------------------------**/

char whitespace[] = " \t\r\n\v";
char symbols[] = "<|>&;()";

int gettoken(char **ps, char *es, char **q, char **eq)
{
    char *s;
    int ret;

    s = *ps;
    while(s < es && strchr(whitespace, *s))
        s++;
    if(q)
        *q = s;
    ret = *s;
    switch(*s){
    case 0:
        break;
    case '|':
    case '(':
    case ')':
    case ';':
    case '&':
    case '<':
        s++;
        break;
    case '>':
        s++;
        if(*s == '>') {
            ret = '+';
            s++;
        }
        break;
    default:
        ret = 'a';
        while(s < es && !strchr(whitespace, *s) && !strchr(symbols, *s))
            s++;
        break;
    }

    if(eq)
        *eq = s;

    while(s < es && strchr(whitespace, *s))
        s++;

    *ps = s;

    return ret;
}

int peek(char **ps, char *es, char *toks)
{
    char *s;

    s = *ps;
    while(s < es && strchr(whitespace, *s))
        s++;
    *ps = s;
    return *s && strchr(toks, *s);
}

struct cmd *parseline(char**, char*);
struct cmd *parsepipe(char**, char*);
struct cmd *parseexec(char**, char*);
struct cmd *nulterminate(struct cmd*);

struct cmd *parsecmd(char *s)
{
    char *es;
    struct cmd *cmd;

    es = s + strlen(s);
    cmd = parseline(&s, es);
    peek(&s, es, "");
    if(s != es){
        /* fprintf(2, "leftovers: %s\n", s); */
        sys_panic("syntax");
    }
    nulterminate(cmd);
    return cmd;
}

struct cmd *parseline(char **ps, char *es)
{
    struct cmd *cmd;

    cmd = parsepipe(ps, es);
    while(peek(ps, es, "&")) {
        gettoken(ps, es, 0, 0);
        cmd = backcmd(cmd);
    }
    if(peek(ps, es, ";")) {
        gettoken(ps, es, 0, 0);
        cmd = listcmd(cmd, parseline(ps, es));
    }
    return cmd;
}

struct cmd *parsepipe(char **ps, char *es)
{
    struct cmd *cmd;

    cmd = parseexec(ps, es);
    if(peek(ps, es, "|")){
        gettoken(ps, es, 0, 0);
        cmd = pipecmd(cmd, parsepipe(ps, es));
    }
    return cmd;
}

struct cmd *parseredirs(struct cmd *cmd, char **ps, char *es)
{
    int tok;
    char *q, *eq;
    while(peek(ps, es, "<>")){
        tok = gettoken(ps, es, 0, 0);
        if(gettoken(ps, es, &q, &eq) != 'a')
            sys_panic("missing file for redirection");
        switch(tok){
        case '<':
            /* TODO: Need to review and support in the future */
            /* cmd = redircmd(cmd, q, eq, O_RDONLY, 0); */
            break;
        case '>':
            /* cmd = redircmd(cmd, q, eq, O_WRONLY|O_CREATE|O_TRUNC, 1); */
            break;
        case '+':  /* >> */
            /* cmd = redircmd(cmd, q, eq, O_WRONLY|O_CREATE, 1); */
            break;
        }
    }
    return cmd;
}

struct cmd *parseblock(char **ps, char *es)
{
    struct cmd *cmd;

    if(!peek(ps, es, "("))
        sys_panic("parseblock");
    gettoken(ps, es, 0, 0);
    cmd = parseline(ps, es);
    if(!peek(ps, es, ")"))
        sys_panic("syntax - missing )");
    gettoken(ps, es, 0, 0);
    cmd = parseredirs(cmd, ps, es);
    return cmd;
}

struct cmd *parseexec(char **ps, char *es)
{
    char *q, *eq;
    int tok, argc;
    struct execcmd *cmd;
    struct cmd *ret;

    if(peek(ps, es, "("))
        return parseblock(ps, es);

    ret = execcmd();
    cmd = (struct execcmd*)ret;

    argc = 0;
    ret = parseredirs(ret, ps, es);
    while(!peek(ps, es, "|)&;")){
        if((tok = gettoken(ps, es, &q, &eq)) == 0)
            break;
        if(tok != 'a')
            sys_panic("syntax");
        cmd->argv[argc] = q;
        cmd->eargv[argc] = eq;
        argc++;
        if(argc >= MAXARGS)
            sys_panic("too many args");
        ret = parseredirs(ret, ps, es);
    }
    cmd->argv[argc] = 0;
    cmd->eargv[argc] = 0;
    return ret;
}

/* NUL-terminate all the counted strings. */
struct cmd *nulterminate(struct cmd *cmd)
{
    int i;
    struct backcmd *bcmd;
    struct execcmd *ecmd;
    struct listcmd *lcmd;
    struct pipecmd *pcmd;
    struct redircmd *rcmd;

    if(cmd == 0)
        return 0;

    switch(cmd->type){
    case EXEC:
        ecmd = (struct execcmd*)cmd;
        for(i = 0; ecmd->argv[i]; i++)
            *ecmd->eargv[i] = 0;
        break;

    case REDIR:
        rcmd = (struct redircmd*)cmd;
        nulterminate(rcmd->cmd);
        *rcmd->efile = 0;
        break;

    case PIPE:
        pcmd = (struct pipecmd*)cmd;
        nulterminate(pcmd->left);
        nulterminate(pcmd->right);
        break;

    case LIST:
        lcmd = (struct listcmd*)cmd;
        nulterminate(lcmd->left);
        nulterminate(lcmd->right);
        break;

    case BACK:
        bcmd = (struct backcmd*)cmd;
        nulterminate(bcmd->cmd);
        break;
    }
    return cmd;
}

