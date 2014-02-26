/*
 * libxl_domain.c: libxl domain object private state
 *
 * Copyright (C) 2011-2014 SUSE LINUX Products GmbH, Nuernberg, Germany.
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
 *
 * Authors:
 *     Jim Fehlig <jfehlig@suse.com>
 */

#include <config.h>

#include <fcntl.h>

#include "libxl_domain.h"

#include "viralloc.h"
#include "viratomic.h"
#include "virfile.h"
#include "virerror.h"
#include "virlog.h"
#include "virstring.h"
#include "virtime.h"

#define VIR_FROM_THIS VIR_FROM_LIBXL

VIR_LOG_INIT("libxl.libxl_domain");

VIR_ENUM_IMPL(libxlDomainJob, LIBXL_JOB_LAST,
              "none",
              "query",
              "destroy",
              "modify",
);

/* Object used to store info related to libxl event registrations */
typedef struct _libxlEventHookInfo libxlEventHookInfo;
typedef libxlEventHookInfo *libxlEventHookInfoPtr;
struct _libxlEventHookInfo {
    libxlEventHookInfoPtr next;
    libxlDomainObjPrivatePtr priv;
    void *xl_priv;
    int id;
};

static virClassPtr libxlDomainObjPrivateClass;

static void
libxlDomainObjPrivateDispose(void *obj);

static int
libxlDomainObjPrivateOnceInit(void)
{
    if (!(libxlDomainObjPrivateClass = virClassNew(virClassForObjectLockable(),
                                                   "libxlDomainObjPrivate",
                                                   sizeof(libxlDomainObjPrivate),
                                                   libxlDomainObjPrivateDispose)))
        return -1;

    return 0;
}

VIR_ONCE_GLOBAL_INIT(libxlDomainObjPrivate)

static void
libxlDomainObjFDEventHookInfoFree(void *obj)
{
    VIR_FREE(obj);
}

static void
libxlDomainObjTimerEventHookInfoFree(void *obj)
{
    libxlEventHookInfoPtr info = obj;

    /* Drop reference on libxlDomainObjPrivate */
    virObjectUnref(info->priv);
    VIR_FREE(info);
}

static void
libxlDomainObjFDEventCallback(int watch ATTRIBUTE_UNUSED,
                              int fd,
                              int vir_events,
                              void *fd_info)
{
    libxlEventHookInfoPtr info = fd_info;
    int events = 0;

    virObjectLock(info->priv);
    if (vir_events & VIR_EVENT_HANDLE_READABLE)
        events |= POLLIN;
    if (vir_events & VIR_EVENT_HANDLE_WRITABLE)
        events |= POLLOUT;
    if (vir_events & VIR_EVENT_HANDLE_ERROR)
        events |= POLLERR;
    if (vir_events & VIR_EVENT_HANDLE_HANGUP)
        events |= POLLHUP;

    virObjectUnlock(info->priv);
    libxl_osevent_occurred_fd(info->priv->ctx, info->xl_priv, fd, 0, events);
}

static int
libxlDomainObjFDRegisterEventHook(void *priv,
                                  int fd,
                                  void **hndp,
                                  short events,
                                  void *xl_priv)
{
    int vir_events = VIR_EVENT_HANDLE_ERROR;
    libxlEventHookInfoPtr info;

    if (VIR_ALLOC(info) < 0)
        return -1;

    info->priv = priv;
    info->xl_priv = xl_priv;

    if (events & POLLIN)
        vir_events |= VIR_EVENT_HANDLE_READABLE;
    if (events & POLLOUT)
        vir_events |= VIR_EVENT_HANDLE_WRITABLE;

    info->id = virEventAddHandle(fd, vir_events, libxlDomainObjFDEventCallback,
                                 info, libxlDomainObjFDEventHookInfoFree);
    if (info->id < 0) {
        VIR_FREE(info);
        return -1;
    }

    *hndp = info;

    return 0;
}

static int
libxlDomainObjFDModifyEventHook(void *priv ATTRIBUTE_UNUSED,
                                int fd ATTRIBUTE_UNUSED,
                                void **hndp,
                                short events)
{
    libxlEventHookInfoPtr info = *hndp;
    int vir_events = VIR_EVENT_HANDLE_ERROR;

    virObjectLock(info->priv);
    if (events & POLLIN)
        vir_events |= VIR_EVENT_HANDLE_READABLE;
    if (events & POLLOUT)
        vir_events |= VIR_EVENT_HANDLE_WRITABLE;

    virEventUpdateHandle(info->id, vir_events);
    virObjectUnlock(info->priv);

    return 0;
}

static void
libxlDomainObjFDDeregisterEventHook(void *priv ATTRIBUTE_UNUSED,
                                    int fd ATTRIBUTE_UNUSED,
                                    void *hnd)
{
    libxlEventHookInfoPtr info = hnd;
    libxlDomainObjPrivatePtr p = info->priv;

    virObjectLock(p);
    virEventRemoveHandle(info->id);
    virObjectUnlock(p);
}

static void
libxlDomainObjTimerCallback(int timer ATTRIBUTE_UNUSED, void *timer_info)
{
    libxlEventHookInfoPtr info = timer_info;
    libxlDomainObjPrivatePtr p = info->priv;

    virObjectLock(p);
    /*
     * libxl expects the event to be deregistered when calling
     * libxl_osevent_occurred_timeout, but we dont want the event info
     * destroyed.  Disable the timeout and only remove it after returning
     * from libxl.
     */
    virEventUpdateTimeout(info->id, -1);
    virObjectUnlock(p);
    libxl_osevent_occurred_timeout(p->ctx, info->xl_priv);
    virObjectLock(p);
    virEventRemoveTimeout(info->id);
    virObjectUnlock(p);
}

static int
libxlDomainObjTimeoutRegisterEventHook(void *priv,
                                       void **hndp,
                                       struct timeval abs_t,
                                       void *xl_priv)
{
    libxlEventHookInfoPtr info;
    struct timeval now;
    struct timeval res;
    static struct timeval zero;
    int timeout;

    if (VIR_ALLOC(info) < 0)
        return -1;

    info->priv = priv;
    /*
     * Also take a reference on the domain object.  Reference is dropped in
     * libxlDomainObjEventHookInfoFree, ensuring the domain object outlives the
     * timeout event objects.
     */
    virObjectRef(info->priv);
    info->xl_priv = xl_priv;

    gettimeofday(&now, NULL);
    timersub(&abs_t, &now, &res);
    /* Ensure timeout is not overflowed */
    if (timercmp(&res, &zero, <)) {
        timeout = 0;
    } else if (res.tv_sec > INT_MAX / 1000) {
        timeout = INT_MAX;
    } else {
        timeout = res.tv_sec * 1000 + (res.tv_usec + 999) / 1000;
    }
    info->id = virEventAddTimeout(timeout, libxlDomainObjTimerCallback,
                                  info, libxlDomainObjTimerEventHookInfoFree);
    if (info->id < 0) {
        virObjectUnref(info->priv);
        VIR_FREE(info);
        return -1;
    }

    *hndp = info;

    return 0;
}

/*
 * Note:  There are two changes wrt timeouts starting with xen-unstable
 * changeset 26469:
 *
 * 1. Timeout modify callbacks will only be invoked with an abs_t of {0,0},
 * i.e. make the timeout fire immediately.  Prior to this commit, timeout
 * modify callbacks were never invoked.
 *
 * 2. Timeout deregister hooks will no longer be called.
 */
static int
libxlDomainObjTimeoutModifyEventHook(void *priv ATTRIBUTE_UNUSED,
                                     void **hndp,
                                     struct timeval abs_t ATTRIBUTE_UNUSED)
{
    libxlEventHookInfoPtr info = *hndp;

    virObjectLock(info->priv);
    /* Make the timeout fire */
    virEventUpdateTimeout(info->id, 0);
    virObjectUnlock(info->priv);

    return 0;
}

static void
libxlDomainObjTimeoutDeregisterEventHook(void *priv ATTRIBUTE_UNUSED,
                                         void *hnd)
{
    libxlEventHookInfoPtr info = hnd;
    libxlDomainObjPrivatePtr p = info->priv;

    virObjectLock(p);
    virEventRemoveTimeout(info->id);
    virObjectUnlock(p);
}


static const libxl_osevent_hooks libxl_event_callbacks = {
    .fd_register = libxlDomainObjFDRegisterEventHook,
    .fd_modify = libxlDomainObjFDModifyEventHook,
    .fd_deregister = libxlDomainObjFDDeregisterEventHook,
    .timeout_register = libxlDomainObjTimeoutRegisterEventHook,
    .timeout_modify = libxlDomainObjTimeoutModifyEventHook,
    .timeout_deregister = libxlDomainObjTimeoutDeregisterEventHook,
};

static int
libxlDomainObjInitJob(libxlDomainObjPrivatePtr priv)
{
    memset(&priv->job, 0, sizeof(priv->job));

    if (virCondInit(&priv->job.cond) < 0)
        return -1;

    return 0;
}

static void
libxlDomainObjResetJob(libxlDomainObjPrivatePtr priv)
{
    struct libxlDomainJobObj *job = &priv->job;

    job->active = LIBXL_JOB_NONE;
    job->owner = 0;
}

static void
libxlDomainObjFreeJob(libxlDomainObjPrivatePtr priv)
{
    ignore_value(virCondDestroy(&priv->job.cond));
}

/* Give up waiting for mutex after 30 seconds */
#define LIBXL_JOB_WAIT_TIME (1000ull * 30)

/*
 * obj must be locked before calling, libxlDriverPrivatePtr must NOT be locked
 *
 * This must be called by anything that will change the VM state
 * in any way
 *
 * Upon successful return, the object will have its ref count increased,
 * successful calls must be followed by EndJob eventually
 */
int
libxlDomainObjBeginJob(libxlDriverPrivatePtr driver ATTRIBUTE_UNUSED,
                       virDomainObjPtr obj,
                       enum libxlDomainJob job)
{
    libxlDomainObjPrivatePtr priv = obj->privateData;
    unsigned long long now;
    unsigned long long then;

    if (virTimeMillisNow(&now) < 0)
        return -1;
    then = now + LIBXL_JOB_WAIT_TIME;

    virObjectRef(obj);

    while (priv->job.active) {
        VIR_DEBUG("Wait normal job condition for starting job: %s",
                  libxlDomainJobTypeToString(job));
        if (virCondWaitUntil(&priv->job.cond, &obj->parent.lock, then) < 0)
            goto error;
    }

    libxlDomainObjResetJob(priv);

    VIR_DEBUG("Starting job: %s", libxlDomainJobTypeToString(job));
    priv->job.active = job;
    priv->job.owner = virThreadSelfID();

    return 0;

error:
    VIR_WARN("Cannot start job (%s) for domain %s;"
             " current job is (%s) owned by (%d)",
             libxlDomainJobTypeToString(job),
             obj->def->name,
             libxlDomainJobTypeToString(priv->job.active),
             priv->job.owner);

    if (errno == ETIMEDOUT)
        virReportError(VIR_ERR_OPERATION_TIMEOUT,
                       "%s", _("cannot acquire state change lock"));
    else
        virReportSystemError(errno,
                             "%s", _("cannot acquire job mutex"));

    virObjectUnref(obj);
    return -1;
}

/*
 * obj must be locked before calling
 *
 * To be called after completing the work associated with the
 * earlier libxlDomainBeginJob() call
 *
 * Returns true if the remaining reference count on obj is
 * non-zero, false if the reference count has dropped to zero
 * and obj is disposed.
 */
bool
libxlDomainObjEndJob(libxlDriverPrivatePtr driver ATTRIBUTE_UNUSED,
                     virDomainObjPtr obj)
{
    libxlDomainObjPrivatePtr priv = obj->privateData;
    enum libxlDomainJob job = priv->job.active;

    VIR_DEBUG("Stopping job: %s",
              libxlDomainJobTypeToString(job));

    libxlDomainObjResetJob(priv);
    virCondSignal(&priv->job.cond);

    return virObjectUnref(obj);
}

static void *
libxlDomainObjPrivateAlloc(void)
{
    libxlDomainObjPrivatePtr priv;

    if (libxlDomainObjPrivateInitialize() < 0)
        return NULL;

    if (!(priv = virObjectLockableNew(libxlDomainObjPrivateClass)))
        return NULL;

    if (!(priv->devs = virChrdevAlloc())) {
        virObjectUnref(priv);
        return NULL;
    }

    if (libxlDomainObjInitJob(priv) < 0) {
        virChrdevFree(priv->devs);
        virObjectUnref(priv);
        return NULL;
    }

    return priv;
}

static void
libxlDomainObjPrivateDispose(void *obj)
{
    libxlDomainObjPrivatePtr priv = obj;

    if (priv->deathW)
        libxl_evdisable_domain_death(priv->ctx, priv->deathW);

    libxlDomainObjFreeJob(priv);
    virChrdevFree(priv->devs);
    libxl_ctx_free(priv->ctx);
    if (priv->logger_file)
        VIR_FORCE_FCLOSE(priv->logger_file);

    xtl_logger_destroy(priv->logger);
}

static void
libxlDomainObjPrivateFree(void *data)
{
    libxlDomainObjPrivatePtr priv = data;

    if (priv->deathW) {
        libxl_evdisable_domain_death(priv->ctx, priv->deathW);
        priv->deathW = NULL;
    }

    virObjectUnref(priv);
}

virDomainXMLPrivateDataCallbacks libxlDomainXMLPrivateDataCallbacks = {
    .alloc = libxlDomainObjPrivateAlloc,
    .free = libxlDomainObjPrivateFree,
};


static int
libxlDomainDeviceDefPostParse(virDomainDeviceDefPtr dev,
                              const virDomainDef *def,
                              virCapsPtr caps ATTRIBUTE_UNUSED,
                              void *opaque ATTRIBUTE_UNUSED)
{
    if (dev->type == VIR_DOMAIN_DEVICE_CHR &&
        dev->data.chr->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE &&
        dev->data.chr->targetType == VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_NONE &&
        STRNEQ(def->os.type, "hvm"))
        dev->data.chr->targetType = VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_XEN;

    if (dev->type == VIR_DOMAIN_DEVICE_HOSTDEV) {
        virDomainHostdevDefPtr hostdev = dev->data.hostdev;

        if (hostdev->mode == VIR_DOMAIN_HOSTDEV_MODE_SUBSYS &&
            hostdev->source.subsys.type == VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI &&
            hostdev->source.subsys.u.pci.backend == VIR_DOMAIN_HOSTDEV_PCI_BACKEND_DEFAULT)
            hostdev->source.subsys.u.pci.backend = VIR_DOMAIN_HOSTDEV_PCI_BACKEND_XEN;
    }

    return 0;
}

virDomainDefParserConfig libxlDomainDefParserConfig = {
    .macPrefix = { 0x00, 0x16, 0x3e },
    .devicesPostParseCallback = libxlDomainDeviceDefPostParse,
};

static const libxl_childproc_hooks libxl_child_hooks = {
#ifdef LIBXL_HAVE_SIGCHLD_OWNER_SELECTIVE_REAP
    .chldowner = libxl_sigchld_owner_libxl_always_selective_reap,
#else
    .chldowner = libxl_sigchld_owner_libxl,
#endif
};

int
libxlDomainObjPrivateInitCtx(virDomainObjPtr vm)
{
    libxlDomainObjPrivatePtr priv = vm->privateData;
    char *log_file;
    int ret = -1;

    if (priv->ctx)
        return 0;

    if (virAsprintf(&log_file, "%s/%s.log", LIBXL_LOG_DIR, vm->def->name) < 0)
        return -1;

    if ((priv->logger_file = fopen(log_file, "a")) == NULL)  {
        virReportSystemError(errno,
                             _("failed to open logfile %s"),
                             log_file);
        goto cleanup;
    }

    priv->logger =
        (xentoollog_logger *)xtl_createlogger_stdiostream(priv->logger_file,
                                                          XTL_DEBUG, 0);
    if (!priv->logger) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("cannot create libxenlight logger for domain %s"),
                       vm->def->name);
        goto cleanup;
    }

    if (libxl_ctx_alloc(&priv->ctx, LIBXL_VERSION, 0, priv->logger)) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Failed libxl context initialization"));
        goto cleanup;
    }

    libxl_osevent_register_hooks(priv->ctx, &libxl_event_callbacks, priv);
    libxl_childproc_setmode(priv->ctx, &libxl_child_hooks, priv);

    ret = 0;

cleanup:
    VIR_FREE(log_file);
    return ret;
}

void
libxlDomainEventQueue(libxlDriverPrivatePtr driver, virObjectEventPtr event)
{
    virObjectEventStateQueue(driver->domainEventState, event);
}

char *
libxlDomainManagedSavePath(libxlDriverPrivatePtr driver, virDomainObjPtr vm) {
    char *ret;
    libxlDriverConfigPtr cfg = libxlDriverConfigGet(driver);

    ignore_value(virAsprintf(&ret, "%s/%s.save", cfg->saveDir, vm->def->name));
    virObjectUnref(cfg);
    return ret;
}

/*
 * Open a saved image file and initialize domain definition from the header.
 *
 * Returns the opened fd on success, -1 on failure.
 */
int
libxlDomainSaveImageOpen(libxlDriverPrivatePtr driver,
                         libxlDriverConfigPtr cfg,
                         const char *from,
                         virDomainDefPtr *ret_def,
                         libxlSavefileHeaderPtr ret_hdr)
{
    int fd;
    virDomainDefPtr def = NULL;
    libxlSavefileHeader hdr;
    char *xml = NULL;

    if ((fd = virFileOpenAs(from, O_RDONLY, 0, -1, -1, 0)) < 0) {
        virReportSystemError(-fd,
                             _("Failed to open domain image file '%s'"), from);
        goto error;
    }

    if (saferead(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       "%s", _("failed to read libxl header"));
        goto error;
    }

    if (memcmp(hdr.magic, LIBXL_SAVE_MAGIC, sizeof(hdr.magic))) {
        virReportError(VIR_ERR_INVALID_ARG, "%s", _("image magic is incorrect"));
        goto error;
    }

    if (hdr.version > LIBXL_SAVE_VERSION) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("image version is not supported (%d > %d)"),
                       hdr.version, LIBXL_SAVE_VERSION);
        goto error;
    }

    if (hdr.xmlLen <= 0) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("invalid XML length: %d"), hdr.xmlLen);
        goto error;
    }

    if (VIR_ALLOC_N(xml, hdr.xmlLen) < 0)
        goto error;

    if (saferead(fd, xml, hdr.xmlLen) != hdr.xmlLen) {
        virReportError(VIR_ERR_OPERATION_FAILED, "%s", _("failed to read XML"));
        goto error;
    }

    if (!(def = virDomainDefParseString(xml, cfg->caps, driver->xmlopt,
                                        1 << VIR_DOMAIN_VIRT_XEN,
                                        VIR_DOMAIN_XML_INACTIVE)))
        goto error;

    VIR_FREE(xml);

    *ret_def = def;
    *ret_hdr = hdr;

    return fd;

error:
    VIR_FREE(xml);
    virDomainDefFree(def);
    VIR_FORCE_CLOSE(fd);
    return -1;
}

/*
 * Cleanup function for domain that has reached shutoff state.
 *
 * virDomainObjPtr must be locked on invocation
 */
void
libxlDomainCleanup(libxlDriverPrivatePtr driver,
                   virDomainObjPtr vm,
                   virDomainShutoffReason reason)
{
    libxlDomainObjPrivatePtr priv = vm->privateData;
    libxlDriverConfigPtr cfg = libxlDriverConfigGet(driver);
    int vnc_port;
    char *file;
    size_t i;
    virHostdevManagerPtr hostdev_mgr = driver->hostdevMgr;

    virHostdevReAttachDomainDevices(hostdev_mgr, LIBXL_DRIVER_NAME,
                                    vm->def, VIR_HOSTDEV_SP_PCI, NULL);

    vm->def->id = -1;

    if (priv->deathW) {
        libxl_evdisable_domain_death(priv->ctx, priv->deathW);
        priv->deathW = NULL;
    }

    if (vm->persistent)
        virDomainObjSetState(vm, VIR_DOMAIN_SHUTOFF, reason);

    if (virAtomicIntDecAndTest(&driver->nactive) && driver->inhibitCallback)
        driver->inhibitCallback(false, driver->inhibitOpaque);

    if ((vm->def->ngraphics == 1) &&
        vm->def->graphics[0]->type == VIR_DOMAIN_GRAPHICS_TYPE_VNC &&
        vm->def->graphics[0]->data.vnc.autoport) {
        vnc_port = vm->def->graphics[0]->data.vnc.port;
        if (vnc_port >= LIBXL_VNC_PORT_MIN) {
            if (virPortAllocatorRelease(driver->reservedVNCPorts,
                                        vnc_port) < 0)
                VIR_DEBUG("Could not mark port %d as unused", vnc_port);
        }
    }

    /* Remove any cputune settings */
    if (vm->def->cputune.nvcpupin) {
        for (i = 0; i < vm->def->cputune.nvcpupin; ++i) {
            virBitmapFree(vm->def->cputune.vcpupin[i]->cpumask);
            VIR_FREE(vm->def->cputune.vcpupin[i]);
        }
        VIR_FREE(vm->def->cputune.vcpupin);
        vm->def->cputune.nvcpupin = 0;
    }

    if (virAsprintf(&file, "%s/%s.xml", cfg->stateDir, vm->def->name) > 0) {
        if (unlink(file) < 0 && errno != ENOENT && errno != ENOTDIR)
            VIR_DEBUG("Failed to remove domain XML for %s", vm->def->name);
        VIR_FREE(file);
    }

    if (vm->newDef) {
        virDomainDefFree(vm->def);
        vm->def = vm->newDef;
        vm->def->id = -1;
        vm->newDef = NULL;
    }

    virObjectUnref(cfg);
}

/*
 * Cleanup function for domain that has reached shutoff state.
 * Executed in the context of a job.
 *
 * virDomainObjPtr should be locked on invocation
 * Returns true if references remain on virDomainObjPtr, false otherwise.
 */
bool
libxlDomainCleanupJob(libxlDriverPrivatePtr driver,
                      virDomainObjPtr vm,
                      virDomainShutoffReason reason)
{
    if (libxlDomainObjBeginJob(driver, vm, LIBXL_JOB_DESTROY) < 0)
        return true;

    libxlDomainCleanup(driver, vm, reason);

    return libxlDomainObjEndJob(driver, vm);
}

/*
 * Register for domain events emitted by libxl.
 */
int
libxlDomainEventsRegister(virDomainObjPtr vm)
{
    libxlDomainObjPrivatePtr priv = vm->privateData;

    libxl_event_register_callbacks(priv->ctx, &ev_hooks, vm);

    /* Always enable domain death events */
    if (libxl_evenable_domain_death(priv->ctx, vm->def->id, 0, &priv->deathW))
        goto error;

    return 0;

error:
    if (priv->deathW) {
        libxl_evdisable_domain_death(priv->ctx, priv->deathW);
        priv->deathW = NULL;
    }
    return -1;
}

/*
 * Core dump domain to default dump path.
 *
 * virDomainObjPtr must be locked on invocation
 */
int
libxlDomainAutoCoreDump(libxlDriverPrivatePtr driver,
                        virDomainObjPtr vm)
{
    libxlDomainObjPrivatePtr priv = vm->privateData;
    libxlDriverConfigPtr cfg = libxlDriverConfigGet(driver);
    time_t curtime = time(NULL);
    char timestr[100];
    struct tm time_info;
    char *dumpfile = NULL;
    int ret = -1;

    localtime_r(&curtime, &time_info);
    strftime(timestr, sizeof(timestr), "%Y-%m-%d-%H:%M:%S", &time_info);

    if (virAsprintf(&dumpfile, "%s/%s-%s",
                    cfg->autoDumpDir,
                    vm->def->name,
                    timestr) < 0)
        goto cleanup;

    if (libxlDomainObjBeginJob(driver, vm, LIBXL_JOB_MODIFY) < 0)
        goto cleanup;

    /* Unlock virDomainObj while dumping core */
    virObjectUnlock(vm);
    libxl_domain_core_dump(priv->ctx, vm->def->id, dumpfile, NULL);
    virObjectLock(vm);

    ignore_value(libxlDomainObjEndJob(driver, vm));
    ret = 0;

cleanup:
    VIR_FREE(dumpfile);
    virObjectUnref(cfg);

    return ret;
}
