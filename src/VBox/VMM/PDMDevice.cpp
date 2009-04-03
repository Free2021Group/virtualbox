/* $Id: PDMDevice.cpp $ */
/** @file
 * PDM - Pluggable Device and Driver Manager, Device parts.
 */

/*
 * Copyright (C) 2006-2007 Sun Microsystems, Inc.
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa
 * Clara, CA 95054 USA or visit http://www.sun.com if you need
 * additional information or have any questions.
 */


/*******************************************************************************
*   Header Files                                                               *
*******************************************************************************/
#define LOG_GROUP LOG_GROUP_PDM_DEVICE
#include "PDMInternal.h"
#include <VBox/pdm.h>
#include <VBox/mm.h>
#include <VBox/pgm.h>
#include <VBox/iom.h>
#include <VBox/cfgm.h>
#include <VBox/rem.h>
#include <VBox/dbgf.h>
#include <VBox/vm.h>
#include <VBox/vmm.h>

#include <VBox/version.h>
#include <VBox/log.h>
#include <VBox/err.h>
#include <iprt/alloc.h>
#include <iprt/alloca.h>
#include <iprt/asm.h>
#include <iprt/assert.h>
#include <iprt/path.h>
#include <iprt/semaphore.h>
#include <iprt/string.h>
#include <iprt/thread.h>


/*******************************************************************************
*   Structures and Typedefs                                                    *
*******************************************************************************/
/**
 * Internal callback structure pointer.
 * The main purpose is to define the extra data we associate
 * with PDMDEVREGCB so we can find the VM instance and so on.
 */
typedef struct PDMDEVREGCBINT
{
    /** The callback structure. */
    PDMDEVREGCB     Core;
    /** A bit of padding. */
    uint32_t        u32[4];
    /** VM Handle. */
    PVM             pVM;
} PDMDEVREGCBINT;
/** Pointer to a PDMDEVREGCBINT structure. */
typedef PDMDEVREGCBINT *PPDMDEVREGCBINT;
/** Pointer to a const PDMDEVREGCBINT structure. */
typedef const PDMDEVREGCBINT *PCPDMDEVREGCBINT;


/*******************************************************************************
*   Internal Functions                                                         *
*******************************************************************************/
static DECLCALLBACK(int)    pdmR3DevReg_Register(PPDMDEVREGCB pCallbacks, PCPDMDEVREG pDevReg);
static DECLCALLBACK(void *) pdmR3DevReg_MMHeapAlloc(PPDMDEVREGCB pCallbacks, size_t cb);
static int                  pdmR3DevLoadModules(PVM pVM);
static int                  pdmR3DevLoad(PVM pVM, PPDMDEVREGCBINT pRegCB, const char *pszFilename, const char *pszName);




/**
 * This function will initialize the devices for this VM instance.
 *
 *
 * First of all this mean loading the builtin device and letting them
 * register themselves. Beyond that any additional device modules are
 * loaded and called for registration.
 *
 * Then the device configuration is enumerated, the instantiation order
 * is determined, and finally they are instantiated.
 *
 * After all devices have been successfully instantiated the primary
 * PCI Bus device is called to emulate the PCI BIOS, i.e. making the
 * resource assignments. If there is no PCI device, this step is of course
 * skipped.
 *
 * Finally the init completion routines of the instantiated devices
 * are called.
 *
 * @returns VBox status code.
 * @param   pVM     VM Handle.
 */
int pdmR3DevInit(PVM pVM)
{
    LogFlow(("pdmR3DevInit:\n"));

    AssertRelease(!(RT_OFFSETOF(PDMDEVINS, achInstanceData) & 15));
    AssertRelease(sizeof(pVM->pdm.s.pDevInstances->Internal.s) <= sizeof(pVM->pdm.s.pDevInstances->Internal.padding));

    /*
     * Load device modules.
     */
    int rc = pdmR3DevLoadModules(pVM);
    if (RT_FAILURE(rc))
        return rc;

#ifdef VBOX_WITH_USB
    /* ditto for USB Devices. */
    rc = pdmR3UsbLoadModules(pVM);
    if (RT_FAILURE(rc))
        return rc;
#endif

    /*
     * Get the RC & R0 devhlps and create the devhlp R3 task queue.
     */
    PCPDMDEVHLPRC pDevHlpRC;
    rc = PDMR3LdrGetSymbolRC(pVM, NULL, "g_pdmRCDevHlp", &pDevHlpRC);
    AssertReleaseRCReturn(rc, rc);

    PCPDMDEVHLPR0 pDevHlpR0;
    rc = PDMR3LdrGetSymbolR0(pVM, NULL, "g_pdmR0DevHlp", &pDevHlpR0);
    AssertReleaseRCReturn(rc, rc);

    rc = PDMR3QueueCreateInternal(pVM, sizeof(PDMDEVHLPTASK), 8, 0, pdmR3DevHlpQueueConsumer, true, &pVM->pdm.s.pDevHlpQueueR3);
    AssertRCReturn(rc, rc);
    pVM->pdm.s.pDevHlpQueueR0 = PDMQueueR0Ptr(pVM->pdm.s.pDevHlpQueueR3);
    pVM->pdm.s.pDevHlpQueueRC = PDMQueueRCPtr(pVM->pdm.s.pDevHlpQueueR3);


    /*
     *
     * Enumerate the device instance configurations
     * and come up with a instantiation order.
     *
     */
    /* Switch to /Devices, which contains the device instantiations. */
    PCFGMNODE pDevicesNode = CFGMR3GetChild(CFGMR3GetRoot(pVM), "Devices");

    /*
     * Count the device instances.
     */
    PCFGMNODE pCur;
    PCFGMNODE pInstanceNode;
    unsigned cDevs = 0;
    for (pCur = CFGMR3GetFirstChild(pDevicesNode); pCur; pCur = CFGMR3GetNextChild(pCur))
        for (pInstanceNode = CFGMR3GetFirstChild(pCur); pInstanceNode; pInstanceNode = CFGMR3GetNextChild(pInstanceNode))
            cDevs++;
    if (!cDevs)
    {
        Log(("PDM: No devices were configured!\n"));
        return VINF_SUCCESS;
    }
    Log2(("PDM: cDevs=%d!\n", cDevs));

    /*
     * Collect info on each device instance.
     */
    struct DEVORDER
    {
        /** Configuration node. */
        PCFGMNODE   pNode;
        /** Pointer to device. */
        PPDMDEV     pDev;
        /** Init order. */
        uint32_t    u32Order;
        /** VBox instance number. */
        uint32_t    iInstance;
    } *paDevs = (struct DEVORDER *)alloca(sizeof(paDevs[0]) * (cDevs + 1)); /* (One extra for swapping) */
    Assert(paDevs);
    unsigned i = 0;
    for (pCur = CFGMR3GetFirstChild(pDevicesNode); pCur; pCur = CFGMR3GetNextChild(pCur))
    {
        /* Get the device name. */
        char szName[sizeof(paDevs[0].pDev->pDevReg->szDeviceName)];
        rc = CFGMR3GetName(pCur, szName, sizeof(szName));
        AssertMsgRCReturn(rc, ("Configuration error: device name is too long (or something)! rc=%Rrc\n", rc), rc);

        /* Find the device. */
        PPDMDEV pDev = pdmR3DevLookup(pVM, szName);
        AssertMsgReturn(pDev, ("Configuration error: device '%s' not found!\n", szName), VERR_PDM_DEVICE_NOT_FOUND);

        /* Configured priority or use default based on device class? */
        uint32_t u32Order;
        rc = CFGMR3QueryU32(pCur, "Priority", &u32Order);
        if (rc == VERR_CFGM_VALUE_NOT_FOUND)
        {
            uint32_t u32 = pDev->pDevReg->fClass;
            for (u32Order = 1; !(u32 & u32Order); u32Order <<= 1)
                /* nop */;
        }
        else
            AssertMsgRCReturn(rc, ("Configuration error: reading \"Priority\" for the '%s' device failed rc=%Rrc!\n", szName, rc), rc);

        /* Enumerate the device instances. */
        for (pInstanceNode = CFGMR3GetFirstChild(pCur); pInstanceNode; pInstanceNode = CFGMR3GetNextChild(pInstanceNode))
        {
            paDevs[i].pNode = pInstanceNode;
            paDevs[i].pDev = pDev;
            paDevs[i].u32Order = u32Order;

            /* Get the instance number. */
            char szInstance[32];
            rc = CFGMR3GetName(pInstanceNode, szInstance, sizeof(szInstance));
            AssertMsgRCReturn(rc, ("Configuration error: instance name is too long (or something)! rc=%Rrc\n", rc), rc);
            char *pszNext = NULL;
            rc = RTStrToUInt32Ex(szInstance, &pszNext, 0, &paDevs[i].iInstance);
            AssertMsgRCReturn(rc, ("Configuration error: RTStrToInt32Ex failed on the instance name '%s'! rc=%Rrc\n", szInstance, rc), rc);
            AssertMsgReturn(!*pszNext, ("Configuration error: the instance name '%s' isn't all digits. (%s)\n", szInstance, pszNext), VERR_INVALID_PARAMETER);

            /* next instance */
            i++;
        }
    } /* devices */
    Assert(i == cDevs);

    /*
     * Sort the device array ascending on u32Order. (bubble)
     */
    unsigned c = cDevs - 1;
    while (c)
    {
        unsigned j = 0;
        for (i = 0; i < c; i++)
            if (paDevs[i].u32Order > paDevs[i + 1].u32Order)
            {
                paDevs[cDevs] = paDevs[i + 1];
                paDevs[i + 1] = paDevs[i];
                paDevs[i] = paDevs[cDevs];
                j = i;
            }
        c = j;
    }


    /*
     *
     * Instantiate the devices.
     *
     */
    for (i = 0; i < cDevs; i++)
    {
        /*
         * Gather a bit of config.
         */
        /* trusted */
        bool fTrusted;
        rc = CFGMR3QueryBool(paDevs[i].pNode, "Trusted", &fTrusted);
        if (rc == VERR_CFGM_VALUE_NOT_FOUND)
            fTrusted = false;
        else if (RT_FAILURE(rc))
        {
            AssertMsgFailed(("configuration error: failed to query boolean \"Trusted\", rc=%Rrc\n", rc));
            return rc;
        }
        /* config node */
        PCFGMNODE pConfigNode = CFGMR3GetChild(paDevs[i].pNode, "Config");
        if (!pConfigNode)
        {
            rc = CFGMR3InsertNode(paDevs[i].pNode, "Config", &pConfigNode);
            if (RT_FAILURE(rc))
            {
                AssertMsgFailed(("Failed to create Config node! rc=%Rrc\n", rc));
                return rc;
            }
        }
        CFGMR3SetRestrictedRoot(pConfigNode);

        /*
         * Allocate the device instance.
         */
        size_t cb = RT_OFFSETOF(PDMDEVINS, achInstanceData[paDevs[i].pDev->pDevReg->cbInstance]);
        cb = RT_ALIGN_Z(cb, 16);
        PPDMDEVINS pDevIns;
        if (paDevs[i].pDev->pDevReg->fFlags & (PDM_DEVREG_FLAGS_RC | PDM_DEVREG_FLAGS_R0))
            rc = MMR3HyperAllocOnceNoRel(pVM, cb, 0, MM_TAG_PDM_DEVICE, (void **)&pDevIns);
        else
            rc = MMR3HeapAllocZEx(pVM, MM_TAG_PDM_DEVICE, cb, (void **)&pDevIns);
        if (RT_FAILURE(rc))
        {
            AssertMsgFailed(("Failed to allocate %d bytes of instance data for device '%s'. rc=%Rrc\n",
                             cb, paDevs[i].pDev->pDevReg->szDeviceName, rc));
            return rc;
        }

        /*
         * Initialize it.
         */
        pDevIns->u32Version                     = PDM_DEVINS_VERSION;
        //pDevIns->Internal.s.pNextR3             = NULL;
        //pDevIns->Internal.s.pPerDeviceNextR3    = NULL;
        pDevIns->Internal.s.pDevR3              = paDevs[i].pDev;
        pDevIns->Internal.s.pVMR3               = pVM;
        pDevIns->Internal.s.pVMR0               = pVM->pVMR0;
        pDevIns->Internal.s.pVMRC               = pVM->pVMRC;
        //pDevIns->Internal.s.pLunsR3             = NULL;
        pDevIns->Internal.s.pCfgHandle          = paDevs[i].pNode;
        //pDevIns->Internal.s.pPciDeviceR3        = NULL;
        //pDevIns->Internal.s.pPciBusR3           = NULL;
        //pDevIns->Internal.s.pPciDeviceR0        = 0;
        //pDevIns->Internal.s.pPciBusR0           = 0;
        //pDevIns->Internal.s.pPciDeviceRC        = 0;
        //pDevIns->Internal.s.pPciBusRC           = 0;
        pDevIns->pDevHlpR3                      = fTrusted ? &g_pdmR3DevHlpTrusted : &g_pdmR3DevHlpUnTrusted;
        pDevIns->pDevHlpRC                      = pDevHlpRC;
        pDevIns->pDevHlpR0                      = pDevHlpR0;
        pDevIns->pDevReg                        = paDevs[i].pDev->pDevReg;
        pDevIns->pCfgHandle                     = pConfigNode;
        pDevIns->iInstance                      = paDevs[i].iInstance;
        pDevIns->pvInstanceDataR3               = &pDevIns->achInstanceData[0];
        pDevIns->pvInstanceDataRC               = pDevIns->pDevReg->fFlags & PDM_DEVREG_FLAGS_RC
                                                ? MMHyperR3ToRC(pVM, pDevIns->pvInstanceDataR3) : NIL_RTRCPTR;
        pDevIns->pvInstanceDataR0               = pDevIns->pDevReg->fFlags & PDM_DEVREG_FLAGS_R0
                                                ? MMHyperR3ToR0(pVM, pDevIns->pvInstanceDataR3) : NIL_RTR0PTR;

        /*
         * Link it into all the lists.
         */
        /* The global instance FIFO. */
        PPDMDEVINS pPrev1 = pVM->pdm.s.pDevInstances;
        if (!pPrev1)
            pVM->pdm.s.pDevInstances = pDevIns;
        else
        {
            while (pPrev1->Internal.s.pNextR3)
                pPrev1 = pPrev1->Internal.s.pNextR3;
            pPrev1->Internal.s.pNextR3 = pDevIns;
        }

        /* The per device instance FIFO. */
        PPDMDEVINS pPrev2 = paDevs[i].pDev->pInstances;
        if (!pPrev2)
            paDevs[i].pDev->pInstances = pDevIns;
        else
        {
            while (pPrev2->Internal.s.pPerDeviceNextR3)
                pPrev2 = pPrev2->Internal.s.pPerDeviceNextR3;
            pPrev2->Internal.s.pPerDeviceNextR3 = pDevIns;
        }

        /*
         * Call the constructor.
         */
        Log(("PDM: Constructing device '%s' instance %d...\n", pDevIns->pDevReg->szDeviceName, pDevIns->iInstance));
        rc = pDevIns->pDevReg->pfnConstruct(pDevIns, pDevIns->iInstance, pDevIns->pCfgHandle);
        if (RT_FAILURE(rc))
        {
            LogRel(("PDM: Failed to construct '%s'/%d! %Rra\n", pDevIns->pDevReg->szDeviceName, pDevIns->iInstance, rc));
            /* because we're damn lazy right now, we'll say that the destructor will be called even if the constructor fails. */
            return rc;
        }
    } /* for device instances */

#ifdef VBOX_WITH_USB
    /* ditto for USB Devices. */
    rc = pdmR3UsbInstantiateDevices(pVM);
    if (RT_FAILURE(rc))
        return rc;
#endif


    /*
     *
     * PCI BIOS Fake and Init Complete.
     *
     */
    if (pVM->pdm.s.aPciBuses[0].pDevInsR3)
    {
        pdmLock(pVM);
        rc = pVM->pdm.s.aPciBuses[0].pfnFakePCIBIOSR3(pVM->pdm.s.aPciBuses[0].pDevInsR3);
        pdmUnlock(pVM);
        if (RT_FAILURE(rc))
        {
            AssertMsgFailed(("PCI BIOS fake failed rc=%Rrc\n", rc));
            return rc;
        }
    }

    for (PPDMDEVINS pDevIns = pVM->pdm.s.pDevInstances; pDevIns; pDevIns = pDevIns->Internal.s.pNextR3)
    {
        if (pDevIns->pDevReg->pfnInitComplete)
        {
            rc = pDevIns->pDevReg->pfnInitComplete(pDevIns);
            if (RT_FAILURE(rc))
            {
                AssertMsgFailed(("InitComplete on device '%s'/%d failed with rc=%Rrc\n",
                                 pDevIns->pDevReg->szDeviceName, pDevIns->iInstance, rc));
                return rc;
            }
        }
    }

#ifdef VBOX_WITH_USB
    /* ditto for USB Devices. */
    rc = pdmR3UsbVMInitComplete(pVM);
    if (RT_FAILURE(rc))
        return rc;
#endif

    LogFlow(("pdmR3DevInit: returns %Rrc\n", VINF_SUCCESS));
    return VINF_SUCCESS;
}


/**
 * Lookups a device structure by name.
 * @internal
 */
PPDMDEV pdmR3DevLookup(PVM pVM, const char *pszName)
{
    size_t cchName = strlen(pszName);
    for (PPDMDEV pDev = pVM->pdm.s.pDevs; pDev; pDev = pDev->pNext)
        if (    pDev->cchName == cchName
            &&  !strcmp(pDev->pDevReg->szDeviceName, pszName))
            return pDev;
    return NULL;
}


/**
 * Loads the device modules.
 *
 * @returns VBox status code.
 * @param   pVM     Pointer to the shared VM structure.
 */
static int pdmR3DevLoadModules(PVM pVM)
{
    /*
     * Initialize the callback structure.
     */
    PDMDEVREGCBINT RegCB;
    RegCB.Core.u32Version = PDM_DEVREG_CB_VERSION;
    RegCB.Core.pfnRegister = pdmR3DevReg_Register;
    RegCB.Core.pfnMMHeapAlloc = pdmR3DevReg_MMHeapAlloc;
    RegCB.pVM = pVM;

    /*
     * Load the builtin module
     */
    PCFGMNODE pDevicesNode = CFGMR3GetChild(CFGMR3GetRoot(pVM), "PDM/Devices");
    bool fLoadBuiltin;
    int rc = CFGMR3QueryBool(pDevicesNode, "LoadBuiltin", &fLoadBuiltin);
    if (rc == VERR_CFGM_VALUE_NOT_FOUND || rc == VERR_CFGM_NO_PARENT)
        fLoadBuiltin = true;
    else if (RT_FAILURE(rc))
    {
        AssertMsgFailed(("Configuration error: Querying boolean \"LoadBuiltin\" failed with %Rrc\n", rc));
        return rc;
    }
    if (fLoadBuiltin)
    {
        /* make filename */
        char *pszFilename = pdmR3FileR3("VBoxDD", /* fShared = */ true);
        if (!pszFilename)
            return VERR_NO_TMP_MEMORY;
        rc = pdmR3DevLoad(pVM, &RegCB, pszFilename, "VBoxDD");
        RTMemTmpFree(pszFilename);
        if (RT_FAILURE(rc))
            return rc;

        /* make filename */
        pszFilename = pdmR3FileR3("VBoxDD2", /* fShared = */ true);
        if (!pszFilename)
            return VERR_NO_TMP_MEMORY;
        rc = pdmR3DevLoad(pVM, &RegCB, pszFilename, "VBoxDD2");
        RTMemTmpFree(pszFilename);
        if (RT_FAILURE(rc))
            return rc;
    }

    /*
     * Load additional device modules.
     */
    PCFGMNODE pCur;
    for (pCur = CFGMR3GetFirstChild(pDevicesNode); pCur; pCur = CFGMR3GetNextChild(pCur))
    {
        /*
         * Get the name and path.
         */
        char szName[PDMMOD_NAME_LEN];
        rc = CFGMR3GetName(pCur, &szName[0], sizeof(szName));
        if (rc == VERR_CFGM_NOT_ENOUGH_SPACE)
        {
            AssertMsgFailed(("configuration error: The module name is too long, cchName=%zu.\n", CFGMR3GetNameLen(pCur)));
            return VERR_PDM_MODULE_NAME_TOO_LONG;
        }
        else if (RT_FAILURE(rc))
        {
            AssertMsgFailed(("CFGMR3GetName -> %Rrc.\n", rc));
            return rc;
        }

        /* the path is optional, if no path the module name + path is used. */
        char szFilename[RTPATH_MAX];
        rc = CFGMR3QueryString(pCur, "Path", &szFilename[0], sizeof(szFilename));
        if (rc == VERR_CFGM_VALUE_NOT_FOUND)
            strcpy(szFilename, szName);
        else if (RT_FAILURE(rc))
        {
            AssertMsgFailed(("configuration error: Failure to query the module path, rc=%Rrc.\n", rc));
            return rc;
        }

        /* prepend path? */
        if (!RTPathHavePath(szFilename))
        {
            char *psz = pdmR3FileR3(szFilename);
            if (!psz)
                return VERR_NO_TMP_MEMORY;
            size_t cch = strlen(psz) + 1;
            if (cch > sizeof(szFilename))
            {
                RTMemTmpFree(psz);
                AssertMsgFailed(("Filename too long! cch=%d '%s'\n", cch, psz));
                return VERR_FILENAME_TOO_LONG;
            }
            memcpy(szFilename, psz, cch);
            RTMemTmpFree(psz);
        }

        /*
         * Load the module and register it's devices.
         */
        rc = pdmR3DevLoad(pVM, &RegCB, szFilename, szName);
        if (RT_FAILURE(rc))
            return rc;
    }

    return VINF_SUCCESS;
}


/**
 * Loads one device module and call the registration entry point.
 *
 * @returns VBox status code.
 * @param   pVM             VM handle.
 * @param   pRegCB          The registration callback stuff.
 * @param   pszFilename     Module filename.
 * @param   pszName         Module name.
 */
static int pdmR3DevLoad(PVM pVM, PPDMDEVREGCBINT pRegCB, const char *pszFilename, const char *pszName)
{
    /*
     * Load it.
     */
    int rc = pdmR3LoadR3U(pVM->pUVM, pszFilename, pszName);
    if (RT_SUCCESS(rc))
    {
        /*
         * Get the registration export and call it.
         */
        FNPDMVBOXDEVICESREGISTER *pfnVBoxDevicesRegister;
        rc = PDMR3LdrGetSymbolR3(pVM, pszName, "VBoxDevicesRegister", (void **)&pfnVBoxDevicesRegister);
        if (RT_SUCCESS(rc))
        {
            Log(("PDM: Calling VBoxDevicesRegister (%p) of %s (%s)\n", pfnVBoxDevicesRegister, pszName, pszFilename));
            rc = pfnVBoxDevicesRegister(&pRegCB->Core, VBOX_VERSION);
            if (RT_SUCCESS(rc))
                Log(("PDM: Successfully loaded device module %s (%s).\n", pszName, pszFilename));
            else
                AssertMsgFailed(("VBoxDevicesRegister failed with rc=%Rrc for module %s (%s)\n", rc, pszName, pszFilename));
        }
        else
        {
            AssertMsgFailed(("Failed to locate 'VBoxDevicesRegister' in %s (%s) rc=%Rrc\n", pszName, pszFilename, rc));
            if (rc == VERR_SYMBOL_NOT_FOUND)
                rc = VERR_PDM_NO_REGISTRATION_EXPORT;
        }
    }
    else
        AssertMsgFailed(("Failed to load %s %s!\n", pszFilename, pszName));
    return rc;
}


/**
 * Registers a device with the current VM instance.
 *
 * @returns VBox status code.
 * @param   pCallbacks      Pointer to the callback table.
 * @param   pDevReg         Pointer to the device registration record.
 *                          This data must be permanent and readonly.
 */
static DECLCALLBACK(int) pdmR3DevReg_Register(PPDMDEVREGCB pCallbacks, PCPDMDEVREG pDevReg)
{
    /*
     * Validate the registration structure.
     */
    Assert(pDevReg);
    if (pDevReg->u32Version != PDM_DEVREG_VERSION)
    {
        AssertMsgFailed(("Unknown struct version %#x!\n", pDevReg->u32Version));
        return VERR_PDM_UNKNOWN_DEVREG_VERSION;
    }
    if (    !pDevReg->szDeviceName[0]
        ||  strlen(pDevReg->szDeviceName) >= sizeof(pDevReg->szDeviceName))
    {
        AssertMsgFailed(("Invalid name '%s'\n", pDevReg->szDeviceName));
        return VERR_PDM_INVALID_DEVICE_REGISTRATION;
    }
    if (    (pDevReg->fFlags & PDM_DEVREG_FLAGS_RC)
        &&  (   !pDevReg->szRCMod[0]
             || strlen(pDevReg->szRCMod) >= sizeof(pDevReg->szRCMod)))
    {
        AssertMsgFailed(("Invalid GC module name '%s' - (Device %s)\n", pDevReg->szRCMod, pDevReg->szDeviceName));
        return VERR_PDM_INVALID_DEVICE_REGISTRATION;
    }
    if (    (pDevReg->fFlags & PDM_DEVREG_FLAGS_R0)
        &&  (   !pDevReg->szR0Mod[0]
             || strlen(pDevReg->szR0Mod) >= sizeof(pDevReg->szR0Mod)))
    {
        AssertMsgFailed(("Invalid R0 module name '%s' - (Device %s)\n", pDevReg->szR0Mod, pDevReg->szDeviceName));
        return VERR_PDM_INVALID_DEVICE_REGISTRATION;
    }
    if ((pDevReg->fFlags & PDM_DEVREG_FLAGS_HOST_BITS_MASK) != PDM_DEVREG_FLAGS_HOST_BITS_DEFAULT)
    {
        AssertMsgFailed(("Invalid host bits flags! fFlags=%#x (Device %s)\n", pDevReg->fFlags, pDevReg->szDeviceName));
        return VERR_PDM_INVALID_DEVICE_HOST_BITS;
    }
    if (!(pDevReg->fFlags & PDM_DEVREG_FLAGS_GUEST_BITS_MASK))
    {
        AssertMsgFailed(("Invalid guest bits flags! fFlags=%#x (Device %s)\n", pDevReg->fFlags, pDevReg->szDeviceName));
        return VERR_PDM_INVALID_DEVICE_REGISTRATION;
    }
    if (!pDevReg->fClass)
    {
        AssertMsgFailed(("No class! (Device %s)\n", pDevReg->szDeviceName));
        return VERR_PDM_INVALID_DEVICE_REGISTRATION;
    }
    if (pDevReg->cMaxInstances <= 0)
    {
        AssertMsgFailed(("Max instances %u! (Device %s)\n", pDevReg->cMaxInstances, pDevReg->szDeviceName));
        return VERR_PDM_INVALID_DEVICE_REGISTRATION;
    }
    if (pDevReg->cbInstance > (RTUINT)(pDevReg->fFlags & (PDM_DEVREG_FLAGS_RC | PDM_DEVREG_FLAGS_R0)  ? 96 * _1K : _1M))
    {
        AssertMsgFailed(("Instance size %d bytes! (Device %s)\n", pDevReg->cbInstance, pDevReg->szDeviceName));
        return VERR_PDM_INVALID_DEVICE_REGISTRATION;
    }
    if (!pDevReg->pfnConstruct)
    {
        AssertMsgFailed(("No constructore! (Device %s)\n", pDevReg->szDeviceName));
        return VERR_PDM_INVALID_DEVICE_REGISTRATION;
    }
    /* Check matching guest bits last without any asserting. Enables trial and error registration. */
    if (!(pDevReg->fFlags & PDM_DEVREG_FLAGS_GUEST_BITS_DEFAULT))
    {
        Log(("PDM: Rejected device '%s' because it didn't match the guest bits.\n", pDevReg->szDeviceName));
        return VERR_PDM_INVALID_DEVICE_GUEST_BITS;
    }
    AssertLogRelMsg(pDevReg->u32VersionEnd == PDM_DEVREG_VERSION,
                    ("u32VersionEnd=%#x, expected %#x. (szDeviceName=%s)\n",
                     pDevReg->u32VersionEnd, PDM_DEVREG_VERSION, pDevReg->szDeviceName));

    /*
     * Check for duplicate and find FIFO entry at the same time.
     */
    PCPDMDEVREGCBINT pRegCB = (PCPDMDEVREGCBINT)pCallbacks;
    PPDMDEV pDevPrev = NULL;
    PPDMDEV pDev = pRegCB->pVM->pdm.s.pDevs;
    for (; pDev; pDevPrev = pDev, pDev = pDev->pNext)
    {
        if (!strcmp(pDev->pDevReg->szDeviceName, pDevReg->szDeviceName))
        {
            AssertMsgFailed(("Device '%s' already exists\n", pDevReg->szDeviceName));
            return VERR_PDM_DEVICE_NAME_CLASH;
        }
    }

    /*
     * Allocate new device structure and insert it into the list.
     */
    pDev = (PPDMDEV)MMR3HeapAlloc(pRegCB->pVM, MM_TAG_PDM_DEVICE, sizeof(*pDev));
    if (pDev)
    {
        pDev->pNext = NULL;
        pDev->cInstances = 0;
        pDev->pInstances = NULL;
        pDev->pDevReg = pDevReg;
        pDev->cchName = (uint32_t)strlen(pDevReg->szDeviceName);

        if (pDevPrev)
            pDevPrev->pNext = pDev;
        else
            pRegCB->pVM->pdm.s.pDevs = pDev;
        Log(("PDM: Registered device '%s'\n", pDevReg->szDeviceName));
        return VINF_SUCCESS;
    }
    return VERR_NO_MEMORY;
}


/**
 * Allocate memory which is associated with current VM instance
 * and automatically freed on it's destruction.
 *
 * @returns Pointer to allocated memory. The memory is *NOT* zero-ed.
 * @param   pCallbacks      Pointer to the callback table.
 * @param   cb              Number of bytes to allocate.
 */
static DECLCALLBACK(void *) pdmR3DevReg_MMHeapAlloc(PPDMDEVREGCB pCallbacks, size_t cb)
{
    Assert(pCallbacks);
    Assert(pCallbacks->u32Version == PDM_DEVREG_CB_VERSION);

    void *pv = MMR3HeapAlloc(((PPDMDEVREGCBINT)pCallbacks)->pVM, MM_TAG_PDM_DEVICE_USER, cb);
    LogFlow(("pdmR3DevReg_MMHeapAlloc(,%#zx): returns %p\n", cb, pv));
    return pv;
}


/**
 * Locates a LUN.
 *
 * @returns VBox status code.
 * @param   pVM             VM Handle.
 * @param   pszDevice       Device name.
 * @param   iInstance       Device instance.
 * @param   iLun            The Logical Unit to obtain the interface of.
 * @param   ppLun           Where to store the pointer to the LUN if found.
 * @thread  Try only do this in EMT...
 */
int pdmR3DevFindLun(PVM pVM, const char *pszDevice, unsigned iInstance, unsigned iLun, PPDMLUN *ppLun)
{
    /*
     * Iterate registered devices looking for the device.
     */
    size_t cchDevice = strlen(pszDevice);
    for (PPDMDEV pDev = pVM->pdm.s.pDevs; pDev; pDev = pDev->pNext)
    {
        if (    pDev->cchName == cchDevice
            &&  !memcmp(pDev->pDevReg->szDeviceName, pszDevice, cchDevice))
        {
            /*
             * Iterate device instances.
             */
            for (PPDMDEVINS pDevIns = pDev->pInstances; pDevIns; pDevIns = pDevIns->Internal.s.pPerDeviceNextR3)
            {
                if (pDevIns->iInstance == iInstance)
                {
                    /*
                     * Iterate luns.
                     */
                    for (PPDMLUN pLun = pDevIns->Internal.s.pLunsR3; pLun; pLun = pLun->pNext)
                    {
                        if (pLun->iLun == iLun)
                        {
                            *ppLun = pLun;
                            return VINF_SUCCESS;
                        }
                    }
                    return VERR_PDM_LUN_NOT_FOUND;
                }
            }
            return VERR_PDM_DEVICE_INSTANCE_NOT_FOUND;
        }
    }
    return VERR_PDM_DEVICE_NOT_FOUND;
}


/**
 * Attaches a preconfigured driver to an existing device instance.
 *
 * This is used to change drivers and suchlike at runtime.
 *
 * @returns VBox status code.
 * @param   pVM             VM Handle.
 * @param   pszDevice       Device name.
 * @param   iInstance       Device instance.
 * @param   iLun            The Logical Unit to obtain the interface of.
 * @param   ppBase          Where to store the base interface pointer. Optional.
 * @thread  EMT
 */
VMMR3DECL(int) PDMR3DeviceAttach(PVM pVM, const char *pszDevice, unsigned iInstance, unsigned iLun, PPDMIBASE *ppBase)
{
    VM_ASSERT_EMT(pVM);
    LogFlow(("PDMR3DeviceAttach: pszDevice=%p:{%s} iInstance=%d iLun=%d ppBase=%p\n",
             pszDevice, pszDevice, iInstance, iLun, ppBase));

    /*
     * Find the LUN in question.
     */
    PPDMLUN pLun;
    int rc = pdmR3DevFindLun(pVM, pszDevice, iInstance, iLun, &pLun);
    if (RT_SUCCESS(rc))
    {
        /*
         * Can we attach anything at runtime?
         */
        PPDMDEVINS pDevIns = pLun->pDevIns;
        if (pDevIns->pDevReg->pfnAttach)
        {
            if (!pLun->pTop)
            {
                rc = pDevIns->pDevReg->pfnAttach(pDevIns, iLun);

            }
            else
                rc = VERR_PDM_DRIVER_ALREADY_ATTACHED;
        }
        else
            rc = VERR_PDM_DEVICE_NO_RT_ATTACH;

        if (ppBase)
            *ppBase = pLun->pTop ? &pLun->pTop->IBase : NULL;
    }
    else if (ppBase)
        *ppBase = NULL;

    if (ppBase)
        LogFlow(("PDMR3DeviceAttach: returns %Rrc *ppBase=%p\n", rc, *ppBase));
    else
        LogFlow(("PDMR3DeviceAttach: returns %Rrc\n", rc));
    return rc;
}


/**
 * Detaches a driver chain from an existing device instance.
 *
 * This is used to change drivers and suchlike at runtime.
 *
 * @returns VBox status code.
 * @param   pVM             VM Handle.
 * @param   pszDevice       Device name.
 * @param   iInstance       Device instance.
 * @param   iLun            The Logical Unit to obtain the interface of.
 * @thread  EMT
 */
VMMR3DECL(int) PDMR3DeviceDetach(PVM pVM, const char *pszDevice, unsigned iInstance, unsigned iLun)
{
    VM_ASSERT_EMT(pVM);
    LogFlow(("PDMR3DeviceDetach: pszDevice=%p:{%s} iInstance=%d iLun=%d\n",
             pszDevice, pszDevice, iInstance, iLun));

    /*
     * Find the LUN in question.
     */
    PPDMLUN pLun;
    int rc = pdmR3DevFindLun(pVM, pszDevice, iInstance, iLun, &pLun);
    if (RT_SUCCESS(rc))
    {
        /*
         * Can we detach anything at runtime?
         */
        PPDMDEVINS pDevIns = pLun->pDevIns;
        if (pDevIns->pDevReg->pfnDetach)
        {
            if (pLun->pTop)
                rc = pdmR3DrvDetach(pLun->pTop);
            else
                rc = VINF_PDM_NO_DRIVER_ATTACHED_TO_LUN;
        }
        else
            rc = VERR_PDM_DEVICE_NO_RT_DETACH;
    }

    LogFlow(("PDMR3DeviceDetach: returns %Rrc\n", rc));
    return rc;
}
