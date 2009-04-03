/* $Id: kLdrModMachO.c 2 2007-11-16 16:07:14Z bird $ */
/** @file
 * kLdr - The Module Interpreter for the MACH-O format.
 */

/*
 * Copyright (c) 2006-2007 knut st. osmundsen <bird-kStuff-spam@anduin.net>
 *
 * This file is part of kStuff.
 *
 * kStuff is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * In addition to the permissions in the GNU Lesser General Public
 * License, you are granted unlimited permission to link the compiled
 * version of this file into combinations with other programs, and to
 * distribute those combinations without any restriction coming from
 * the use of this file.
 *
 * kStuff is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with kStuff; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA
 */

/*******************************************************************************
*   Header Files                                                               *
*******************************************************************************/
#include <k/kLdr.h>
#include "kLdrInternal.h"
#include <k/kLdrFmts/mach-o.h>


/*******************************************************************************
*   Defined Constants And Macros                                               *
*******************************************************************************/
/** @def KLDRMODMACHO_STRICT
 * Define KLDRMODMACHO_STRICT to enabled strict checks in KLDRMODMACHO. */
#define KLDRMODMACHO_STRICT 1

/** @def KLDRMODMACHO_ASSERT
 * Assert that an expression is true when KLDR_STRICT is defined.
 */
#ifdef KLDRMODMACHO_STRICT
# define KLDRMODMACHO_ASSERT(expr)  kHlpAssert(expr)
#else
# define KLDRMODMACHO_ASSERT(expr)  do {} while (0)
#endif


/*******************************************************************************
*   Structures and Typedefs                                                    *
*******************************************************************************/
/**
 * Mach-O section details.
 */
typedef struct KLDRMODMACHOSECT
{
    /** The size of the section (in bytes). */
    KLDRSIZE                cb;
    /** The link address of this section. */
    KLDRADDR                LinkAddress;
    /** The RVA of this section. */
    KLDRADDR                RVA;
    /** The file offset of this section.
     * This is -1 if the section doesn't have a file backing. */
    KLDRFOFF                offFile;
    /** The number of fixups. */
    KU32                    cFixups;
    /** The array of fixups. (lazy loaded) */
    macho_relocation_info_t *paFixups;
    /** The file offset of the fixups for this section.
     * This is -1 if the section doesn't have any fixups. */
    KLDRFOFF                offFixups;
    /** Mach-O section flags. */
    KU32                    fFlags;
    /** kLdr segment index. */
    KU32                    iSegment;
    /** Pointer to the Mach-O section structure. */
    void                   *pvMachoSection;
} KLDRMODMACHOSECT, *PKLDRMODMACHOSECT;

/**
 * Extra per-segment info.
 *
 * This is corresponds to a kLdr segment, not a Mach-O segment!
 */
typedef struct KLDRMODMACHOSEG
{
    /** The number of sections in the segment. */
    KU32                    cSections;
    /** Pointer to the sections belonging to this segment.
     * The array resides in the big memory chunk allocated for
     * the module handle, so it doesn't need freeing. */
    PKLDRMODMACHOSECT       paSections;

} KLDRMODMACHOSEG, *PKLDRMODMACHOSEG;

/**
 * Instance data for the Mach-O MH_OBJECT module interpreter.
 * @todo interpret the other MH_* formats.
 */
typedef struct KLDRMODMACHO
{
    /** Pointer to the module. (Follows the section table.) */
    PKLDRMOD                pMod;
    /** Pointer to the RDR file mapping of the raw file bits. NULL if not mapped. */
    const void             *pvBits;
    /** Pointer to the user mapping. */
    void                   *pvMapping;

    /** The link address. */
    KLDRADDR                LinkAddress;
    /** The size of the mapped image. */
    KLDRADDR                cbImage;
    /** When set the sections in the load command segments must be used when
     * mapping or loading the image. */
    int                     fMapUsingLoadCommandSections;

    /** Pointer to the load commands. (endian converted) */
    KU8                    *pbLoadCommands;
    /** The Mach-O header. (endian converted)
     * @remark The reserved field is only valid for real 64-bit headers. */
    mach_header_64_t        Hdr;

    /** The offset of the symbol table. */
    KLDRFOFF                offSymbols;
    /** The number of symbols. */
    KU32                    cSymbols;
    /** The pointer to the loaded symbol table. */
    void                   *pvaSymbols;
    /** The offset of the string table. */
    KLDRFOFF                offStrings;
    /** The size of the of the string table. */
    KU32                    cchStrings;
    /** Pointer to the loaded string table. */
    char                   *pchStrings;

    /** The number of sections. */
    KU32                    cSections;
    /** Pointer to the section array running in parallel to the Mach-O one. */
    PKLDRMODMACHOSECT       paSections;

    /** Array of segments parallel to the one in KLDRMOD. */
    KLDRMODMACHOSEG         aSegments[1];
} KLDRMODMACHO, *PKLDRMODMACHO;



/*******************************************************************************
*   Internal Functions                                                         *
*******************************************************************************/
#if 0
static KI32 kldrModMachONumberOfImports(PKLDRMOD pMod, const void *pvBits);
#endif
static int kldrModMachORelocateBits(PKLDRMOD pMod, void *pvBits, KLDRADDR NewBaseAddress, KLDRADDR OldBaseAddress,
                                    PFNKLDRMODGETIMPORT pfnGetImport, void *pvUser);

static int  kldrModMachODoCreate(PKRDR pRdr, PKLDRMODMACHO *ppMod);
static int  kldrModMachOPreParseLoadCommands(KU8 *pbLoadCommands, const mach_header_32_t *pHdr, PKRDR pRdr,
                                             KU32 *pcSegments, KU32 *pcSections, KU32 *pcbStringPool);
static int  kldrModMachOParseLoadCommands(PKLDRMODMACHO pModMachO, char *pbStringPool, KU32 cbStringPool);
static int  kldrModMachOAdjustBaseAddress(PKLDRMODMACHO pModMachO, PKLDRADDR pBaseAddress);

/*static int  kldrModMachOLoadLoadCommands(PKLDRMODMACHO pModMachO);*/
static int  kldrModMachOLoadObjSymTab(PKLDRMODMACHO pModMachO);
static int  kldrModMachOLoadFixups(PKLDRMODMACHO pModMachO, KLDRFOFF offFixups, KU32 cFixups, macho_relocation_info_t **ppaFixups);
static int  kldrModMachOMapVirginBits(PKLDRMODMACHO pModMachO);

static int  kldrModMachODoQuerySymbol32Bit(PKLDRMODMACHO pModMachO, const macho_nlist_32_t *paSyms, KU32 cSyms, const char *pchStrings,
                                           KU32 cchStrings, KLDRADDR BaseAddress, KU32 iSymbol, const char *pchSymbol,
                                           KSIZE cchSymbol, PKLDRADDR puValue, KU32 *pfKind);
static int  kldrModMachODoEnumSymbols32Bit(PKLDRMODMACHO pModMachO, const macho_nlist_32_t *paSyms, KU32 cSyms,
                                           const char *pchStrings, KU32 cchStrings, KLDRADDR BaseAddress,
                                           KU32 fFlags, PFNKLDRMODENUMSYMS pfnCallback, void *pvUser);
static int  kldrModMachOObjDoImports(PKLDRMODMACHO pModMachO, PFNKLDRMODGETIMPORT pfnGetImport, void *pvUser);
static int  kldrModMachOObjDoFixups(PKLDRMODMACHO pModMachO, void *pvMapping, KLDRADDR NewBaseAddress);
static int  kldrModMachOFixupSectionGeneric32Bit(PKLDRMODMACHO pModMachO, KU8 *pbSectBits, PKLDRMODMACHOSECT pFixupSect,
                                                 macho_nlist_32_t *paSyms, KU32 cSyms, KLDRADDR NewBaseAddress);

/*static int  kldrModMachODoFixups(PKLDRMODMACHO pModMachO, void *pvMapping, KLDRADDR NewBaseAddress, KLDRADDR OldBaseAddress);
static int  kldrModMachODoImports(PKLDRMODMACHO pModMachO, void *pvMapping, PFNKLDRMODGETIMPORT pfnGetImport, void *pvUser);*/


/**
 * Create a loader module instance interpreting the executable image found
 * in the specified file provider instance.
 *
 * @returns 0 on success and *ppMod pointing to a module instance.
 *          On failure, a non-zero OS specific error code is returned.
 * @param   pOps            Pointer to the registered method table.
 * @param   pRdr            The file provider instance to use.
 * @param   offNewHdr       The offset of the new header in MZ files. -1 if not found.
 * @param   ppMod           Where to store the module instance pointer.
 */
static int kldrModMachOCreate(PCKLDRMODOPS pOps, PKRDR pRdr, KLDRFOFF offNewHdr, PPKLDRMOD ppMod)
{
    PKLDRMODMACHO pModMachO;
    int rc;

    /*
     * Create the instance data and do a minimal header validation.
     */
    rc = kldrModMachODoCreate(pRdr, &pModMachO);
    if (!rc)
    {
        pModMachO->pMod->pOps = pOps;
        pModMachO->pMod->u32Magic = KLDRMOD_MAGIC;
        *ppMod = pModMachO->pMod;
        return 0;
    }
    if (pModMachO)
    {
        kHlpFree(pModMachO->pbLoadCommands);
        kHlpFree(pModMachO);
    }
    return rc;
}


/**
 * Separate function for reading creating the PE module instance to
 * simplify cleanup on failure.
 */
static int kldrModMachODoCreate(PKRDR pRdr, PKLDRMODMACHO *ppModMachO)
{
    union
    {
        mach_header_32_t    Hdr32;
        mach_header_64_t    Hdr64;
    } s;
    PKLDRMODMACHO pModMachO;
    PKLDRMOD pMod;
    KU8 *pbLoadCommands;
    KU32 cSegments;
    KU32 cSections;
    KU32 cbStringPool;
    KSIZE cchFilename;
    KSIZE cb;
    int rc;
    *ppModMachO = NULL;

    /*
     * Read the Mach-O header.
     */
    rc = kRdrRead(pRdr, &s, sizeof(s), 0);
    if (rc)
        return rc;
    if (s.Hdr32.magic != IMAGE_MACHO32_SIGNATURE)
    {
        if (    s.Hdr32.magic == IMAGE_MACHO32_SIGNATURE_OE
            ||  s.Hdr32.magic == IMAGE_MACHO64_SIGNATURE_OE)
            return KLDR_ERR_MACHO_OTHER_ENDIAN_NOT_SUPPORTED;
        if (s.Hdr32.magic == IMAGE_MACHO64_SIGNATURE)
            return KLDR_ERR_MACHO_64BIT_NOT_SUPPORTED;
        return KLDR_ERR_UNKNOWN_FORMAT;
    }

    /* sanity checks. */
    if (    s.Hdr32.sizeofcmds > kRdrSize(pRdr) - sizeof(mach_header_32_t)
        ||  s.Hdr32.sizeofcmds < sizeof(load_command_t) * s.Hdr32.ncmds
        ||  (s.Hdr32.flags & ~MH_VALID_FLAGS))
        return KLDR_ERR_MACHO_BAD_HEADER;
    switch (s.Hdr32.cputype)
    {
        case CPU_TYPE_X86:
        case CPU_TYPE_X86_64:
            break;
        default:
            return KLDR_ERR_MACHO_UNSUPPORTED_MACHINE;
    }
    if (s.Hdr32.filetype != MH_OBJECT)
        return KLDR_ERR_MACHO_UNSUPPORTED_FILE_TYPE;

    /*
     * Read and pre-parse the load commands to figure out how many segments we'll be needing.
     */
    pbLoadCommands = kHlpAlloc(s.Hdr32.sizeofcmds);
    if (!pbLoadCommands)
        return KERR_NO_MEMORY;
    rc = kRdrRead(pRdr, pbLoadCommands, s.Hdr32.sizeofcmds,
                        s.Hdr32.magic == IMAGE_MACHO32_SIGNATURE
                     || s.Hdr32.magic == IMAGE_MACHO32_SIGNATURE_OE
                     ? sizeof(mach_header_32_t) : sizeof(mach_header_64_t));
    if (!rc)
        rc = kldrModMachOPreParseLoadCommands(pbLoadCommands, &s.Hdr32, pRdr, &cSegments, &cSections, &cbStringPool);
    if (rc)
    {
        kHlpFree(pbLoadCommands);
        return rc;
    }


    /*
     * Calc the instance size, allocate and initialize it.
     */
    cchFilename = kHlpStrLen(kRdrName(pRdr));
    cb = K_ALIGN_Z(  K_OFFSETOF(KLDRMODMACHO, aSegments[cSegments])
                      + sizeof(KLDRMODMACHOSECT) * cSections, 16)
       + K_OFFSETOF(KLDRMOD, aSegments[cSegments])
       + cchFilename + 1
       + cbStringPool;
    pModMachO = (PKLDRMODMACHO)kHlpAlloc(cb);
    if (!pModMachO)
        return KERR_NO_MEMORY;
    *ppModMachO = pModMachO;
    pModMachO->pbLoadCommands = pbLoadCommands;

    /* KLDRMOD */
    pMod = (PKLDRMOD)((KU8 *)pModMachO + K_ALIGN_Z(  K_OFFSETOF(KLDRMODMACHO, aSegments[cSegments])
                                                      + sizeof(KLDRMODMACHOSECT) * cSections, 16));
    pMod->pvData = pModMachO;
    pMod->pRdr = pRdr;
    pMod->pOps = NULL;      /* set upon success. */
    pMod->cSegments = cSegments;
    pMod->cchFilename = cchFilename;
    pMod->pszFilename = (char *)&pMod->aSegments[pMod->cSegments];
    kHlpMemCopy((char *)pMod->pszFilename, kRdrName(pRdr), cchFilename + 1);
    pMod->pszName = kHlpGetFilename(pMod->pszFilename);
    pMod->cchName = cchFilename - (pMod->pszName - pMod->pszFilename);
    switch (s.Hdr32.cputype)
    {
        case CPU_TYPE_X86:
            pMod->enmArch = KCPUARCH_X86_32;
            pMod->enmEndian = KLDRENDIAN_LITTLE;
            switch (s.Hdr32.cpusubtype)
            {
                case CPU_SUBTYPE_I386_ALL:          pMod->enmCpu = KCPU_X86_32_BLEND; break;
                /*case CPU_SUBTYPE_386: ^^           pMod->enmCpu = KCPU_I386; break;*/
                case CPU_SUBTYPE_486:               pMod->enmCpu = KCPU_I486; break;
                case CPU_SUBTYPE_486SX:             pMod->enmCpu = KCPU_I486SX; break;
                /*case CPU_SUBTYPE_586: vv */
                case CPU_SUBTYPE_PENT:              pMod->enmCpu = KCPU_I586; break;
                case CPU_SUBTYPE_PENTPRO:
                case CPU_SUBTYPE_PENTII_M3:
                case CPU_SUBTYPE_PENTII_M5:
                case CPU_SUBTYPE_CELERON:
                case CPU_SUBTYPE_CELERON_MOBILE:
                case CPU_SUBTYPE_PENTIUM_3:
                case CPU_SUBTYPE_PENTIUM_3_M:
                case CPU_SUBTYPE_PENTIUM_3_XEON:    pMod->enmCpu = KCPU_I686; break;
                case CPU_SUBTYPE_PENTIUM_M:
                case CPU_SUBTYPE_PENTIUM_4:
                case CPU_SUBTYPE_PENTIUM_4_M:
                case CPU_SUBTYPE_XEON:
                case CPU_SUBTYPE_XEON_MP:           pMod->enmCpu = KCPU_P4; break;
                    break;
                default:
                    return KLDR_ERR_MACHO_UNSUPPORTED_MACHINE;
            }
            break;

        case CPU_TYPE_X86_64:
            pMod->enmArch = KCPUARCH_AMD64;
            pMod->enmEndian = KLDRENDIAN_LITTLE;
            switch (s.Hdr32.cpusubtype)
            {
                case CPU_SUBTYPE_X86_64_ALL:        pMod->enmCpu = KCPU_AMD64_BLEND; break;
                default:
                    return KLDR_ERR_MACHO_UNSUPPORTED_MACHINE;
            }
            break;

        default:
            return KLDR_ERR_MACHO_UNSUPPORTED_MACHINE;
    }

    pMod->enmFmt = KLDRFMT_MACHO;
    switch (s.Hdr32.filetype)
    {
        case MH_OBJECT:
            pMod->enmType = KLDRTYPE_OBJECT;
            break;
        default:
            return KLDR_ERR_MACHO_UNSUPPORTED_FILE_TYPE;
    }
    pMod->u32Magic = 0;     /* set upon success. */

    /* KLDRMODMACHO */
    pModMachO->pMod = pMod;
    pModMachO->pvBits = NULL;
    pModMachO->pvMapping = NULL;
    pModMachO->Hdr = s.Hdr64;
    if (    s.Hdr32.magic == IMAGE_MACHO32_SIGNATURE
        ||  s.Hdr32.magic == IMAGE_MACHO32_SIGNATURE_OE)
        pModMachO->Hdr.reserved = 0;
    pModMachO->LinkAddress = 0;
    pModMachO->cbImage = 0;
    pModMachO->fMapUsingLoadCommandSections = 0;
    pModMachO->offSymbols = 0;
    pModMachO->cSymbols = 0;
    pModMachO->pvaSymbols = NULL;
    pModMachO->offStrings = 0;
    pModMachO->cchStrings = 0;
    pModMachO->pchStrings = NULL;
    pModMachO->cSections = cSections;
    pModMachO->paSections = (PKLDRMODMACHOSECT)&pModMachO->aSegments[pModMachO->pMod->cSegments];

    /*
     * Setup the KLDRMOD segment array.
     */
    rc = kldrModMachOParseLoadCommands(pModMachO, (char *)pMod->pszFilename + pMod->cchFilename + 1, cbStringPool);
    if (rc)
        return rc;

    /*
     * We're done.
     */
    return 0;
}


/**
 * Converts, validates and preparses the load commands before we carve
 * out the module instance.
 *
 * The conversion that's preformed is format endian to host endian.
 * The preparsing has to do with segment counting, section counting and string pool sizing.
 *
 * @returns 0 on success.
 * @returns KLDR_ERR_MACHO_* on failure.
 * @param   pbLoadCommands  The load commands to parse.
 * @param   pHdr            The header.
 * @param   pRdr            The file reader.
 * @param   pcSegments      Where to store the segment count.
 * @param   pcSegments      Where to store the section count.
 * @param   pcbStringPool   Where to store the string pool size.
 */
static int  kldrModMachOPreParseLoadCommands(KU8 *pbLoadCommands, const mach_header_32_t *pHdr, PKRDR pRdr,
                                             KU32 *pcSegments, KU32 *pcSections, KU32 *pcbStringPool)
{
    union
    {
        KU8                  *pb;
        load_command_t       *pLoadCmd;
        segment_command_32_t *pSeg32;
        segment_command_64_t *pSeg64;
        thread_command_t     *pThread;
        symtab_command_t     *pSymTab;
        uuid_command_t       *pUuid;
    } u;
    const KU64 cbFile = kRdrSize(pRdr);
    KU32 cSegments = 0;
    KU32 cSections = 0;
    KU32 cbStringPool = 0;
    KU32 cLeft = pHdr->ncmds;
    KU32 cbLeft = pHdr->sizeofcmds;
    KU8 *pb = pbLoadCommands;
    int cSegmentCommands = 0;
    int cSymbolTabs = 0;
    int fConvertEndian = pHdr->magic == IMAGE_MACHO32_SIGNATURE_OE
                      || pHdr->magic == IMAGE_MACHO64_SIGNATURE_OE;

    *pcSegments = 0;
    *pcSections = 0;
    *pcbStringPool = 0;

    while (cLeft-- > 0)
    {
        u.pb = pb;

        /*
         * Convert and validate command header.
         */
        if (cbLeft < sizeof(load_command_t))
            return KLDR_ERR_MACHO_BAD_LOAD_COMMAND;
        if (fConvertEndian)
        {
            u.pLoadCmd->cmd = K_E2E_U32(u.pLoadCmd->cmd);
            u.pLoadCmd->cmdsize = K_E2E_U32(u.pLoadCmd->cmdsize);
        }
        if (u.pLoadCmd->cmdsize > cbLeft)
            return KLDR_ERR_MACHO_BAD_LOAD_COMMAND;
        cbLeft -= u.pLoadCmd->cmdsize;
        pb += u.pLoadCmd->cmdsize;

        /*
         * Convert endian if needed, parse and validate the command.
         */
        switch (u.pLoadCmd->cmd)
        {
            case LC_SEGMENT_32:
            {
                section_32_t *pSect;
                section_32_t *pFirstSect;
                KU32 cSectionsLeft;

                /* convert and verify*/
                if (u.pLoadCmd->cmdsize < sizeof(segment_command_32_t))
                    return KLDR_ERR_MACHO_BAD_LOAD_COMMAND;
                if (    pHdr->magic != IMAGE_MACHO32_SIGNATURE_OE
                    &&  pHdr->magic != IMAGE_MACHO32_SIGNATURE)
                    return KLDR_ERR_MACHO_BIT_MIX;
                if (fConvertEndian)
                {
                    u.pSeg32->vmaddr   = K_E2E_U32(u.pSeg32->vmaddr);
                    u.pSeg32->vmsize   = K_E2E_U32(u.pSeg32->vmsize);
                    u.pSeg32->fileoff  = K_E2E_U32(u.pSeg32->fileoff);
                    u.pSeg32->filesize = K_E2E_U32(u.pSeg32->filesize);
                    u.pSeg32->maxprot  = K_E2E_U32(u.pSeg32->maxprot);
                    u.pSeg32->initprot = K_E2E_U32(u.pSeg32->initprot);
                    u.pSeg32->nsects   = K_E2E_U32(u.pSeg32->nsects);
                    u.pSeg32->flags    = K_E2E_U32(u.pSeg32->flags);
                }

                if (    u.pSeg32->filesize
                    &&  (   u.pSeg32->fileoff > cbFile
                         || (KU64)u.pSeg32->fileoff + u.pSeg32->filesize > cbFile))
                    return KLDR_ERR_MACHO_BAD_LOAD_COMMAND;
                if (!u.pSeg32->filesize && u.pSeg32->fileoff)
                    return KLDR_ERR_MACHO_BAD_LOAD_COMMAND;
                if (u.pSeg32->vmsize < u.pSeg32->filesize)
                    return KLDR_ERR_MACHO_BAD_LOAD_COMMAND;
                if ((u.pSeg32->maxprot & u.pSeg32->initprot) != u.pSeg32->initprot)
                    return KLDR_ERR_MACHO_BAD_LOAD_COMMAND;
                if (u.pSeg32->flags & ~(SG_HIGHVM | SG_FVMLIB | SG_NORELOC | SG_PROTECTED_VERSION_1))
                    return KLDR_ERR_MACHO_BAD_LOAD_COMMAND;
                if (u.pSeg32->nsects * sizeof(section_32_t) > u.pLoadCmd->cmdsize - sizeof(segment_command_32_t))
                    return KLDR_ERR_MACHO_BAD_LOAD_COMMAND;
                if (    pHdr->filetype == MH_OBJECT
                    &&  cSegmentCommands > 0)
                    return KLDR_ERR_MACHO_BAD_OBJECT_FILE;
                cSegmentCommands++;

                /*
                 * convert, validate and parse the sections.
                 */
                cSectionsLeft = u.pSeg32->nsects;
                pFirstSect = pSect = (section_32_t *)(u.pSeg32 + 1);
                while (cSectionsLeft-- > 0)
                {
                    int fFileBits;

                    if (fConvertEndian)
                    {
                        pSect->addr      = K_E2E_U32(pSect->addr);
                        pSect->size      = K_E2E_U32(pSect->size);
                        pSect->offset    = K_E2E_U32(pSect->offset);
                        pSect->align     = K_E2E_U32(pSect->align);
                        pSect->reloff    = K_E2E_U32(pSect->reloff);
                        pSect->nreloc    = K_E2E_U32(pSect->nreloc);
                        pSect->flags     = K_E2E_U32(pSect->flags);
                        pSect->reserved1 = K_E2E_U32(pSect->reserved1);
                        pSect->reserved2 = K_E2E_U32(pSect->reserved2);
                    }

                    /* validate */
                    switch (pSect->flags & SECTION_TYPE)
                    {
                        case S_ZEROFILL:
                            if (pSect->reserved1 || pSect->reserved2)
                                return KLDR_ERR_MACHO_BAD_SECTION;
                            fFileBits = 0;
                            break;
                        case S_REGULAR:
                        case S_CSTRING_LITERALS:
                        case S_COALESCED:
                        case S_4BYTE_LITERALS:
                        case S_8BYTE_LITERALS:
                        case S_16BYTE_LITERALS:
                            if (pSect->reserved1 || pSect->reserved2)
                                return KLDR_ERR_MACHO_BAD_SECTION;
                            fFileBits = 1;
                            break;

                        case S_LITERAL_POINTERS:
                        case S_INTERPOSING:
                        case S_GB_ZEROFILL:
                        case S_NON_LAZY_SYMBOL_POINTERS:
                        case S_LAZY_SYMBOL_POINTERS:
                        case S_SYMBOL_STUBS:
                        case S_MOD_INIT_FUNC_POINTERS:
                        case S_MOD_TERM_FUNC_POINTERS:
                            return KLDR_ERR_MACHO_UNSUPPORTED_SECTION;

                        default:
                            return KLDR_ERR_MACHO_UNKNOWN_SECTION;
                    }
                    if (pSect->flags & ~(  S_ATTR_PURE_INSTRUCTIONS | S_ATTR_NO_TOC | S_ATTR_STRIP_STATIC_SYMS
                                         | S_ATTR_NO_DEAD_STRIP | S_ATTR_LIVE_SUPPORT | S_ATTR_SELF_MODIFYING_CODE
                                         | S_ATTR_DEBUG | S_ATTR_SOME_INSTRUCTIONS | S_ATTR_EXT_RELOC
                                         | S_ATTR_LOC_RELOC | SECTION_TYPE))
                        return KLDR_ERR_MACHO_BAD_SECTION;
                    if (    pSect->addr - u.pSeg32->vmaddr > u.pSeg32->vmsize
                        ||  pSect->addr - u.pSeg32->vmaddr + pSect->size > u.pSeg32->vmsize)
                        return KLDR_ERR_MACHO_BAD_SECTION;
                    if (    pSect->align >= 31
                        ||  (((1 << pSect->align) - 1) & pSect->addr)
                        ||  (((1 << pSect->align) - 1) & u.pSeg32->vmaddr))
                        return KLDR_ERR_MACHO_BAD_SECTION;
                    if (    fFileBits
                        &&  (   pSect->offset > cbFile
                             || (KU64)pSect->offset + pSect->size > cbFile))
                        return KLDR_ERR_MACHO_BAD_SECTION;
                    if (!fFileBits && pSect->offset)
                        return KLDR_ERR_MACHO_BAD_SECTION;
                    if (!pSect->nreloc && pSect->reloff)
                        return KLDR_ERR_MACHO_BAD_SECTION;
                    if (    pSect->nreloc
                        &&  (   pSect->reloff > cbFile
                             || (KU64)pSect->reloff + (KLDRFOFF)pSect->nreloc * sizeof(macho_relocation_info_t)) > cbFile)
                        return KLDR_ERR_MACHO_BAD_SECTION;


                    /* count segments and strings */
                    switch (pHdr->filetype)
                    {
                        case MH_OBJECT:
                        {
                            cSections++;

                            /* Don't load debug symbols. (test this) */
                            if (pSect->flags & S_ATTR_DEBUG)
                                break;

                            /* a new segment? */
                            if (    !cSegments
                                ||  kHlpStrNComp(pSect->segname, (pSect - 1)->segname, sizeof(pSect->segname)))
                            {
#if 0 /** @todo This  doesn't work because of BSS. */
                                /* verify that the linker/assembler has ordered sections correctly. */
                                section_32_t *pCur = (pSect - 2);
                                while ((KUPTR)pCur >= (KUPTR)pFirstSect)
                                {
                                    if (!kHlpStrNComp(pCur->segname, pSect->segname, sizeof(pSect->segname)))
                                        return KLDR_ERR_MACHO_BAD_SECTION_ORDER;
                                    pCur--;
                                }
#endif

                                /* ok. count it and the string. */
                                cSegments++;
                                cbStringPool += kHlpStrNLen(&pSect->segname[0], sizeof(pSect->segname)) + 1;
                            }
                            break;
                        }

                        default:
                            return KERR_INVALID_PARAMETER;
                    }

                    /* next */
                    pSect++;
                }
                break;
            }

            /*case LC_SEGMENT_64:
                 copy 32-bit code
                break;
            */

            case LC_SYMTAB:
            {
                KSIZE cbSym;
                if (fConvertEndian)
                {
                    u.pSymTab->symoff  = K_E2E_U32(u.pSymTab->symoff);
                    u.pSymTab->nsyms   = K_E2E_U32(u.pSymTab->nsyms);
                    u.pSymTab->stroff  = K_E2E_U32(u.pSymTab->stroff);
                    u.pSymTab->strsize = K_E2E_U32(u.pSymTab->strsize);
                }

                /* verify */
                cbSym = pHdr->magic == IMAGE_MACHO32_SIGNATURE
                     || pHdr->magic == IMAGE_MACHO32_SIGNATURE_OE
                      ? sizeof(macho_nlist_32_t)
                      : sizeof(macho_nlist_64_t);
                if (    u.pSymTab->symoff >= cbFile
                    ||  (KU64)u.pSymTab->symoff + u.pSymTab->nsyms * cbSym > cbFile)
                    return KLDR_ERR_MACHO_BAD_LOAD_COMMAND;
                if (    u.pSymTab->stroff >= cbFile
                    ||  (KU64)u.pSymTab->stroff + u.pSymTab->strsize > cbFile)
                    return KLDR_ERR_MACHO_BAD_LOAD_COMMAND;

                /* only one string in objects, please. */
                cSymbolTabs++;
                if (    pHdr->filetype == MH_OBJECT
                    &&  cSymbolTabs != 1)
                    return KLDR_ERR_MACHO_BAD_OBJECT_FILE;
                break;
            }

            case LC_DYSYMTAB:
                /** @todo deal with this! */
                break;

            case LC_THREAD:
            case LC_UNIXTHREAD:
            {
                KU32 *pu32 = (KU32 *)(u.pb + sizeof(load_command_t));
                KU32 cItemsLeft = (u.pThread->cmdsize - sizeof(load_command_t)) / sizeof(KU32);
                while (cItemsLeft)
                {
                    /* convert & verify header items ([0] == flavor, [1] == KU32 count). */
                    if (cItemsLeft < 2)
                        return KLDR_ERR_MACHO_BAD_LOAD_COMMAND;
                    if (fConvertEndian)
                    {
                        pu32[0] = K_E2E_U32(pu32[0]);
                        pu32[1] = K_E2E_U32(pu32[1]);
                    }
                    if (pu32[1] + 2 > cItemsLeft)
                        return KLDR_ERR_MACHO_BAD_LOAD_COMMAND;

                    /* convert & verify according to flavor. */
                    switch (pu32[0])
                    {
                        /** @todo */
                        default:
                            break;
                    }

                    /* next */
                    cItemsLeft -= pu32[1] + 2;
                    pu32 += pu32[1] + 2;
                }
                break;
            }

            case LC_UUID:
                if (u.pUuid->cmdsize != sizeof(uuid_command_t))
                    return KLDR_ERR_MACHO_BAD_LOAD_COMMAND;
                /** @todo Check anything here need converting? */
                break;

            case LC_SEGMENT_64:
            case LC_LOADFVMLIB:
            case LC_IDFVMLIB:
            case LC_IDENT:
            case LC_FVMFILE:
            case LC_PREPAGE:
            case LC_LOAD_DYLIB:
            case LC_ID_DYLIB:
            case LC_LOAD_DYLINKER:
            case LC_ID_DYLINKER:
            case LC_PREBOUND_DYLIB:
            case LC_ROUTINES:
            case LC_ROUTINES_64:
            case LC_SUB_FRAMEWORK:
            case LC_SUB_UMBRELLA:
            case LC_SUB_CLIENT:
            case LC_SUB_LIBRARY:
            case LC_TWOLEVEL_HINTS:
            case LC_PREBIND_CKSUM:
            case LC_LOAD_WEAK_DYLIB:
            case LC_SYMSEG:
                return KLDR_ERR_MACHO_UNSUPPORTED_LOAD_COMMAND;

            default:
                return KLDR_ERR_MACHO_UNKNOWN_LOAD_COMMAND;
        }
    }

    /* be strict. */
    if (cbLeft)
        return KLDR_ERR_MACHO_BAD_LOAD_COMMAND;

    switch (pHdr->filetype)
    {
        case MH_OBJECT:
            if (!cSegments)
                return KLDR_ERR_MACHO_BAD_OBJECT_FILE;
            break;
    }

    *pcSegments = cSegments;
    *pcSections = cSections;
    *pcbStringPool = cbStringPool;

    return 0;
}


/**
 * Parses the load commands after we've carved out the module instance.
 *
 * This fills in the segment table and perhaps some other properties.
 *
 * @returns 0 on success.
 * @returns KLDR_ERR_MACHO_* on failure.
 * @param   pModMachO       The module.
 * @param   pbStringPool    The string pool
 * @param   cbStringPool    The size of the string pool.
 */
static int  kldrModMachOParseLoadCommands(PKLDRMODMACHO pModMachO, char *pbStringPool, KU32 cbStringPool)
{
    union
    {
        const KU8                  *pb;
        const load_command_t       *pLoadCmd;
        const segment_command_32_t *pSeg32;
        const segment_command_64_t *pSeg64;
        const symtab_command_t     *pSymTab;
    } u;
    KU32 cLeft = pModMachO->Hdr.ncmds;
    KU32 cbLeft = pModMachO->Hdr.sizeofcmds;
    const KU8 *pb = pModMachO->pbLoadCommands;
    int fFirstSegment = 1;
    PKLDRSEG pSeg = &pModMachO->pMod->aSegments[0];
    PKLDRMODMACHOSEG pSegExtra = &pModMachO->aSegments[0];
    PKLDRMODMACHOSECT pSectExtra = pModMachO->paSections;
    const KU32 cSegments = pModMachO->pMod->cSegments;
    KU32 i;

    while (cLeft-- > 0)
    {
        u.pb = pb;
        cbLeft -= u.pLoadCmd->cmdsize;
        pb += u.pLoadCmd->cmdsize;

        /*
         * Convert endian if needed, parse and validate the command.
         */
        switch (u.pLoadCmd->cmd)
        {
            case LC_SEGMENT_32:
            {
                section_32_t *pSect;
                section_32_t *pFirstSect;
                KU32 cSectionsLeft;

                pModMachO->LinkAddress = u.pSeg32->vmaddr;

                /*
                 * convert, validate and parse the sections.
                 */
                cSectionsLeft = u.pSeg32->nsects;
                pFirstSect = pSect = (section_32_t *)(u.pSeg32 + 1);
                while (cSectionsLeft-- > 0)
                {
                    switch (pModMachO->Hdr.filetype)
                    {
                        case MH_OBJECT:
                        {
                            /* Section data extract. */
                            pSectExtra->cb = pSect->size;
                            pSectExtra->RVA = pSect->addr;
                            pSectExtra->LinkAddress = pSect->addr;
                            pSectExtra->offFile = pSect->offset ? pSect->offset : -1;
                            pSectExtra->cFixups = pSect->nreloc;
                            pSectExtra->paFixups = NULL;
                            pSectExtra->offFixups = pSect->nreloc ? pSect->reloff : -1;
                            pSectExtra->fFlags = pSect->flags;
                            pSectExtra->iSegment = pSegExtra - &pModMachO->aSegments[0];
                            pSectExtra->pvMachoSection = pSect;

                            /* Don't load debug symbols. (test this!) */
                            if (pSect->flags & S_ATTR_DEBUG)
                            {
                                pSectExtra++;
                                /** @todo */
                                break;
                            }

                            if (    fFirstSegment
                                ||  kHlpStrNComp(pSect->segname, (pSect - 1)->segname, sizeof(pSect->segname)))
                            {
                                /* close the previous segment */
                                if (pSegExtra != &pModMachO->aSegments[0])
                                    pSegExtra[-1].cSections = pSectExtra - pSegExtra[-1].paSections;

                                /* new segment. */
                                pSeg->pvUser = NULL;
                                pSeg->pchName = pbStringPool;
                                pSeg->cchName = (KU32)kHlpStrNLen(&pSect->segname[0], sizeof(pSect->sectname));
                                kHlpMemCopy(pbStringPool, &pSect->segname[0], pSeg->cchName);
                                pbStringPool += pSeg->cchName;
                                *pbStringPool++ = '\0';
                                pSeg->SelFlat = 0;
                                pSeg->Sel16bit = 0;
                                pSeg->fFlags = 0;
                                pSeg->enmProt = KPROT_EXECUTE_WRITECOPY; /** @todo fixme! */
                                pSeg->cb = pSect->size;
                                pSeg->Alignment = (1 << pSect->align);
                                pSeg->LinkAddress = pSect->addr;
                                pSeg->offFile = pSect->offset ? pSect->offset : -1;
                                pSeg->cbFile  = pSect->offset ? pSect->size : -1;
                                pSeg->RVA = pSect->addr - pModMachO->LinkAddress;
                                pSeg->cbMapped = 0;
                                pSeg->MapAddress = 0;

                                pSegExtra->cSections = 0;
                                pSegExtra->paSections = pSectExtra;

                                pSeg++;
                                pSegExtra++;
                                fFirstSegment = 0;
                            }
                            else
                            {
                                /* update exiting segment */
                                if (pSeg[-1].Alignment < (1 << pSect->align))
                                    pSeg[-1].Alignment = (1 << pSect->align);
                                if (pSect->addr < pSeg[-1].LinkAddress)
                                    return KLDR_ERR_MACHO_BAD_SECTION; /** @todo move up! */

                                /* If there are file bits, ensure they are in the current flow.
                                   (yes, we are very very careful here, I know.) */
                                if (    pSect->offset
                                    &&  pSeg[-1].cbFile == pSeg[-1].cb)
                                {
                                    int fOk = pSeg[-1].offFile + (pSect->addr - pSeg[-1].LinkAddress) == pSect->offset
                                           && pSect[-1].offset
                                           && pSeg[-1].offFile + pSeg[-1].cbFile == pSect[-1].offset + pSect[-1].size;
                                    /* more checks? */
                                    if (fOk)
                                        pSeg[-1].cbFile = (KLDRFOFF)(pSect->addr - pSeg[-1].LinkAddress) + pSect->size;
                                    else
                                    {

                                        pSeg[-1].cbFile = pSeg[-1].offFile = -1;
                                        pModMachO->fMapUsingLoadCommandSections = 1;
                                    }
                                }
                                pSeg[-1].cb = pSect->addr - pSeg[-1].LinkAddress + pSect->size;

                                /** @todo update the protection... */
                            }
                            pSectExtra++;
                            break;
                        }

                        default:
                            return KERR_INVALID_PARAMETER;
                    }

                    /* next */
                    pSect++;
                }
                break;
            }

            case LC_SYMTAB:
                switch (pModMachO->Hdr.filetype)
                {
                    case MH_OBJECT:
                        pModMachO->offSymbols = u.pSymTab->symoff;
                        pModMachO->cSymbols = u.pSymTab->nsyms;
                        pModMachO->offStrings = u.pSymTab->stroff;
                        pModMachO->cchStrings = u.pSymTab->strsize;
                        break;
                }
                break;

            default:
                break;
        } /* command switch */
    } /* while more commands */

    /*
     * Close the last segment (if any).
     */
    if (pSegExtra != &pModMachO->aSegments[0])
        pSegExtra[-1].cSections = pSectExtra - pSegExtra[-1].paSections;

    /*
     * Adjust mapping addresses calculating the image size.
     */
    pSeg = &pModMachO->pMod->aSegments[0];
    switch (pModMachO->Hdr.filetype)
    {
        case MH_OBJECT:
        {
            KLDRADDR cb1;
            KSIZE cb2;

            for (i = 0; i < cSegments - 1; i++)
            {
                cb1 = pSeg[i + 1].LinkAddress - pSeg[i].LinkAddress;
                cb2 = (KSIZE)cb1;
                pSeg[i].cbMapped = cb2 == cb1 ? cb2 : ~(KSIZE)0;
            }
            cb1 = KLDR_ALIGN_ADDR(pSeg[i].cb, pSeg[i].Alignment);
            cb2 = (KSIZE)cb1;
            pSeg[i].cbMapped = cb2 == cb1 ? cb2 : ~(KSIZE)0;

            pModMachO->cbImage = pSeg[i].RVA + cb1;
            break;
        }
    }

    return 0;
}


/** @copydoc KLDRMODOPS::pfnDestroy */
static int kldrModMachODestroy(PKLDRMOD pMod)
{
    PKLDRMODMACHO pModMachO = (PKLDRMODMACHO)pMod->pvData;
    int rc = 0;
    KU32 i, j;
    KLDRMODMACHO_ASSERT(!pModMachO->pvMapping);

    i = pMod->cSegments;
    while (i-- > 0)
    {
        j = pModMachO->aSegments[i].cSections;
        while (j-- > 0)
        {
            kHlpFree(pModMachO->aSegments[i].paSections[j].paFixups);
            pModMachO->aSegments[i].paSections[j].paFixups = NULL;
        }
    }

    if (pMod->pRdr)
    {
        rc = kRdrClose(pMod->pRdr);
        pMod->pRdr = NULL;
    }
    pMod->u32Magic = 0;
    pMod->pOps = NULL;
    kHlpFree(pModMachO->pbLoadCommands);
    pModMachO->pbLoadCommands = NULL;
    kHlpFree(pModMachO->pchStrings);
    pModMachO->pchStrings = NULL;
    kHlpFree(pModMachO->pvaSymbols);
    pModMachO->pvaSymbols = NULL;
    kHlpFree(pModMachO);
    return rc;
}


/**
 * Gets the right base address.
 *
 * @returns 0 on success.
 * @returns A non-zero status code if the BaseAddress isn't right.
 * @param   pModMachO       The interpreter module instance
 * @param   pBaseAddress    The base address, IN & OUT. Optional.
 */
static int kldrModMachOAdjustBaseAddress(PKLDRMODMACHO pModMachO, PKLDRADDR pBaseAddress)
{
    /*
     * Adjust the base address.
     */
    if (*pBaseAddress == KLDRMOD_BASEADDRESS_MAP)
        *pBaseAddress = pModMachO->pMod->aSegments[0].MapAddress;
    else if (*pBaseAddress == KLDRMOD_BASEADDRESS_LINK)
        *pBaseAddress = pModMachO->LinkAddress;

    return 0;
}



/** @copydoc kLdrModQuerySymbol */
static int kldrModMachOQuerySymbol(PKLDRMOD pMod, const void *pvBits, KLDRADDR BaseAddress, KU32 iSymbol,
                                   const char *pchSymbol, KSIZE cchSymbol, const char *pszVersion,
                                   PFNKLDRMODGETIMPORT pfnGetForwarder, void *pvUser, PKLDRADDR puValue, KU32 *pfKind)
{
    PKLDRMODMACHO pModMachO = (PKLDRMODMACHO)pMod->pvData;
    int rc;

    /*
     * Resolve defaults.
     */
    rc  = kldrModMachOAdjustBaseAddress(pModMachO, &BaseAddress);
    if (rc)
        return rc;

    /*
     * Refuse segmented requests for now.
     */
    if (    pfKind
        &&  (*pfKind & KLDRSYMKIND_REQ_TYPE_MASK) != KLDRSYMKIND_REQ_FLAT)
        return KLDR_ERR_TODO;

    /*
     * Take action according to file type.
     */
    if (pModMachO->Hdr.filetype == MH_OBJECT)
    {
        rc = kldrModMachOLoadObjSymTab(pModMachO);
        if (!rc)
        {
            if (    pModMachO->Hdr.magic == IMAGE_MACHO32_SIGNATURE
                ||  pModMachO->Hdr.magic == IMAGE_MACHO32_SIGNATURE_OE)
                rc = kldrModMachODoQuerySymbol32Bit(pModMachO, (macho_nlist_32_t *)pModMachO->pvaSymbols, pModMachO->cSymbols,
                                                    pModMachO->pchStrings, pModMachO->cchStrings, BaseAddress, iSymbol, pchSymbol,
                                                    cchSymbol, puValue, pfKind);
            else
                rc = KLDR_ERR_TODO; /** @todo duplicate kldrModMachOQuerySymbol32 for parsing 64-bit symbols when everything is working. */
                /*rc = kldrModMachODoQuerySymbol64Bit(pModMachO, (macho_nlist_64_t *)pModMachO->pvaSymbols, pModMachO->cSymbols,
                                                    pModMachO->pchStrings, pModMachO->cchStrings, BaseAddress, iSymbol, pchSymbol,
                                                    cchSymbol, puValue, pfKind);*/

        }
    }
    else
        rc = KLDR_ERR_TODO;

    return rc;
}



/**
 * Lookup a symbol in a 32-bit symbol table.
 *
 * @returns See kLdrModQuerySymbol.
 * @param   pModMachO
 * @param   paSyms      Pointer to the symbol table.
 * @param   cSyms       Number of symbols in the table.
 * @param   pchStrings  Pointer to the string table.
 * @param   cchStrings  Size of the string table.
 * @param   BaseAddress Adjusted base address, see kLdrModQuerySymbol.
 * @param   iSymbol     See kLdrModQuerySymbol.
 * @param   pchSymbol   See kLdrModQuerySymbol.
 * @param   cchSymbol   See kLdrModQuerySymbol.
 * @param   puValue     See kLdrModQuerySymbol.
 * @param   pfKind      See kLdrModQuerySymbol.
 */
static int kldrModMachODoQuerySymbol32Bit(PKLDRMODMACHO pModMachO, const macho_nlist_32_t *paSyms, KU32 cSyms, const char *pchStrings,
                                          KU32 cchStrings, KLDRADDR BaseAddress, KU32 iSymbol, const char *pchSymbol, KSIZE cchSymbol,
                                          PKLDRADDR puValue, KU32 *pfKind)
{
    /*
     * Find a valid symbol matching the search criteria.
     */
    if (iSymbol == NIL_KLDRMOD_SYM_ORDINAL)
    {
        /* simplify validation. */
        if (cchStrings <= cchSymbol)
            return KLDR_ERR_SYMBOL_NOT_FOUND;
        cchStrings -= cchSymbol;

        /* external symbols are usually at the end, so search the other way. */
        for (iSymbol = cSyms - 1; iSymbol != KU32_MAX; iSymbol--)
        {
            const char *psz;

            /* Skip irrellevant and non-public symbols. */
            if (paSyms[iSymbol].n_type & MACHO_N_STAB)
                continue;
            if ((paSyms[iSymbol].n_type & MACHO_N_TYPE) == MACHO_N_UNDF)
                continue;
            if (!(paSyms[iSymbol].n_type & MACHO_N_EXT)) /*??*/
                continue;
            if (paSyms[iSymbol].n_type & MACHO_N_PEXT) /*??*/
                continue;

            /* get name */
            if (!paSyms[iSymbol].n_un.n_strx)
                continue;
            if ((KU32)paSyms[iSymbol].n_un.n_strx >= cchStrings)
                continue;
            psz = &pchStrings[paSyms[iSymbol].n_un.n_strx];
            if (psz[cchSymbol])
                continue;
            if (kHlpMemComp(psz, pchSymbol, cchSymbol))
                continue;

            /* match! */
            break;
        }
        if (iSymbol == KU32_MAX)
            return KLDR_ERR_SYMBOL_NOT_FOUND;
    }
    else
    {
        if (iSymbol >= cSyms)
            return KLDR_ERR_SYMBOL_NOT_FOUND;
        if (paSyms[iSymbol].n_type & MACHO_N_STAB)
            return KLDR_ERR_SYMBOL_NOT_FOUND;
        if ((paSyms[iSymbol].n_type & MACHO_N_TYPE) == MACHO_N_UNDF)
            return KLDR_ERR_SYMBOL_NOT_FOUND;
    }

    /*
     * Calc the return values.
     */
    if (pfKind)
    {
        if (    pModMachO->Hdr.magic == IMAGE_MACHO32_SIGNATURE
            ||  pModMachO->Hdr.magic == IMAGE_MACHO32_SIGNATURE_OE)
            *pfKind = KLDRSYMKIND_32BIT | KLDRSYMKIND_NO_TYPE;
        else
            *pfKind = KLDRSYMKIND_64BIT | KLDRSYMKIND_NO_TYPE;
        if (paSyms[iSymbol].n_desc & N_WEAK_DEF)
            *pfKind |= KLDRSYMKIND_WEAK;
    }

    switch (paSyms[iSymbol].n_type & MACHO_N_TYPE)
    {
        case MACHO_N_SECT:
        {
            PKLDRMODMACHOSECT pSect;
            KLDRADDR RVA;
            if ((KU32)(paSyms[iSymbol].n_sect - 1) >= pModMachO->cSections)
                return KLDR_ERR_MACHO_BAD_SYMBOL;
            pSect = &pModMachO->paSections[paSyms[iSymbol].n_sect - 1];

            RVA = paSyms[iSymbol].n_value - pModMachO->LinkAddress;
            if (RVA - pSect->RVA >= pSect->cb)
                return KLDR_ERR_MACHO_BAD_SYMBOL;
            if (puValue)
                *puValue = RVA + BaseAddress;

            if (    pfKind
                &&  (pSect->fFlags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SELF_MODIFYING_CODE)))
                *pfKind = (*pfKind & ~KLDRSYMKIND_TYPE_MASK) | KLDRSYMKIND_CODE;
            break;
        }

        case MACHO_N_ABS:
            if (puValue)
                *puValue = paSyms[iSymbol].n_value;
            /*if (pfKind)
                pfKind |= KLDRSYMKIND_ABS;*/
            break;

        case MACHO_N_PBUD:
        case MACHO_N_INDR:
            /** @todo implement indirect and prebound symbols. */
        default:
            KLDRMODMACHO_ASSERT(0);
            return KLDR_ERR_TODO;
    }

    return 0;
}


/** @copydoc kLdrModEnumSymbols */
static int kldrModMachOEnumSymbols(PKLDRMOD pMod, const void *pvBits, KLDRADDR BaseAddress,
                                   KU32 fFlags, PFNKLDRMODENUMSYMS pfnCallback, void *pvUser)
{
    PKLDRMODMACHO pModMachO = (PKLDRMODMACHO)pMod->pvData;
    int rc;

    /*
     * Resolve defaults.
     */
    rc  = kldrModMachOAdjustBaseAddress(pModMachO, &BaseAddress);
    if (rc)
        return rc;

    /*
     * Take action according to file type.
     */
    if (pModMachO->Hdr.filetype == MH_OBJECT)
    {
        rc = kldrModMachOLoadObjSymTab(pModMachO);
        if (!rc)
        {
            if (    pModMachO->Hdr.magic == IMAGE_MACHO32_SIGNATURE
                ||  pModMachO->Hdr.magic == IMAGE_MACHO32_SIGNATURE_OE)
                rc = kldrModMachODoEnumSymbols32Bit(pModMachO, (macho_nlist_32_t *)pModMachO->pvaSymbols, pModMachO->cSymbols,
                                                    pModMachO->pchStrings, pModMachO->cchStrings, BaseAddress,
                                                    fFlags, pfnCallback, pvUser);
            else
                rc = KLDR_ERR_TODO; /** @todo duplicate kldrModMachOQuerySymbol32 for parsing 64-bit symbols when everything is working. */
                /*rc = kldrModMachODoEnumSymbols32Bit(pModMachO, (macho_nlist_32_t *)pModMachO->pvaSymbols, pModMachO->cSymbols,
                                                    pModMachO->pchStrings, pModMachO->cchStrings, BaseAddress, pfnCallback, pvUser);*/
        }
    }
    else
        rc = KLDR_ERR_TODO;

    return rc;
}


/**
 * Enum a 32-bit symbol table.
 *
 * @returns See kLdrModQuerySymbol.
 * @param   pModMachO
 * @param   paSyms      Pointer to the symbol table.
 * @param   cSyms       Number of symbols in the table.
 * @param   pchStrings  Pointer to the string table.
 * @param   cchStrings  Size of the string table.
 * @param   BaseAddress Adjusted base address, see kLdrModEnumSymbols.
 * @param   fFlags      See kLdrModEnumSymbols.
 * @param   pfnCallback See kLdrModEnumSymbols.
 * @param   pvUser      See kLdrModEnumSymbols.
 */
static int kldrModMachODoEnumSymbols32Bit(PKLDRMODMACHO pModMachO, const macho_nlist_32_t *paSyms, KU32 cSyms,
                                          const char *pchStrings, KU32 cchStrings, KLDRADDR BaseAddress,
                                          KU32 fFlags, PFNKLDRMODENUMSYMS pfnCallback, void *pvUser)
{
    const KU32 fKindBase = pModMachO->Hdr.magic == IMAGE_MACHO32_SIGNATURE
                            || pModMachO->Hdr.magic == IMAGE_MACHO32_SIGNATURE_OE
                             ? KLDRSYMKIND_32BIT : KLDRSYMKIND_64BIT;
    KU32 iSym;
    int rc;

    /*
     * Iterate the symbol table.
     */
    for (iSym = 0; iSym < cSyms; iSym++)
    {
        KU32 fKind;
        KLDRADDR uValue;
        const char *psz;
        KSIZE cch;

        /* Skip debug symbols and undefined symbols. */
        if (paSyms[iSym].n_type & MACHO_N_STAB)
            continue;
        if ((paSyms[iSym].n_type & MACHO_N_TYPE) == MACHO_N_UNDF)
            continue;

        /* Skip non-public symbols unless they are requested explicitly. */
        if (!(fFlags & KLDRMOD_ENUM_SYMS_FLAGS_ALL))
        {
            if (!(paSyms[iSym].n_type & MACHO_N_EXT)) /*??*/
                continue;
            if (paSyms[iSym].n_type & MACHO_N_PEXT) /*??*/
                continue;
            if (!paSyms[iSym].n_un.n_strx)
                continue;
        }

        /*
         * Gather symbol info
         */

        /* name */
        if ((KU32)paSyms[iSym].n_un.n_strx >= cchStrings)
            return KLDR_ERR_MACHO_BAD_SYMBOL;
        psz = &pchStrings[paSyms[iSym].n_un.n_strx];
        cch = kHlpStrLen(psz);
        if (!cch)
            psz = NULL;

        /* kind & value */
        fKind = fKindBase;
        if (paSyms[iSym].n_desc & N_WEAK_DEF)
            fKind |= KLDRSYMKIND_WEAK;
        switch (paSyms[iSym].n_type & MACHO_N_TYPE)
        {
            case MACHO_N_SECT:
            {
                PKLDRMODMACHOSECT pSect;
                if ((KU32)(paSyms[iSym].n_sect - 1) >= pModMachO->cSections)
                    return KLDR_ERR_MACHO_BAD_SYMBOL;
                pSect = &pModMachO->paSections[paSyms[iSym].n_sect - 1];

                uValue = paSyms[iSym].n_value - pModMachO->LinkAddress;
                if (uValue - pSect->RVA >= pSect->cb)
                    return KLDR_ERR_MACHO_BAD_SYMBOL;
                uValue += BaseAddress;

                if (pSect->fFlags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SELF_MODIFYING_CODE))
                    fKind |= KLDRSYMKIND_CODE;
                else
                    fKind |= KLDRSYMKIND_NO_TYPE;
                break;
            }

            case MACHO_N_ABS:
                uValue = paSyms[iSym].n_value;
                fKind |= KLDRSYMKIND_NO_TYPE /*KLDRSYMKIND_ABS*/;
                break;

            case MACHO_N_PBUD:
            case MACHO_N_INDR:
                /** @todo implement indirect and prebound symbols. */
            default:
                KLDRMODMACHO_ASSERT(0);
                return KLDR_ERR_TODO;
        }

        /*
         * Do callback.
         */
        rc = pfnCallback(pModMachO->pMod, iSym, psz, cch, NULL, uValue, fKind, pvUser);
        if (rc)
            return rc;
    }
    return 0;
}


/** @copydoc kLdrModGetImport */
static int kldrModMachOGetImport(PKLDRMOD pMod, const void *pvBits, KU32 iImport, char *pszName, KSIZE cchName)
{
    PKLDRMODMACHO pModMachO = (PKLDRMODMACHO)pMod->pvData;
    if (pModMachO->Hdr.filetype == MH_OBJECT)
        return KLDR_ERR_IMPORT_ORDINAL_OUT_OF_BOUNDS;

    /* later */
    return KLDR_ERR_IMPORT_ORDINAL_OUT_OF_BOUNDS;
}


/** @copydoc kLdrModNumberOfImports */
static KI32 kldrModMachONumberOfImports(PKLDRMOD pMod, const void *pvBits)
{
    PKLDRMODMACHO pModMachO = (PKLDRMODMACHO)pMod->pvData;
    if (pModMachO->Hdr.filetype == MH_OBJECT)
        return 0;

    /* later */
    return 0;
}


/** @copydoc kLdrModGetStackInfo */
static int kldrModMachOGetStackInfo(PKLDRMOD pMod, const void *pvBits, KLDRADDR BaseAddress, PKLDRSTACKINFO pStackInfo)
{
    /*PKLDRMODMACHO pModMachO = (PKLDRMODMACHO)pMod->pvData;*/

    pStackInfo->Address = NIL_KLDRADDR;
    pStackInfo->LinkAddress = NIL_KLDRADDR;
    pStackInfo->cbStack = pStackInfo->cbStackThread = 0;
    /* later */

    return 0;
}


/** @copydoc kLdrModQueryMainEntrypoint */
static int kldrModMachOQueryMainEntrypoint(PKLDRMOD pMod, const void *pvBits, KLDRADDR BaseAddress, PKLDRADDR pMainEPAddress)
{
#if 0
    PKLDRMODMACHO pModMachO = (PKLDRMODMACHO)pMod->pvData;
    int rc;

    /*
     * Resolve base address alias if any.
     */
    rc = kldrModMachOBitsAndBaseAddress(pModMachO, NULL, &BaseAddress);
    if (rc)
        return rc;

    /*
     * Convert the address from the header.
     */
    *pMainEPAddress = pModMachO->Hdrs.OptionalHeader.AddressOfEntryPoint
        ? BaseAddress + pModMachO->Hdrs.OptionalHeader.AddressOfEntryPoint
        : NIL_KLDRADDR;
#else
    *pMainEPAddress = NIL_KLDRADDR;
#endif
    return 0;
}


/** @copydoc kLdrModEnumDbgInfo */
static int kldrModMachOEnumDbgInfo(PKLDRMOD pMod, const void *pvBits, PFNKLDRENUMDBG pfnCallback, void *pvUser)
{
#if 0
    PKLDRMODMACHO                      pModMachO = (PKLDRMODMACHO)pMod->pvData;
    const IMAGE_DEBUG_DIRECTORY    *pDbgDir;
    KU32                            iDbgInfo;
    KU32                            cb;
    int                             rc;

    /*
     * Check that there is a debug directory first.
     */
    cb = pModMachO->Hdrs.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG].Size;
    if (    cb < sizeof(IMAGE_DEBUG_DIRECTORY) /* screw borland linkers */
        ||  !pModMachO->Hdrs.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG].VirtualAddress)
        return 0;

    /*
     * Make sure we've got mapped bits.
     */
    rc = kldrModMachOBitsAndBaseAddress(pModMachO, &pvBits, NULL);
    if (rc)
        return rc;

    /*
     * Enumerate the debug directory.
     */
    pDbgDir = KLDRMODMACHO_RVA2TYPE(pvBits,
                                 pModMachO->Hdrs.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG].VirtualAddress,
                                 const IMAGE_DEBUG_DIRECTORY *);
    for (iDbgInfo = 0;; iDbgInfo++, pDbgDir++, cb -= sizeof(IMAGE_DEBUG_DIRECTORY))
    {
        KLDRDBGINFOTYPE     enmDbgInfoType;

        /* convert the type. */
        switch (pDbgDir->Type)
        {
            case IMAGE_DEBUG_TYPE_UNKNOWN:
            case IMAGE_DEBUG_TYPE_FPO:
            case IMAGE_DEBUG_TYPE_COFF: /*stabs dialect??*/
            case IMAGE_DEBUG_TYPE_MISC:
            case IMAGE_DEBUG_TYPE_EXCEPTION:
            case IMAGE_DEBUG_TYPE_FIXUP:
            case IMAGE_DEBUG_TYPE_BORLAND:
            default:
                enmDbgInfoType = KLDRDBGINFOTYPE_UNKNOWN;
                break;
            case IMAGE_DEBUG_TYPE_CODEVIEW:
                enmDbgInfoType = KLDRDBGINFOTYPE_CODEVIEW;
                break;
        }

        rc = pfnCallback(pMod, iDbgInfo,
                         enmDbgInfoType, pDbgDir->MajorVersion, pDbgDir->MinorVersion,
                         pDbgDir->PointerToRawData ? pDbgDir->PointerToRawData : -1,
                         pDbgDir->AddressOfRawData ? pDbgDir->AddressOfRawData : NIL_KLDRADDR,
                         pDbgDir->SizeOfData,
                         NULL,
                         pvUser);
        if (rc)
            break;

        /* next */
        if (cb <= sizeof(IMAGE_DEBUG_DIRECTORY))
            break;
    }

    return rc;
#else
    return 0;
#endif
}


/** @copydoc kLdrModHasDbgInfo */
static int kldrModMachOHasDbgInfo(PKLDRMOD pMod, const void *pvBits)
{
    /*PKLDRMODMACHO pModMachO = (PKLDRMODMACHO)pMod->pvData;*/

#if 0
    /*
     * Base this entirely on the presence of a debug directory.
     */
    if (    pModMachO->Hdrs.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG].Size
            < sizeof(IMAGE_DEBUG_DIRECTORY) /* screw borland linkers */
        ||  !pModMachO->Hdrs.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG].VirtualAddress)
        return KLDR_ERR_NO_DEBUG_INFO;
    return 0;
#else
    return KLDR_ERR_NO_DEBUG_INFO;
#endif
}


/** @copydoc kLdrModMap */
static int kldrModMachOMap(PKLDRMOD pMod)
{
    PKLDRMODMACHO pModMachO = (PKLDRMODMACHO)pMod->pvData;
    unsigned fFixed;
    KU32 i;
    void *pvBase;
    int rc;

    /*
     * Already mapped?
     */
    if (pModMachO->pvMapping)
        return KLDR_ERR_ALREADY_MAPPED;

    /*
     * Map it.
     */
    /* fixed image? */
    fFixed = pMod->enmType == KLDRTYPE_EXECUTABLE_FIXED
          || pMod->enmType == KLDRTYPE_SHARED_LIBRARY_FIXED;
    if (!fFixed)
        pvBase = NULL;
    else
    {
        pvBase = (void *)(KUPTR)pMod->aSegments[0].LinkAddress;
        if ((KUPTR)pvBase != pMod->aSegments[0].LinkAddress)
            return KLDR_ERR_ADDRESS_OVERFLOW;
    }

    /* try do the prepare */
    if (pModMachO->fMapUsingLoadCommandSections)
        return KLDR_ERR_TODO; /* deal with this if it ever occurs. */
    else
    {
        rc = kRdrMap(pMod->pRdr, &pvBase, pMod->cSegments, pMod->aSegments, fFixed);
        if (rc)
            return rc;
    }

    /*
     * Update the segments with their map addresses.
     */
    for (i = 0; i < pMod->cSegments; i++)
    {
        if (pMod->aSegments[i].RVA != NIL_KLDRADDR)
            pMod->aSegments[i].MapAddress = (KUPTR)pvBase + (KUPTR)pMod->aSegments[i].RVA;
    }
    pModMachO->pvMapping = pvBase;

    return 0;
}


/** @copydoc kLdrModUnmap */
static int kldrModMachOUnmap(PKLDRMOD pMod)
{
    PKLDRMODMACHO pModMachO = (PKLDRMODMACHO)pMod->pvData;
    KU32 i;
    int rc;

    /*
     * Mapped?
     */
    if (!pModMachO->pvMapping)
        return KLDR_ERR_NOT_MAPPED;

    /*
     * Try unmap the image.
     */
    if (pModMachO->fMapUsingLoadCommandSections)
        return KLDR_ERR_TODO; /* deal with this if it ever occurs. */
    else
    {
        rc = kRdrUnmap(pMod->pRdr, pModMachO->pvMapping, pMod->cSegments, pMod->aSegments);
        if (rc)
            return rc;
    }

    /*
     * Update the segments to reflect that they aren't mapped any longer.
     */
    pModMachO->pvMapping = NULL;
    for (i = 0; i < pMod->cSegments; i++)
        pMod->aSegments[i].MapAddress = 0;

    return 0;
}


/** @copydoc kLdrModAllocTLS */
static int kldrModMachOAllocTLS(PKLDRMOD pMod)
{
    PKLDRMODMACHO pModMachO = (PKLDRMODMACHO)pMod->pvData;

    /*
     * Mapped?
     */
    if (!pModMachO->pvMapping)
        return KLDR_ERR_NOT_MAPPED;
    return 0;
}


/** @copydoc kLdrModFreeTLS */
static void kldrModMachOFreeTLS(PKLDRMOD pMod)
{
}


/** @copydoc kLdrModReload */
static int kldrModMachOReload(PKLDRMOD pMod)
{
    PKLDRMODMACHO pModMachO = (PKLDRMODMACHO)pMod->pvData;

    /*
     * Mapped?
     */
    if (!pModMachO->pvMapping)
        return KLDR_ERR_NOT_MAPPED;

    /* the file provider does it all */
    return kRdrRefresh(pMod->pRdr, pModMachO->pvMapping, pMod->cSegments, pMod->aSegments);
}


/** @copydoc kLdrModFixupMapping */
static int kldrModMachOFixupMapping(PKLDRMOD pMod, PFNKLDRMODGETIMPORT pfnGetImport, void *pvUser)
{
    PKLDRMODMACHO pModMachO = (PKLDRMODMACHO)pMod->pvData;
    int rc, rc2;

    /*
     * Mapped?
     */
    if (!pModMachO->pvMapping)
        return KLDR_ERR_NOT_MAPPED;

    /*
     * Before doing anything we'll have to make all pages writable.
     */
    if (pModMachO->fMapUsingLoadCommandSections)
        return KLDR_ERR_TODO; /* deal with this if it ever occurs. */
    else
    {
        rc = kRdrProtect(pMod->pRdr, pModMachO->pvMapping, pMod->cSegments, pMod->aSegments, 1 /* unprotect */);
        if (rc)
            return rc;
    }

    /*
     * Resolve imports and apply base relocations.
     */
    rc = kldrModMachORelocateBits(pMod, pModMachO->pvMapping, (KUPTR)pModMachO->pvMapping, pModMachO->LinkAddress,
                                  pfnGetImport, pvUser);

    /*
     * Restore protection.
     */
    if (pModMachO->fMapUsingLoadCommandSections)
        rc2 = KLDR_ERR_TODO; /* deal with this if it ever occurs. */
    else
        rc2 = kRdrProtect(pMod->pRdr, pModMachO->pvMapping, pMod->cSegments, pMod->aSegments, 0 /* protect */);
    if (!rc && rc2)
        rc = rc2;
    return rc;
}


/**
 * MH_OBJECT: Resolves undefined symbols (imports).
 *
 * @returns 0 on success, non-zero kLdr status code on failure.
 * @param   pModMachO       The Mach-O module interpreter instance.
 * @param   pfnGetImport    The callback for resolving an imported symbol.
 * @param   pvUser          User argument to the callback.
 */
static int  kldrModMachOObjDoImports(PKLDRMODMACHO pModMachO, PFNKLDRMODGETIMPORT pfnGetImport, void *pvUser)
{
    const KU32 cSyms = pModMachO->cSymbols;
    KU32 iSym;
    int rc;

    /*
     * Ensure that we've got the symbol table and section fixups handy.
     */
    rc = kldrModMachOLoadObjSymTab(pModMachO);
    if (rc)
        return rc;

    /*
     * Iterate the symbol table and resolve undefined symbols.
     * We currently ignore REFERENCE_TYPE.
     */
    if (    pModMachO->Hdr.magic == IMAGE_MACHO32_SIGNATURE
        ||  pModMachO->Hdr.magic == IMAGE_MACHO32_SIGNATURE_OE)
    {
        macho_nlist_32_t *paSyms = (macho_nlist_32_t *)pModMachO->pvaSymbols;
        for (iSym = 0; iSym < cSyms; iSym++)
        {
            /* skip stabs */
            if (paSyms[iSym].n_type & MACHO_N_STAB)
                continue;

            if ((paSyms[iSym].n_type & MACHO_N_TYPE) == MACHO_N_UNDF)
            {
                const char *pszSymbol;
                KSIZE cchSymbol;
                KU32 fKind = KLDRSYMKIND_REQ_FLAT;
                KLDRADDR Value;

                /** @todo Implement N_REF_TO_WEAK. */
                if (paSyms[iSym].n_desc & N_REF_TO_WEAK)
                    return KLDR_ERR_TODO;

                /* Get the symbol name and try resolve it. */
                if ((KU32)paSyms[iSym].n_un.n_strx >= pModMachO->cchStrings)
                    return KLDR_ERR_MACHO_BAD_SYMBOL;
                pszSymbol = &pModMachO->pchStrings[paSyms[iSym].n_un.n_strx];
                cchSymbol = kHlpStrLen(pszSymbol);
                rc = pfnGetImport(pModMachO->pMod, NIL_KLDRMOD_IMPORT, iSym, pszSymbol, cchSymbol, NULL,
                                  &Value, &fKind, pvUser);
                if (rc)
                {
                    /* weak reference? */
                    if (!(paSyms[iSym].n_desc & N_WEAK_REF))
                        break;
                    Value = 0;
                }

                /* Update the symbol. */
                paSyms[iSym].n_value = (KU32)Value;
                if (paSyms[iSym].n_value != Value)
                {
                    rc = KLDR_ERR_ADDRESS_OVERFLOW;
                    break;
                }
            }
            else if (paSyms[iSym].n_desc & N_WEAK_DEF)
            {
                /** @todo implement weak symbols. */
                /*return KLDR_ERR_TODO; - ignored for now. */
            }
        }
    }
    else
    {
        /* (Identical to the 32-bit code, just different paSym type. (and n_strx is unsigned)) */
        macho_nlist_64_t *paSyms = (macho_nlist_64_t *)pModMachO->pvaSymbols;
        for (iSym = 0; iSym < cSyms; iSym++)
        {
            /* skip stabs */
            if (paSyms[iSym].n_type & MACHO_N_STAB)
                continue;

            if ((paSyms[iSym].n_type & MACHO_N_TYPE) == MACHO_N_UNDF)
            {
                const char *pszSymbol;
                KSIZE cchSymbol;
                KU32 fKind = KLDRSYMKIND_REQ_FLAT;
                KLDRADDR Value;

                /** @todo Implement N_REF_TO_WEAK. */
                if (paSyms[iSym].n_desc & N_REF_TO_WEAK)
                    return KLDR_ERR_TODO;

                /* Get the symbol name and try resolve it. */
                if (paSyms[iSym].n_un.n_strx >= pModMachO->cchStrings)
                    return KLDR_ERR_MACHO_BAD_SYMBOL;
                pszSymbol = &pModMachO->pchStrings[paSyms[iSym].n_un.n_strx];
                cchSymbol = kHlpStrLen(pszSymbol);
                rc = pfnGetImport(pModMachO->pMod, NIL_KLDRMOD_IMPORT, iSym, pszSymbol, cchSymbol, NULL,
                                  &Value, &fKind, pvUser);
                if (rc)
                {
                    /* weak reference? */
                    if (!(paSyms[iSym].n_desc & N_WEAK_REF))
                        break;
                    Value = 0;
                }

                /* Update the symbol. */
                paSyms[iSym].n_value = Value;
                if (paSyms[iSym].n_value != Value)
                {
                    rc = KLDR_ERR_ADDRESS_OVERFLOW;
                    break;
                }
            }
            else if (paSyms[iSym].n_desc & N_WEAK_DEF)
            {
                /** @todo implement weak symbols. */
                /*return KLDR_ERR_TODO; - ignored for now. */
            }
        }
    }

    return rc;
}


/**
 * MH_OBJECT: Applies base relocations to a (unprotected) image mapping.
 *
 * @returns 0 on success, non-zero kLdr status code on failure.
 * @param   pModMachO       The Mach-O module interpreter instance.
 * @param   pvMapping       The mapping to fixup.
 * @param   NewBaseAddress  The address to fixup the mapping to.
 * @param   OldBaseAddress  The address the mapping is currently fixed up to.
 */
static int  kldrModMachOObjDoFixups(PKLDRMODMACHO pModMachO, void *pvMapping, KLDRADDR NewBaseAddress)
{
    KU32 iSeg;
    int rc;


    /*
     * Ensure that we've got the symbol table and section fixups handy.
     */
    rc = kldrModMachOLoadObjSymTab(pModMachO);
    if (rc)
        return rc;

    /*
     * Iterate over the segments and their sections and apply fixups.
     */
    for (iSeg = rc = 0; !rc && iSeg < pModMachO->pMod->cSegments; iSeg++)
    {
        PKLDRMODMACHOSEG pSeg = &pModMachO->aSegments[iSeg];
        KU32 iSect;

        for (iSect = 0; iSect < pSeg->cSections; iSect++)
        {
            PKLDRMODMACHOSECT pSect = &pSeg->paSections[iSect];
            KU8 *pbSectBits;

            /* skip sections without fixups. */
            if (!pSect->cFixups)
                continue;

            /* lazy load (and endian convert) the fixups. */
            if (!pSect->paFixups)
            {
                rc = kldrModMachOLoadFixups(pModMachO, pSect->offFixups, pSect->cFixups, &pSect->paFixups);
                if (rc)
                    break;
            }

            /*
             * Apply the fixups.
             */
            pbSectBits = (KU8 *)pvMapping + (KUPTR)pSect->RVA;
            if (pModMachO->Hdr.magic == IMAGE_MACHO32_SIGNATURE) /** @todo this aint right. */
                rc = kldrModMachOFixupSectionGeneric32Bit(pModMachO, pbSectBits, pSect,
                                                          (macho_nlist_32_t *)pModMachO->pvaSymbols,
                                                          pModMachO->cSymbols, NewBaseAddress);
            else
                rc = KLDR_ERR_TODO; /* save space for now. */
            if (rc)
                break;
        }
    }

    return rc;
}


/**
 * Applies generic fixups to a section in an image of the same endian-ness
 * as the host CPU.
 *
 * @returns 0 on success, non-zero kLdr status code on failure.
 * @param   pModMachO       The Mach-O module interpreter instance.
 * @param   pbSectBits      Pointer to the section bits.
 * @param   pFixupSect      The section being fixed up.
 * @param   NewBaseAddress  The new base image address.
 */
static int  kldrModMachOFixupSectionGeneric32Bit(PKLDRMODMACHO pModMachO, KU8 *pbSectBits, PKLDRMODMACHOSECT pFixupSect,
                                                 macho_nlist_32_t *paSyms, KU32 cSyms, KLDRADDR NewBaseAddress)
{
    const macho_relocation_info_t *paFixups = pFixupSect->paFixups;
    const KU32 cFixups = pFixupSect->cFixups;
    KSIZE cbSectBits = (KSIZE)pFixupSect->cb;
    const KU8 *pbSectVirginBits;
    KU32 iFixup;
    KLDRPU uFixVirgin;
    KLDRPU uFix;
    KLDRADDR SymAddr;
    int rc;

    /*
     * Find the virgin bits.
     */
    if (pFixupSect->offFile != -1)
    {
        rc = kldrModMachOMapVirginBits(pModMachO);
        if (rc)
            return rc;
        pbSectVirginBits = (const KU8 *)pModMachO->pvBits + pFixupSect->offFile;
    }
    else
        pbSectVirginBits = NULL;

    /*
     * Iterate the fixups and apply them.
     */
    for (iFixup = 0; iFixup < cFixups; iFixup++)
    {
        union
        {
            macho_relocation_info_t     r;
            scattered_relocation_info_t s;
        } Fixup;
        Fixup.r = paFixups[iFixup];

        if (!(Fixup.r.r_address & R_SCATTERED))
        {
            /* sanity */
            if ((KU32)Fixup.r.r_address >= cbSectBits)
                return KLDR_ERR_BAD_FIXUP;

            /* calc fixup addresses. */
            uFix.pv = pbSectBits + Fixup.r.r_address;
            uFixVirgin.pv = pbSectVirginBits ? (KU8 *)pbSectVirginBits + Fixup.r.r_address : 0;

            /*
             * Calc the symbol value.
             */
            /* Calc the linked symbol address / addend. */
            switch (Fixup.r.r_length)
            {
                /** @todo Deal with unaligned accesses on non x86 platforms. */
                case 0: SymAddr = *uFixVirgin.pi8; break;
                case 1: SymAddr = *uFixVirgin.pi16; break;
                case 2: SymAddr = *uFixVirgin.pi32; break;
                case 3: SymAddr = *uFixVirgin.pi64; break;
            }
            if (Fixup.r.r_pcrel)
                SymAddr += Fixup.r.r_address + pFixupSect->LinkAddress;

            /* Add symbol / section address. */
            if (Fixup.r.r_extern)
            {
                const macho_nlist_32_t *pSym;
                if (Fixup.r.r_symbolnum >= cSyms)
                    return KLDR_ERR_BAD_FIXUP;
                pSym = &paSyms[Fixup.r.r_symbolnum];

                if (pSym->n_type & MACHO_N_STAB)
                    return KLDR_ERR_BAD_FIXUP;

                switch (pSym->n_type & MACHO_N_TYPE)
                {
                    case MACHO_N_SECT:
                    {
                        PKLDRMODMACHOSECT pSymSect;
                        if ((KU32)pSym->n_sect - 1 > pModMachO->cSections)
                            return KLDR_ERR_MACHO_BAD_SYMBOL;
                        pSymSect = &pModMachO->paSections[pSym->n_sect - 1];

                        SymAddr += pSym->n_value - pSymSect->LinkAddress + pSymSect->RVA + NewBaseAddress;
                        break;
                    }

                    case MACHO_N_UNDF:
                    case MACHO_N_ABS:
                        SymAddr += pSym->n_value;
                        break;

                    case MACHO_N_INDR:
                    case MACHO_N_PBUD:
                        return KLDR_ERR_TODO;
                    default:
                        return KLDR_ERR_MACHO_BAD_SYMBOL;
                }
            }
            else if (Fixup.r.r_symbolnum != R_ABS)
            {
                PKLDRMODMACHOSECT pSymSect;
                if (Fixup.r.r_symbolnum > pModMachO->cSections)
                    return KLDR_ERR_BAD_FIXUP;
                pSymSect = &pModMachO->paSections[Fixup.r.r_symbolnum - 1];

                SymAddr -= pSymSect->LinkAddress;
                SymAddr += pSymSect->RVA + NewBaseAddress;
            }

            /* adjust for PC relative */
            if (Fixup.r.r_pcrel)
                SymAddr -= Fixup.r.r_address + pFixupSect->RVA + NewBaseAddress;
        }
        else
        {
            PKLDRMODMACHOSECT pSymSect;
            KU32 iSymSect;
            KLDRADDR Value;

            /* sanity */
            KLDRMODMACHO_ASSERT(Fixup.s.r_scattered);
            if ((KU32)Fixup.s.r_address >= cbSectBits)
                return KLDR_ERR_BAD_FIXUP;

            /* calc fixup addresses. */
            uFix.pv = pbSectBits + Fixup.s.r_address;
            uFixVirgin.pv = pbSectVirginBits ? (KU8 *)pbSectVirginBits + Fixup.s.r_address : 0;

            /*
             * Calc the symbol value.
             */
            /* The addend is stored in the code. */
            switch (Fixup.s.r_length)
            {
                case 0: SymAddr = *uFixVirgin.pi8; break;
                case 1: SymAddr = *uFixVirgin.pi16; break;
                case 2: SymAddr = *uFixVirgin.pi32; break;
                case 3: SymAddr = *uFixVirgin.pi64; break;
            }
            if (Fixup.s.r_pcrel)
                SymAddr += Fixup.s.r_address;
            Value = Fixup.s.r_value;
            SymAddr -= Value;                   /* (-> addend only) */

            /* Find the section number from the r_value. */
            pSymSect = NULL;
            for (iSymSect = 0; iSymSect < pModMachO->cSections; iSymSect++)
            {
                KLDRADDR off = Value - pModMachO->paSections[iSymSect].LinkAddress;
                if (off < pModMachO->paSections[iSymSect].cb)
                {
                    pSymSect = &pModMachO->paSections[iSymSect];
                    break;
                }
                else if (off == pModMachO->paSections[iSymSect].cb) /* edge case */
                    pSymSect = &pModMachO->paSections[iSymSect];
            }
            if (!pSymSect)
                return KLDR_ERR_BAD_FIXUP;

            /* Calc the symbol address. */
            SymAddr += Value - pSymSect->LinkAddress + pSymSect->RVA + NewBaseAddress;
            if (Fixup.s.r_pcrel)
                SymAddr -= Fixup.s.r_address + pFixupSect->RVA + NewBaseAddress;

            Fixup.r.r_length = ((scattered_relocation_info_t *)&paFixups[iFixup])->r_length;
            Fixup.r.r_type   = ((scattered_relocation_info_t *)&paFixups[iFixup])->r_type;
        }

        /*
         * Write back the fixed up value.
         */
        if (Fixup.r.r_type == GENERIC_RELOC_VANILLA)
        {
            switch (Fixup.r.r_length)
            {
                case 0: *uFix.pu8  = (KU8)SymAddr; break;
                case 1: *uFix.pu16 = (KU16)SymAddr; break;
                case 2: *uFix.pu32 = (KU32)SymAddr; break;
                case 3: *uFix.pu64 = (KU64)SymAddr; break;
            }
        }
        else if (Fixup.r.r_type <= GENERIC_RELOC_LOCAL_SECTDIFF)
            return KLDR_ERR_MACHO_UNSUPPORTED_FIXUP_TYPE;
        else
            return KLDR_ERR_BAD_FIXUP;
    }

    return 0;
}


/**
 * Loads the symbol table for a MH_OBJECT file.
 *
 * The symbol table is pointed to by KLDRMODMACHO::pvaSymbols.
 *
 * @returns 0 on success, non-zero kLdr status code on failure.
 * @param   pModMachO       The Mach-O module interpreter instance.
 */
static int  kldrModMachOLoadObjSymTab(PKLDRMODMACHO pModMachO)
{
    int rc = 0;

    if (    !pModMachO->pvaSymbols
        &&  pModMachO->cSymbols)
    {
        KSIZE cbSyms;
        KSIZE cbSym;
        void *pvSyms;
        void *pvStrings;

        /* sanity */
        if (    !pModMachO->offSymbols
            ||  (pModMachO->cchStrings && !pModMachO->offStrings))
            return KLDR_ERR_MACHO_BAD_OBJECT_FILE;

        /* allocate */
        cbSym = pModMachO->Hdr.magic == IMAGE_MACHO32_SIGNATURE
             || pModMachO->Hdr.magic == IMAGE_MACHO32_SIGNATURE_OE
             ? sizeof(macho_nlist_32_t)
             : sizeof(macho_nlist_64_t);
        cbSyms = pModMachO->cSymbols * cbSym;
        if (cbSyms / cbSym != pModMachO->cSymbols)
            return KLDR_ERR_SIZE_OVERFLOW;
        rc = KERR_NO_MEMORY;
        pvSyms = kHlpAlloc(cbSyms);
        if (pvSyms)
        {
            if (pModMachO->cchStrings)
                pvStrings = kHlpAlloc(pModMachO->cchStrings);
            else
                pvStrings = kHlpAllocZ(4);
            if (pvStrings)
            {
                /* read */
                rc = kRdrRead(pModMachO->pMod->pRdr, pvSyms, cbSyms, pModMachO->offSymbols);
                if (!rc && pModMachO->cchStrings)
                    rc = kRdrRead(pModMachO->pMod->pRdr, pvStrings, pModMachO->cchStrings, pModMachO->offStrings);
                if (!rc)
                {
                    pModMachO->pvaSymbols = pvSyms;
                    pModMachO->pchStrings = (char *)pvStrings;

                    /* perform endian conversion? */
                    if (pModMachO->Hdr.magic == IMAGE_MACHO32_SIGNATURE_OE)
                    {
                        KU32 cLeft = pModMachO->cSymbols;
                        macho_nlist_32_t *pSym = (macho_nlist_32_t *)pvSyms;
                        while (cLeft-- > 0)
                        {
                            pSym->n_un.n_strx = K_E2E_U32(pSym->n_un.n_strx);
                            pSym->n_desc = (KI16)K_E2E_U16(pSym->n_desc);
                            pSym->n_value = K_E2E_U32(pSym->n_value);
                            pSym++;
                        }
                    }
                    else if (pModMachO->Hdr.magic == IMAGE_MACHO64_SIGNATURE_OE)
                    {
                        KU32 cLeft = pModMachO->cSymbols;
                        macho_nlist_64_t *pSym = (macho_nlist_64_t *)pvSyms;
                        while (cLeft-- > 0)
                        {
                            pSym->n_un.n_strx = K_E2E_U32(pSym->n_un.n_strx);
                            pSym->n_desc = (KI16)K_E2E_U16(pSym->n_desc);
                            pSym->n_value = K_E2E_U64(pSym->n_value);
                            pSym++;
                        }
                    }

                    return 0;
                }
                kHlpFree(pvStrings);
            }
            kHlpFree(pvSyms);
        }
    }
    else
        KLDRMODMACHO_ASSERT(pModMachO->pchStrings);

    return rc;
}


/**
 * Loads the fixups at the given address and performs endian
 * conversion if necessary.
 *
 * @returns 0 on success, non-zero kLdr status code on failure.
 * @param   pModMachO       The Mach-O module interpreter instance.
 * @param   offFixups       The file offset of the fixups.
 * @param   cFixups         The number of fixups to load.
 * @param   ppaFixups       Where to put the pointer to the allocated fixup array.
 */
static int  kldrModMachOLoadFixups(PKLDRMODMACHO pModMachO, KLDRFOFF offFixups, KU32 cFixups, macho_relocation_info_t **ppaFixups)
{
    macho_relocation_info_t *paFixups;
    KSIZE cbFixups;
    int rc;

    /* allocate the memory. */
    cbFixups = cFixups * sizeof(*paFixups);
    if (cbFixups / sizeof(*paFixups) != cFixups)
        return KLDR_ERR_SIZE_OVERFLOW;
    paFixups = (macho_relocation_info_t *)kHlpAlloc(cbFixups);
    if (!paFixups)
        return KERR_NO_MEMORY;

    /* read the fixups. */
    rc = kRdrRead(pModMachO->pMod->pRdr, paFixups, cbFixups, offFixups);
    if (!rc)
    {
        *ppaFixups = paFixups;

        /* do endian conversion if necessary. */
        if (    pModMachO->Hdr.magic == IMAGE_MACHO32_SIGNATURE_OE
            ||  pModMachO->Hdr.magic == IMAGE_MACHO64_SIGNATURE_OE)
        {
            KU32 iFixup;
            for (iFixup = 0; iFixup < cFixups; iFixup++)
            {
                KU32 *pu32 = (KU32 *)&paFixups[iFixup];
                pu32[0] = K_E2E_U32(pu32[0]);
                pu32[1] = K_E2E_U32(pu32[1]);
            }
        }
    }
    else
        kHlpFree(paFixups);
    return rc;
}


/**
 * Maps the virgin file bits into memory if not already done.
 *
 * @returns 0 on success, non-zero kLdr status code on failure.
 * @param   pModMachO       The Mach-O module interpreter instance.
 */
static int kldrModMachOMapVirginBits(PKLDRMODMACHO pModMachO)
{
    int rc = 0;
    if (!pModMachO->pvBits)
        rc = kRdrAllMap(pModMachO->pMod->pRdr, &pModMachO->pvBits);
    return rc;
}


/** @copydoc kLdrModCallInit */
static int kldrModMachOCallInit(PKLDRMOD pMod, KUPTR uHandle)
{
    /* later */
    return 0;
}


/** @copydoc kLdrModCallTerm */
static int kldrModMachOCallTerm(PKLDRMOD pMod, KUPTR uHandle)
{
    /* later */
    return 0;
}


/** @copydoc kLdrModCallThread */
static int kldrModMachOCallThread(PKLDRMOD pMod, KUPTR uHandle, unsigned fAttachingOrDetaching)
{
    /* Relevant for Mach-O? */
    return 0;
}


/** @copydoc kLdrModSize */
static KLDRADDR kldrModMachOSize(PKLDRMOD pMod)
{
    PKLDRMODMACHO pModMachO = (PKLDRMODMACHO)pMod->pvData;
    return pModMachO->cbImage;
}


/** @copydoc kLdrModGetBits */
static int kldrModMachOGetBits(PKLDRMOD pMod, void *pvBits, KLDRADDR BaseAddress, PFNKLDRMODGETIMPORT pfnGetImport, void *pvUser)
{
    PKLDRMODMACHO  pModMachO = (PKLDRMODMACHO)pMod->pvData;
    KU32        i;
    int         rc;

    /*
     * Zero the entire buffer first to simplify things.
     */
    kHlpMemSet(pvBits, 0, (KSIZE)pModMachO->cbImage);

    /*
     * When possible use the segment table to load the data.
     * If not iterate the load commands and execute the segment / section loads.
     */
    if (pModMachO->fMapUsingLoadCommandSections)
        return KLDR_ERR_TODO; /* deal with this if it ever occurs. */
    else
    {
        for (i = 0; i < pMod->cSegments; i++)
        {
            /* skip it? */
            if (    pMod->aSegments[i].cbFile == -1
                ||  pMod->aSegments[i].offFile == -1
                ||  pMod->aSegments[i].LinkAddress == NIL_KLDRADDR
                ||  !pMod->aSegments[i].Alignment)
                continue;
            rc = kRdrRead(pMod->pRdr,
                             (KU8 *)pvBits + (pMod->aSegments[i].LinkAddress - pModMachO->LinkAddress),
                             pMod->aSegments[i].cbFile,
                             pMod->aSegments[i].offFile);
            if (rc)
                return rc;
        }
    }

    /*
     * Perform relocations.
     */
    return kldrModMachORelocateBits(pMod, pvBits, BaseAddress, pModMachO->LinkAddress, pfnGetImport, pvUser);
}


/** @copydoc kLdrModRelocateBits */
static int kldrModMachORelocateBits(PKLDRMOD pMod, void *pvBits, KLDRADDR NewBaseAddress, KLDRADDR OldBaseAddress,
                                    PFNKLDRMODGETIMPORT pfnGetImport, void *pvUser)
{
    PKLDRMODMACHO pModMachO = (PKLDRMODMACHO)pMod->pvData;
    int rc;

    /*
     * Call workers to do the jobs.
     */
    if (pModMachO->Hdr.filetype == MH_OBJECT)
    {
        rc = kldrModMachOObjDoImports(pModMachO, pfnGetImport, pvUser);
        if (!rc)
            rc = kldrModMachOObjDoFixups(pModMachO, pvBits, NewBaseAddress);

    }
    else
        rc = KLDR_ERR_TODO;
    /*{
        rc = kldrModMachODoFixups(pModMachO, pvBits, NewBaseAddress, OldBaseAddress, pfnGetImport, pvUser);
        if (!rc)
            rc = kldrModMachODoImports(pModMachO, pvBits, pfnGetImport, pvUser);
    }*/

    return rc;
}


/**
 * The Mach-O module interpreter method table.
 */
KLDRMODOPS g_kLdrModMachOOps =
{
    "Mach-O",
    NULL,
    kldrModMachOCreate,
    kldrModMachODestroy,
    kldrModMachOQuerySymbol,
    kldrModMachOEnumSymbols,
    kldrModMachOGetImport,
    kldrModMachONumberOfImports,
    NULL /* can execute one is optional */,
    kldrModMachOGetStackInfo,
    kldrModMachOQueryMainEntrypoint,
    NULL,
    NULL,
    kldrModMachOEnumDbgInfo,
    kldrModMachOHasDbgInfo,
    kldrModMachOMap,
    kldrModMachOUnmap,
    kldrModMachOAllocTLS,
    kldrModMachOFreeTLS,
    kldrModMachOReload,
    kldrModMachOFixupMapping,
    kldrModMachOCallInit,
    kldrModMachOCallTerm,
    kldrModMachOCallThread,
    kldrModMachOSize,
    kldrModMachOGetBits,
    kldrModMachORelocateBits,
    NULL, /** @todo mostly done */
    42 /* the end */
};
