#pragma once

#define EDOM            1       /* Math argument out of domain of func */
#define EILSEQ          2       /* Illegal byte sequence */
#define ERANGE          3       /* Math result not representable */

#define E2BIG           1001    /* Arg list too long */
#define EACCES          1002    /* Permission denied */
#define EADDRINUSE      1003    /* Address already in use */
#define EADDRNOTAVAIL   1004    /* Cannot assign requested address */
#define EAFNOSUPPORT    1005    /* Address family not supported */
#define EAGAIN          1006    /* Try again */
#define EALREADY        1007    /* Operation already in progress */
#define EBADF           1008    /* Bad file descriptor */
#define EBADMSG         1009    /* Not a data message */
#define EBUSY           1010    /* Device or resource busy */
#define ECANCELED       1011
#define ECHILD          1012    /* No child processes */
#define ECONNABORTED    1013    /* Software caused connection abort */
#define ECONNREFUSED    1014    /* Connection refused */
#define ECONNRESET      1015    /* Connection reset by peer */
#define EDEADLK         1016    /* Resource deadlock would occur */
#define EDESTADDRREQ    1017    /* Destination address required */
#define EDQUOT          1018    /* Quota exceeded */
#define EEXIST          1019    /* File already exists */
#define EFAULT          1020    /* Bad address */
#define EFBIG           1021    /* File too large */
#define EHOSTUNREACH    1022    /* No route to host */
#define EIDRM           1023    /* Identifier removed */
#define EINPROGRESS     1024    /* Operation now in progress */
#define EINTR           1025    /* Interrupted system call */
#define EINVAL          1026    /* Invalid argument */
#define EIO             1027    /* I/O error */
#define EISCONN         1028    /* Transport endpoint is already connected */
#define EISDIR          1029    /* Is a directory */
#define ELOOP           1030    /* Too many symbolic links encountered */
#define EMFILE          1031    /* Too many open files */
#define EMLINK          1032    /* Too many links */
#define EMSGSIZE        1034    /* Message too long */
#define EMULTIHOP       1035    /* Multihop attempted */
#define ENAMETOOLONG    1036    /* File name too long */
#define ENETDOWN        1037    /* Network is down */
#define ENETRESET       1038    /* Network dropped connection because of
                                 * reset */
#define ENETUNREACH     1039    /* Network is unreachable */
#define ENFILE          1040    /* File table overflow */
#define ENOBUFS         1041    /* No buffer space available */
#define ENODEV          1042    /* No such device */
#define ENOENT          1043    /* No such file or directory */
#define ENOEXEC         1044    /* Exec format error */
#define ENOLCK          1045    /* No record locks available */
#define ENOLINK         1046    /* No links */
#define ENOMEM          1047    /* Out of memory */
#define ENOMSG          1048    /* No message of desired type */
#define ENOPROTOOPT     1049    /* Protocol not available */
#define ENOSPC          1050    /* No space left on device */
#define ENOSYS          1051    /* Function not implemented */
#define ENOTCONN        1052    /* Transport endpoint is not connected */
#define ENOTDIR         1053    /* Not a directory */
#define ENOTEMPTY       1054    /* Directory not empty */
#define ENOTRECOVERABLE 1055
#define ENOTSOCK        1056    /* Argument is not a socket */
#define ENOTSUP         1057
#define ENOTTY          1058    /* Not a typewriter */
#define ENXIO           1059    /* No such device or address */
#define EOPNOTSUPP      1060    /* Operation not supported on transport
                                 * endpoint */
#define EOVERFLOW       1061    /* Value too large for defined data type */
#define EOWNERDEAD      1062
#define EPERM           1063    /* Operation not permitted */
#define EPIPE           1064    /* Broken pipe */
#define EPROTO          1065    /* Protocol error */
#define EPROTONOSUPPORT 1066    /* Protocol not supported */
#define EPROTOTYPE      1067    /* Protocol wrong type for socket */
#define EROFS           1068    /* Read-only file system */
#define ESPIPE          1069    /* Illegal seek */
#define ESRCH           1070    /* No such process */
#define ESTALE          1071    /* Stale NFS file handle */
#define ETIMEDOUT       1072    /* Connection timed out */
#define ETXTBSY         1073    /* Text file busy */
#define EWOULDBLOCK     EAGAIN  /* Operation would block */
#define EXDEV           1075    /* Cross-device link */
#define ENODATA         1076    /* No data available */
#define ETIME           1077    /* Timer expired */
#define ENOKEY          1078
#define ESHUTDOWN       1079
#define EHOSTDOWN       1080
#define EBADFD          1081    /* File descriptor in bad state */
#define ENOMEDIUM       1082    /* No medium found */
#define ENOTBLK         1083    /* Block device required */

