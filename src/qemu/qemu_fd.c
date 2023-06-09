/*
 * qemu_fd.c: QEMU fd and fdpass passing helpers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include "qemu_fd.h"
#include "qemu_domain.h"

#include "viralloc.h"
#include "virfile.h"
#include "virlog.h"

#define VIR_FROM_THIS VIR_FROM_QEMU
VIR_LOG_INIT("qemu.qemu_fd");

struct qemuFDPassFD {
    int fd;
    char *opaque;
};

struct _qemuFDPass {
    bool useFDSet;
    unsigned int fdSetID;
    size_t nfds;
    struct qemuFDPassFD *fds;
    char *prefix;
    char *path;

    bool passed; /* passed to qemu via monitor */
};


void
qemuFDPassFree(qemuFDPass *fdpass)
{
    size_t i;

    if (!fdpass)
        return;

    for (i = 0; i < fdpass->nfds; i++) {
        VIR_FORCE_CLOSE(fdpass->fds[i].fd);
        g_free(fdpass->fds[i].opaque);
    }

    g_free(fdpass->fds);
    g_free(fdpass->prefix);
    g_free(fdpass->path);
    g_free(fdpass);
}


static int
qemuFDPassValidate(qemuFDPass *fdpass)
{
    size_t i;

    if (!fdpass->useFDSet &&
        fdpass->nfds > 1) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("direct FD passing supports only 1 file descriptor"));
        return -1;
    }

    for (i = 0; i < fdpass->nfds; i++) {
        if (fdpass->fds[i].fd < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("invalid file descriptor"));
            return -1;
        }
    }

    return 0;
}


/**
 * qemuFDPassNew:
 * @prefix: prefix used for naming the passed FDs
 * @dompriv: qemu domain private data
 *
 * Create a new helper object for passing FDs to QEMU. The instance created
 * via 'qemuFDPassNew' will result in the fd passed via a 'fdset' (/dev/fdset/N).
 *
 * Non-test uses must pass a valid @dompriv.
 *
 * @prefix is used as prefix for naming the fd in QEMU.
 */
qemuFDPass *
qemuFDPassNew(const char *prefix,
              void *dompriv)
{
    qemuDomainObjPrivate *priv = dompriv;
    qemuFDPass *fdpass = g_new0(qemuFDPass, 1);

    fdpass->prefix = g_strdup(prefix);
    fdpass->useFDSet = true;

    if (priv) {
        fdpass->fdSetID = qemuDomainFDSetIDNew(priv);
        fdpass->path = g_strdup_printf("/dev/fdset/%u", fdpass->fdSetID);
    } else {
        fdpass->path = g_strdup_printf("/dev/fdset/monitor-fake");
    }

    return fdpass;
}


/**
 * qemuFDPassNewDirect:
 * @prefix: prefix used for naming the passed FDs
 * @dompriv: qemu domain private data
 *
 * Create a new helper object for passing FDs to QEMU.
 *
 * The instance created via 'qemuFDPassNewDirect' will result in the older
 * approach of directly using FD number on the commandline and 'getfd'
 * QMP command.
 *
 * Non-test uses must pass a valid @dompriv.
 *
 * @prefix is used for naming the FD if needed and is later referenced when
 * removing the FDSet via monitor.
 */
qemuFDPass *
qemuFDPassNewDirect(const char *prefix,
                    void *dompriv G_GNUC_UNUSED)
{
    qemuFDPass *fdpass = g_new0(qemuFDPass, 1);

    fdpass->prefix = g_strdup(prefix);

    return fdpass;
}


/**
 * qemuFDPassAddFD:
 * @fdpass: The fd passing helper struct
 * @fd: File descriptor to pass
 * @suffix: Name suffix for the file descriptor name
 *
 * Adds @fd to be passed to qemu when transferring @fdpass to qemu. When @fdpass
 * is configured to use FD set mode, multiple file descriptors can be passed by
 * calling this function repeatedly.
 *
 * @suffix is used to build the name of the file descriptor by concatenating
 * it with @prefix passed to qemuFDPassNew. @suffix may be NULL, in which case
 * it's considered to be an empty string.
 */
void
qemuFDPassAddFD(qemuFDPass *fdpass,
                int *fd,
                const char *suffix)
{
    struct qemuFDPassFD newfd = { .fd = *fd };

    *fd = -1;

    newfd.opaque = g_strdup_printf("%s%s", fdpass->prefix, NULLSTR_EMPTY(suffix));

    if (!fdpass->useFDSet) {
        g_free(fdpass->path);
        fdpass->path = g_strdup(newfd.opaque);
    }

    VIR_APPEND_ELEMENT(fdpass->fds, fdpass->nfds, newfd);
}


/**
 * qemuFDPassTransferCommand:
 * @fdpass: The fd passing helper struct
 * @cmd: Command to pass the filedescriptors to
 *
 * Pass the fds in @fdpass to a commandline object @cmd. @fdpass may be NULL
 * in which case this is a no-op.
 */
int
qemuFDPassTransferCommand(qemuFDPass *fdpass,
                          virCommand *cmd)
{
    size_t i;

    if (!fdpass)
        return 0;

    if (qemuFDPassValidate(fdpass) < 0)
        return -1;

    for (i = 0; i < fdpass->nfds; i++) {
        virCommandPassFD(cmd, fdpass->fds[i].fd, VIR_COMMAND_PASS_FD_CLOSE_PARENT);

        if (fdpass->useFDSet) {
            g_autofree char *arg = NULL;

            arg = g_strdup_printf("set=%u,fd=%d,opaque=%s",
                                  fdpass->fdSetID,
                                  fdpass->fds[i].fd,
                                  fdpass->fds[i].opaque);

            virCommandAddArgList(cmd, "-add-fd", arg, NULL);
        } else {
            /* for monitor use the older FD passing needs the FD number */
            g_free(fdpass->path);
            fdpass->path = g_strdup_printf("%d", fdpass->fds[i].fd);
        }

        fdpass->fds[i].fd = -1;
    }

    return 0;
}


/**
 * qemuFDPassTransferMonitor:
 * @fdpass: The fd passing helper struct
 * @mon: monitor object
 *
 * Pass the fds in @fdpass to qemu via the monitor. @fdpass may be NULL
 * in which case this is a no-op. Caller needs to enter the monitor context.
 */
int
qemuFDPassTransferMonitor(qemuFDPass *fdpass,
                          qemuMonitor *mon)
{
    size_t i;

    if (!fdpass)
        return 0;

    if (qemuFDPassValidate(fdpass) < 0)
        return -1;
    if (fdpass->useFDSet) {
        g_autoptr(qemuMonitorFdsets) fdsets = NULL;

        if (qemuMonitorQueryFdsets(mon, &fdsets) < 0)
            return -1;

        for (i = 0; i < fdsets->nfdsets; i++) {
            if (fdsets->fdsets[i].id == fdpass->fdSetID) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("fdset '%u' is already in use by qemu"),
                               fdpass->fdSetID);
                return -1;
            }
        }
    }

    for (i = 0; i < fdpass->nfds; i++) {
        if (fdpass->useFDSet) {
            if (qemuMonitorAddFileHandleToSet(mon,
                                              fdpass->fds[i].fd,
                                              fdpass->fdSetID,
                                              fdpass->fds[i].opaque) < 0)
                return -1;
        } else {
            if (qemuMonitorSendFileHandle(mon,
                                          fdpass->fds[i].opaque,
                                          fdpass->fds[i].fd) < 0)
                return -1;
        }

        VIR_FORCE_CLOSE(fdpass->fds[i].fd);
        fdpass->passed = true;
    }

    return 0;
}


/**
 * qemuFDPassTransferMonitorRollback:
 * @fdpass: The fd passing helper struct
 * @mon: monitor object
 *
 * Rolls back the addition of @fdpass to @mon if it was added originally.
 */
void
qemuFDPassTransferMonitorRollback(qemuFDPass *fdpass,
                                  qemuMonitor *mon)
{
    if (!fdpass || !fdpass->passed)
        return;

    if (fdpass->useFDSet) {
        ignore_value(qemuMonitorRemoveFdset(mon, fdpass->fdSetID));
    } else {
        ignore_value(qemuMonitorCloseFileHandle(mon, fdpass->fds[0].opaque));
    }
}


/**
 * qemuFDPassGetPath:
 * @fdpass: The fd passing helper struct
 *
 * Returns the path/fd name that is used in qemu to refer to the passed FD.
 * Note that it's only valid to call this function after @fdpass was already
 * transferred to the command or monitor.
 */
const char *
qemuFDPassGetPath(qemuFDPass *fdpass)
{
    if (!fdpass)
        return NULL;

    return fdpass->path;
}
