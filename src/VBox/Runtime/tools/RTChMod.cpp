 * Copyright (C) 2013-2019 Oracle Corporation
#include <iprt/buildconfig.h>
#include <iprt/errcore.h>
#include <iprt/file.h>
#include <iprt/getopt.h>
#include <iprt/path.h>
#include <iprt/string.h>
#include <iprt/vfs.h>

        RTVFSOBJ        hVfsObj;
        rc = RTVfsChainOpenObj(pszPath, RTFILE_O_ACCESS_ATTR_READWRITE | RTFILE_O_DENY_NONE | RTFILE_O_OPEN,
                               RTVFSOBJ_F_OPEN_ANY | RTVFSOBJ_F_CREATE_NOTHING | RTPATH_F_FOLLOW_LINK,
                               &hVfsObj, &offError, RTErrInfoInitStatic(&ErrInfo));
            rc = RTVfsObjQueryInfo(hVfsObj, &ObjInfo, RTFSOBJATTRADD_NOTHING);
                    rc = RTVfsObjSetMode(hVfsObj, fNewMode, RTCHMOD_SET_ALL_MASK);
                        RTMsgError("RTVfsObjSetMode failed on '%s' with fNewMode=%#x: %Rrc", pszPath, fNewMode, rc);
                RTVfsChainMsgError("RTVfsObjQueryInfo", pszPath, rc, offError, &ErrInfo.Core);
            RTVfsObjRelease(hVfsObj);
        else
            RTVfsChainMsgError("RTVfsChainOpenObject", pszPath, rc, offError, &ErrInfo.Core);
    {
        /*
         * Don't bother redoing the above work if its not necessary.
         */
        RTFMODE fNewMode = rtCmdMkModCalcNewMode(pOpts, ObjInfo.Attr.fMode);
        if (fNewMode != ObjInfo.Attr.fMode)
            return rtCmdChModOne(pOpts, pszPath);
        if (pOpts->enmNoiseLevel >= kRTCmdChModNoise_Verbose)
            RTPrintf("%s\n", pszPath);
        return RTEXITCODE_SUCCESS;
    }