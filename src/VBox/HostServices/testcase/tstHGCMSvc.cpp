/* $Id: tstHGCMSvc.cpp 22653 2009-09-01 12:02:32Z vboxsync $ */
/** @file
 * HGCM Service Testcase.
 */

/*
 * Copyright (C) 2009 Sun Microsystems, Inc.
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

/*****************************************************************************
*   Header Files                                                             *
*****************************************************************************/
#include <VBox/hgcmsvc.h>
#include <iprt/initterm.h>
#include <iprt/test.h>

int main()
{
    /*
     * Init the runtime, test and say hello.
     */
    RTTEST hTest;
    int rc = RTTestInitAndCreate("tstHGCMSvc", &hTest);
    if (rc)
        return rc;
    RTTestBanner(hTest);

    /*
     * Run the test.
     */
    VBOXHGCMSVCPARM parm;
    parm.testGetString(hTest);

    /*
     * Summary
     */
    return RTTestSummaryAndDestroy(hTest);
}

