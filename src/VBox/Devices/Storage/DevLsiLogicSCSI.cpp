/* $Id$ */
/** @file
 * VBox storage devices: LsiLogic LSI53c1030 SCSI controller.
 */

/*
 * Copyright (C) 2006-2009 Sun Microsystems, Inc.
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

//#define DEBUG
#define LOG_GROUP LOG_GROUP_DEV_LSILOGICSCSI
#include <VBox/pdmdev.h>
#include <VBox/pdmqueue.h>
#include <VBox/pdmcritsect.h>
#include <VBox/scsi.h>
#include <iprt/assert.h>
#include <iprt/asm.h>
#include <iprt/string.h>
#ifdef IN_RING3
# include <iprt/param.h>
# include <iprt/alloc.h>
# include <iprt/cache.h>
#endif

#include "DevLsiLogicSCSI.h"
#include "VBoxSCSI.h"

#include "../Builtins.h"

/**
 * Reply data.
 */
typedef struct LSILOGICSCSIREPLY
{
    /** Lower 32 bits of the reply address in memory. */
    uint32_t      u32HostMFALowAddress;
    /** Full address of the reply in guest memory. */
    RTGCPHYS      GCPhysReplyAddress;
    /** Size of the reply. */
    uint32_t      cbReply;
    /** Different views to the reply depending on the request type. */
    MptReplyUnion Reply;
} LSILOGICSCSIREPLY, *PLSILOGICSCSIREPLY;

/*
 * State of a device attached to the buslogic host adapter.
 */
typedef struct LSILOGICDEVICE
{
    /** Pointer to the owning lsilogic device instance. - R3 pointer */
    R3PTRTYPE(struct LSILOGICSCSI *)  pLsiLogicR3;
    /** Pointer to the owning lsilogic device instance. - R0 pointer */
    R0PTRTYPE(struct LSILOGICSCSI *)  pLsiLogicR0;
    /** Pointer to the owning lsilogic device instance. - RC pointer */
    RCPTRTYPE(struct LSILOGICSCSI *)  pLsiLogicRC;

    /** LUN of the device. */
    RTUINT                        iLUN;
    /** Number of outstanding tasks on the port. */
    volatile uint32_t             cOutstandingRequests;

#if HC_ARCH_BITS == 64
    uint32_t                      Alignment0;
#endif

    /** Our base interace. */
    PDMIBASE                      IBase;
    /** SCSI port interface. */
    PDMISCSIPORT                  ISCSIPort;
    /** Led interface. */
    PDMILEDPORTS                  ILed;
    /** Pointer to the attached driver's base interface. */
    R3PTRTYPE(PPDMIBASE)          pDrvBase;
    /** Pointer to the underlying SCSI connector interface. */
    R3PTRTYPE(PPDMISCSICONNECTOR) pDrvSCSIConnector;
    /** The status LED state for this device. */
    PDMLED                        Led;

} LSILOGICDEVICE, *PLSILOGICDEVICE;

/**
 * Defined states that the SCSI controller can have.
 */
typedef enum LSILOGICSTATE
{
    /** Reset state. */
    LSILOGICSTATE_RESET       = 0x00,
    /** Ready state. */
    LSILOGICSTATE_READY       = 0x01,
    /** Operational state. */
    LSILOGICSTATE_OPERATIONAL = 0x02,
    /** Fault state. */
    LSILOGICSTATE_FAULT       = 0x04,
    /** 32bit size hack */
    LSILOGICSTATE_32BIT_HACK  = 0x7fffffff
} LSILOGICSTATE;

/**
 * Which entity needs to initialize the controller
 * to get into the operational state.
 */
typedef enum LSILOGICWHOINIT
{
    /** Not initialized. */
    LSILOGICWHOINIT_NOT_INITIALIZED = 0x00,
    /** System BIOS. */
    LSILOGICWHOINIT_SYSTEM_BIOS     = 0x01,
    /** ROM Bios. */
    LSILOGICWHOINIT_ROM_BIOS        = 0x02,
    /** PCI Peer. */
    LSILOGICWHOINIT_PCI_PEER        = 0x03,
    /** Host driver. */
    LSILOGICWHOINIT_HOST_DRIVER     = 0x04,
    /** Manufacturing. */
    LSILOGICWHOINIT_MANUFACTURING   = 0x05,
    /** 32bit size hack. */
    LSILOGICWHOINIT_32BIT_HACK      = 0x7fffffff
} LSILOGICWHOINIT;


/**
 * IOC status codes.
 */
#define LSILOGIC_IOCSTATUS_SUCCESS                0x0000
#define LSILOGIC_IOCSTATUS_INVALID_FUNCTION       0x0001
#define LSILOGIC_IOCSTATUS_BUSY                   0x0002
#define LSILOGIC_IOCSTATUS_INVALID_SGL            0x0003
#define LSILOGIC_IOCSTATUS_INTERNAL_ERROR         0x0004
#define LSILOGIC_IOCSTATUS_RESERVED               0x0005
#define LSILOGIC_IOCSTATUS_INSUFFICIENT_RESOURCES 0x0006
#define LSILOGIC_IOCSTATUS_INVALID_FIELD          0x0007
#define LSILOGIC_IOCSTATUS_INVALID_STATE          0x0008
#define LSILOGIC_IOCSTATUS_OP_STATE_NOT_SUPPOTED  0x0009

/**
 * Device instance data for the emulated
 * SCSI controller.
 */
typedef struct LSILOGICSCSI
{
    /** PCI device structure. */
    PCIDEVICE            PciDev;
    /** Pointer to the device instance. - R3 ptr. */
    PPDMDEVINSR3         pDevInsR3;
    /** Pointer to the device instance. - R0 ptr. */
    PPDMDEVINSR0         pDevInsR0;
    /** Pointer to the device instance. - RC ptr. */
    PPDMDEVINSRC         pDevInsRC;

    /** Flag whether the GC part of the device is enabled. */
    bool                 fGCEnabled;
    /** Flag whether the R0 part of the device is enabled. */
    bool                 fR0Enabled;

    /** The state the controller is currently in. */
    LSILOGICSTATE        enmState;
    /** Who needs to init the driver to get into operational state. */
    LSILOGICWHOINIT      enmWhoInit;
    /** Flag whether we are in doorbell function. */
    bool                 fDoorbellInProgress;
    /** Flag whether diagnostic access is enabled. */
    bool                 fDiagnosticEnabled;

    /** Flag whether a notification was send to R3. */
    bool                 fNotificationSend;

    /** Flag whether the guest enabled event notification from the IOC. */
    bool                 fEventNotificationEnabled;

#if HC_ARCH_BITS == 64
    uint32_t             Alignment0;
#endif

    /** Queue to send tasks to R3. - R3 ptr */
    R3PTRTYPE(PPDMQUEUE) pNotificationQueueR3;
    /** Queue to send tasks to R3. - R0 ptr */
    R0PTRTYPE(PPDMQUEUE) pNotificationQueueR0;
    /** Queue to send tasks to R3. - RC ptr */
    RCPTRTYPE(PPDMQUEUE) pNotificationQueueRC;

#if HC_ARCH_BITS == 64
    uint32_t             Alignment1;
#endif

    /** States for attached devices. */
    LSILOGICDEVICE       aDeviceStates[LSILOGIC_DEVICES_MAX];

    /** MMIO address the device is mapped to. */
    RTGCPHYS             GCPhysMMIOBase;
    /** I/O port address the device is mapped to. */
    RTIOPORT             IOPortBase;

    /** Interrupt mask. */
    volatile uint32_t    uInterruptMask;
    /** Interrupt status register. */
    volatile uint32_t    uInterruptStatus;

    /** Buffer for messages which are passed
     * through the doorbell using the
     * handshake method. */
    uint32_t              aMessage[sizeof(MptConfigurationRequest)];
    /** Actual position in the buffer. */
    uint32_t              iMessage;
    /** Size of the message which is given in the doorbell message in dwords. */
    uint32_t              cMessage;

    /** Reply buffer. */
    MptReplyUnion         ReplyBuffer;
    /** Next entry to read. */
    uint32_t              uNextReplyEntryRead;
    /** Size of the reply in the buffer in 16bit words. */
    uint32_t              cReplySize;

    /** The fault code of the I/O controller if we are in the fault state. */
    uint16_t              u16IOCFaultCode;

    /** Upper 32 bits of the moessage frame address to locate requests in guest memory. */
    uint32_t              u32HostMFAHighAddr;
    /** Upper 32 bits of the sense buffer address. */
    uint32_t              u32SenseBufferHighAddr;
    /** Maximum number of devices the driver reported he can handle. */
    uint8_t               cMaxDevices;
    /** Maximum number of buses the driver reported he can handle. */
    uint8_t               cMaxBuses;
    /** Current size of reply message frames in the guest. */
    uint16_t              cbReplyFrame;

    /** Next key to write in the sequence to get access
     *  to diagnostic memory. */
    uint32_t              iDiagnosticAccess;

    /** Number entries allocated for the reply queue. */
    uint32_t              cReplyQueueEntries;
    /** Number entries allocated for the outstanding request queue. */
    uint32_t              cRequestQueueEntries;

    uint32_t              Alignment2;

    /** Critical section protecting the reply post queue. */
    PDMCRITSECT           ReplyPostQueueCritSect;
    /** Critical section protecting the reply free queue. */
    PDMCRITSECT           ReplyFreeQueueCritSect;

    /** Pointer to the start of the reply free queue - R3. */
    R3PTRTYPE(volatile uint32_t *) pReplyFreeQueueBaseR3;
    /** Pointer to the start of the reply post queue - R3. */
    R3PTRTYPE(volatile uint32_t *) pReplyPostQueueBaseR3;
    /** Pointer to the start of the request queue - R3. */
    R3PTRTYPE(volatile uint32_t *) pRequestQueueBaseR3;

    /** Pointer to the start of the reply queue - R0. */
    R0PTRTYPE(volatile uint32_t *) pReplyFreeQueueBaseR0;
    /** Pointer to the start of the reply queue - R0. */
    R0PTRTYPE(volatile uint32_t *) pReplyPostQueueBaseR0;
    /** Pointer to the start of the request queue - R0. */
    R0PTRTYPE(volatile uint32_t *) pRequestQueueBaseR0;

    /** Pointer to the start of the reply queue - RC. */
    RCPTRTYPE(volatile uint32_t *) pReplyFreeQueueBaseRC;
    /** Pointer to the start of the reply queue - RC. */
    RCPTRTYPE(volatile uint32_t *) pReplyPostQueueBaseRC;
    /** Pointer to the start of the request queue - RC. */
    RCPTRTYPE(volatile uint32_t *) pRequestQueueBaseRC;

    /** Next free entry in the reply queue the guest can write a address to. */
    volatile uint32_t     uReplyFreeQueueNextEntryFreeWrite;
    /** Next valid entry the controller can read a valid address for reply frames from. */
    volatile uint32_t     uReplyFreeQueueNextAddressRead;

    /** Next free entry in the reply queue the guest can write a address to. */
    volatile uint32_t     uReplyPostQueueNextEntryFreeWrite;
    /** Next valid entry the controller can read a valid address for reply frames from. */
    volatile uint32_t     uReplyPostQueueNextAddressRead;

    /** Next free entry the guest can write a address to a request frame to. */
    volatile uint32_t     uRequestQueueNextEntryFreeWrite;
    /** Next valid entry the controller can read a valid address for request frames from. */
    volatile uint32_t     uRequestQueueNextAddressRead;

    /* Emulated controller type */
    LSILOGICCTRLTYPE      enmCtrlType;

    /** Configuration pages. */
    union
    {
        /** Pages for the SPI controller. */
        MptConfigurationPagesSupportedSpi SpiPages;
        /** Pages for the SAS controller. */
        MptConfigurationPagesSupportedSas SasPages;
    } ConfigurationPages;

    uint32_t                       u32Alignment2;

    /** BIOS emulation. */
    VBOXSCSI                       VBoxSCSI;

    /** Cache for allocated tasks. */
    R3PTRTYPE(PRTOBJCACHE)         pTaskCache;

    /** The base interface */
    PDMIBASE                       IBase;
    /** Status Port - Leds interface. */
    PDMILEDPORTS                   ILeds;
    /** Partner of ILeds. */
    R3PTRTYPE(PPDMILEDCONNECTORS)  pLedsConnector;
} LSILOGISCSI, *PLSILOGICSCSI;

#define LSILOGIC_PCI_SPACE_IO_SIZE  256
#define LSILOGIC_PCI_SPACE_MEM_SIZE 128 * _1K

#define LSILOGIC_REG_DOORBELL 0x00
# define LSILOGIC_REG_DOORBELL_SET_STATE(enmState)     (((enmState) & 0x0f) << 28)
# define LSILOGIC_REG_DOORBELL_SET_USED(fUsed)         (((fUsed) ? 1 : 0) << 27)
# define LSILOGIC_REG_DOORBELL_SET_WHOINIT(enmWhoInit) (((enmWhoInit) & 0x07) << 24)
# define LSILOGIC_REG_DOORBELL_SET_FAULT_CODE(u16Code) (u16Code)
# define LSILOGIC_REG_DOORBELL_GET_FUNCTION(x)         (((x) & 0xff000000) >> 24)
# define LSILOGIC_REG_DOORBELL_GET_SIZE(x)             (((x) & 0x00ff0000) >> 16)

#define LSILOGIC_REG_WRITE_SEQUENCE    0x04

#define LSILOGIC_REG_HOST_DIAGNOSTIC   0x08
# define LSILOGIC_REG_HOST_DIAGNOSTIC_DIAG_MEM_ENABLE     (RT_BIT(0))
# define LSILOGIC_REG_HOST_DIAGNOSTIC_DISABLE_ARM         (RT_BIT(1))
# define LSILOGIC_REG_HOST_DIAGNOSTIC_RESET_ADAPTER       (RT_BIT(2))
# define LSILOGIC_REG_HOST_DIAGNOSTIC_DIAG_RW_ENABLE      (RT_BIT(4))
# define LSILOGIC_REG_HOST_DIAGNOSTIC_RESET_HISTORY       (RT_BIT(5))
# define LSILOGIC_REG_HOST_DIAGNOSTIC_FLASH_BAD_SIG       (RT_BIT(6))
# define LSILOGIC_REG_HOST_DIAGNOSTIC_DRWE                (RT_BIT(7))
# define LSILOGIC_REG_HOST_DIAGNOSTIC_PREVENT_IOC_BOOT    (RT_BIT(9))
# define LSILOGIC_REG_HOST_DIAGNOSTIC_CLEAR_FLASH_BAD_SIG (RT_BIT(10))

#define LSILOGIC_REG_TEST_BASE_ADDRESS 0x0c
#define LSILOGIC_REG_DIAG_RW_DATA      0x10
#define LSILOGIC_REG_DIAG_RW_ADDRESS   0x14

#define LSILOGIC_REG_HOST_INTR_STATUS  0x30
# define LSILOGIC_REG_HOST_INTR_STATUS_W_MASK (RT_BIT(3))
# define LSILOGIC_REG_HOST_INTR_STATUS_DOORBELL_STS    (RT_BIT(31))
# define LSILOGIC_REG_HOST_INTR_STATUS_REPLY_INTR      (RT_BIT(3))
# define LSILOGIC_REG_HOST_INTR_STATUS_SYSTEM_DOORBELL (RT_BIT(0))

#define LSILOGIC_REG_HOST_INTR_MASK    0x34
# define LSILOGIC_REG_HOST_INTR_MASK_W_MASK (RT_BIT(0) | RT_BIT(3) | RT_BIT(8) | RT_BIT(9))
# define LSILOGIC_REG_HOST_INTR_MASK_IRQ_ROUTING (RT_BIT(8) | RT_BIT(9))
# define LSILOGIC_REG_HOST_INTR_MASK_DOORBELL RT_BIT(0)
# define LSILOGIC_REG_HOST_INTR_MASK_REPLY    RT_BIT(3)

#define LSILOGIC_REG_REQUEST_QUEUE     0x40
#define LSILOGIC_REG_REPLY_QUEUE       0x44

/* Functions which can be passed through the system doorbell. */
#define LSILOGIC_DOORBELL_FUNCTION_IOC_MSG_UNIT_RESET  0x40
#define LSILOGIC_DOORBELL_FUNCTION_IO_UNIT_RESET       0x41
#define LSILOGIC_DOORBELL_FUNCTION_HANDSHAKE           0x42
#define LSILOGIC_DOORBELL_FUNCTION_REPLY_FRAME_REMOVAL 0x43

/**
 * Scatter gather list entry data.
 */
typedef struct LSILOGICTASKSTATESGENTRY
{
    /** Flag whether the buffer in the list is from the guest or an
     *  allocated temporary buffer because the segments in the guest
     *  are not sector aligned.
     */
    bool     fGuestMemory;
    /** Flag whether the buffer contains data or is the destination for the transfer. */
    bool     fBufferContainsData;
    /** Pointer to the start of the buffer. */
    void    *pvBuf;
    /** Size of the buffer. */
    uint32_t cbBuf;
    /** Flag dependent data. */
    union
    {
        /** Data to handle direct mappings of guest buffers. */
        PGMPAGEMAPLOCK  PageLock;
        /** The segment in the guest which is not sector aligned. */
        RTGCPHYS        GCPhysAddrBufferUnaligned;
    } u;
} LSILOGICTASKSTATESGENTRY, *PLSILOGICTASKSTATESGENTRY;

/**
 * Task state object which holds all neccessary data while
 * processing the request from the guest.
 */
typedef struct LSILOGICTASKSTATE
{
    /** Target device. */
    PLSILOGICDEVICE            pTargetDevice;
    /** The message request from the guest. */
    MptRequestUnion            GuestRequest;
    /** Reply message if the request produces one. */
    MptReplyUnion              IOCReply;
    /** SCSI request structure for the SCSI driver. */
    PDMSCSIREQUEST             PDMScsiRequest;
    /** Address of the message request frame in guests memory.
     *  Used to read the S/G entries in the second step. */
    RTGCPHYS                   GCPhysMessageFrameAddr;
    /** Number of scatter gather list entries. */
    uint32_t                   cSGListEntries;
    /** How many entries would fit into the sg list. */
    uint32_t                   cSGListSize;
    /** How many times the list was too big. */
    uint32_t                   cSGListTooBig;
    /** Pointer to the first entry of the scatter gather list. */
    PPDMDATASEG                pSGListHead;
    /** How many entries would fit into the sg info list. */
    uint32_t                   cSGInfoSize;
    /** Number of entries for the information entries. */
    uint32_t                   cSGInfoEntries;
    /** How many times the list was too big. */
    uint32_t                   cSGInfoTooBig;
    /** Pointer to the first mapping information entry. */
    PLSILOGICTASKSTATESGENTRY  paSGEntries;
    /** Size of the temporary buffer for unaligned guest segments. */
    uint32_t                   cbBufferUnaligned;
    /** Pointer to the temporary buffer. */
    void                      *pvBufferUnaligned;
    /** Pointer to the sense buffer. */
    uint8_t                    abSenseBuffer[18];
    /** Flag whether the request was issued from the BIOS. */
    bool                       fBIOS;
} LSILOGICTASKSTATE, *PLSILOGICTASKSTATE;

#ifndef VBOX_DEVICE_STRUCT_TESTCASE

RT_C_DECLS_BEGIN
PDMBOTHCBDECL(int) lsilogicIOPortWrite (PPDMDEVINS pDevIns, void *pvUser,
                                        RTIOPORT Port, uint32_t u32, unsigned cb);
PDMBOTHCBDECL(int) lsilogicIOPortRead (PPDMDEVINS pDevIns, void *pvUser,
                                       RTIOPORT Port, uint32_t *pu32, unsigned cb);
PDMBOTHCBDECL(int) lsilogicMMIOWrite(PPDMDEVINS pDevIns, void *pvUser,
                                     RTGCPHYS GCPhysAddr, void *pv, unsigned cb);
PDMBOTHCBDECL(int) lsilogicMMIORead(PPDMDEVINS pDevIns, void *pvUser,
                                    RTGCPHYS GCPhysAddr, void *pv, unsigned cb);
PDMBOTHCBDECL(int) lsilogicDiagnosticWrite(PPDMDEVINS pDevIns, void *pvUser,
                                           RTGCPHYS GCPhysAddr, void *pv, unsigned cb);
PDMBOTHCBDECL(int) lsilogicDiagnosticRead(PPDMDEVINS pDevIns, void *pvUser,
                                          RTGCPHYS GCPhysAddr, void *pv, unsigned cb);
#ifdef IN_RING3
static void lsilogicInitializeConfigurationPages(PLSILOGICSCSI pLsiLogic);
static int lsilogicProcessConfigurationRequest(PLSILOGICSCSI pLsiLogic, PMptConfigurationRequest pConfigurationReq,
                                               PMptConfigurationReply pReply);
#endif
RT_C_DECLS_END

#define PDMIBASE_2_PLSILOGICDEVICE(pInterface)     ( (PLSILOGICDEVICE)((uintptr_t)(pInterface) - RT_OFFSETOF(LSILOGICDEVICE, IBase)) )
#define PDMISCSIPORT_2_PLSILOGICDEVICE(pInterface) ( (PLSILOGICDEVICE)((uintptr_t)(pInterface) - RT_OFFSETOF(LSILOGICDEVICE, ISCSIPort)) )
#define PDMILEDPORTS_2_PLSILOGICDEVICE(pInterface) ( (PLSILOGICDEVICE)((uintptr_t)(pInterface) - RT_OFFSETOF(LSILOGICDEVICE, ILed)) )
#define LSILOGIC_RTGCPHYS_FROM_U32(Hi, Lo)         ( (RTGCPHYS)RT_MAKE_U64(Lo, Hi) )
#define PDMIBASE_2_PLSILOGICSCSI(pInterface)       ( (PLSILOGICSCSI)((uintptr_t)(pInterface) - RT_OFFSETOF(LSILOGICSCSI, IBase)) )
#define PDMILEDPORTS_2_PLSILOGICSCSI(pInterface)   ( (PLSILOGICSCSI)((uintptr_t)(pInterface) - RT_OFFSETOF(LSILOGICSCSI, ILeds)) )

/** Key sequence the guest has to write to enable access
 * to diagnostic memory. */
static const uint8_t g_lsilogicDiagnosticAccess[] = {0x04, 0x0b, 0x02, 0x07, 0x0d};

/**
 * Updates the status of the interrupt pin of the device.
 *
 * @returns nothing.
 * @param   pThis    Pointer to the device instance data.
 */
static void lsilogicUpdateInterrupt(PLSILOGICSCSI pThis)
{
    uint32_t uIntSts;

    LogFlowFunc(("Updating interrupts\n"));

    /* Mask out doorbell status so that it does not affect interrupt updating. */
    uIntSts = (ASMAtomicReadU32(&pThis->uInterruptStatus) & ~LSILOGIC_REG_HOST_INTR_STATUS_DOORBELL_STS);
    /* Check maskable interrupts. */
    uIntSts &= ~(pThis->uInterruptMask & ~LSILOGIC_REG_HOST_INTR_MASK_IRQ_ROUTING);

    if (uIntSts)
    {
        LogFlowFunc(("Setting interrupt\n"));
        PDMDevHlpPCISetIrq(pThis->CTX_SUFF(pDevIns), 0, 1);
    }
    else
    {
        LogFlowFunc(("Clearing interrupt\n"));
        PDMDevHlpPCISetIrq(pThis->CTX_SUFF(pDevIns), 0, 0);
    }
}

/**
 * Sets a given interrupt status bit in the status register and
 * updates the interupt status.
 *
 * @returns nothing.
 * @param   pLsiLogic    Pointer to the device instance.
 * @param   uStatus      The status bit to set.
 */
DECLINLINE(void) lsilogicSetInterrupt(PLSILOGICSCSI pLsiLogic, uint32_t uStatus)
{
    ASMAtomicOrU32(&pLsiLogic->uInterruptStatus, uStatus);
    lsilogicUpdateInterrupt(pLsiLogic);
}

/**
 * Clears a given interrupt status bit in the status register and
 * updates the interupt status.
 *
 * @returns nothing.
 * @param   pLsiLogic    Pointer to the device instance.
 * @param   uStatus      The status bit to set.
 */
DECLINLINE(void) lsilogicClearInterrupt(PLSILOGICSCSI pLsiLogic, uint32_t uStatus)
{
    ASMAtomicAndU32(&pLsiLogic->uInterruptStatus, ~uStatus);
    lsilogicUpdateInterrupt(pLsiLogic);
}

/**
 * Sets the I/O controller into fault state and sets the fault code.
 *
 * @returns nothing
 * @param   pLsiLogic        Pointer to the controller device instance.
 * @param   uIOCFaultCode    Fault code to set.
 */
DECLINLINE(void) lsilogicSetIOCFaultCode(PLSILOGICSCSI pLsiLogic, uint16_t uIOCFaultCode)
{
    if (pLsiLogic->enmState != LSILOGICSTATE_FAULT)
    {
        Log(("%s: Setting I/O controller into FAULT state: uIOCFaultCode=%u\n", __FUNCTION__, uIOCFaultCode));
        pLsiLogic->enmState        = LSILOGICSTATE_FAULT;
        pLsiLogic->u16IOCFaultCode = uIOCFaultCode;
    }
    else
    {
        Log(("%s: We are already in FAULT state\n"));
    }
}

#ifdef IN_RING3
/**
 * Performs a hard reset on the controller.
 *
 * @returns VBox status code.
 * @param   pThis    Pointer to the device instance to initialize.
 */
static int lsilogicHardReset(PLSILOGICSCSI pThis)
{
    pThis->enmState = LSILOGICSTATE_RESET;

    /* The interrupts are masked out. */
    pThis->uInterruptMask |= LSILOGIC_REG_HOST_INTR_MASK_DOORBELL |
                             LSILOGIC_REG_HOST_INTR_MASK_REPLY;
    /* Reset interrupt states. */
    pThis->uInterruptStatus = 0;
    lsilogicUpdateInterrupt(pThis);

    /* Reset the queues. */
    pThis->uReplyFreeQueueNextEntryFreeWrite = 0;
    pThis->uReplyFreeQueueNextAddressRead = 0;
    pThis->uReplyPostQueueNextEntryFreeWrite = 0;
    pThis->uReplyPostQueueNextAddressRead = 0;
    pThis->uRequestQueueNextEntryFreeWrite = 0;
    pThis->uRequestQueueNextAddressRead = 0;

    /* Disable diagnostic access. */
    pThis->iDiagnosticAccess = 0;

    /* Set default values. */
    pThis->cMaxDevices  = LSILOGIC_DEVICES_MAX;
    pThis->cMaxBuses    = 1;
    pThis->cbReplyFrame = 128; /* @todo Figure out where it is needed. */
    /** @todo: Put stuff to reset here. */

    lsilogicInitializeConfigurationPages(pThis);

    /* Mark that we finished performing the reset. */
    pThis->enmState = LSILOGICSTATE_READY;
    return VINF_SUCCESS;
}

/**
 * Finishes a context reply.
 *
 * @returns nothing
 * @param   pLsiLogic            Pointer to the device instance
 * @param   u32MessageContext    The message context ID to post.
 */
static void lsilogicFinishContextReply(PLSILOGICSCSI pLsiLogic, uint32_t u32MessageContext)
{
    int rc;
    AssertMsg(!pLsiLogic->fDoorbellInProgress, ("We are in a doorbell function\n"));

    /* Write message context ID into reply post queue. */
    rc = PDMCritSectEnter(&pLsiLogic->ReplyPostQueueCritSect, VINF_SUCCESS);
    AssertRC(rc);

#if 0
    /* Check for a entry in the queue. */
    if (RT_UNLIKELY(pLsiLogic->uReplyPostQueueNextAddressRead != pLsiLogic->uReplyPostQueueNextEntryFreeWrite))
    {
        /* Set error code. */
        lsilogicSetIOCFaultCode(pLsiLogic, LSILOGIC_IOCSTATUS_INSUFFICIENT_RESOURCES);
        PDMCritSectLeave(&pLsiLogic->ReplyPostQueueCritSect);
        return;
    }
#endif

    /* We have a context reply. */
    ASMAtomicWriteU32(&pLsiLogic->CTX_SUFF(pReplyPostQueueBase)[pLsiLogic->uReplyPostQueueNextEntryFreeWrite], u32MessageContext);
    ASMAtomicIncU32(&pLsiLogic->uReplyPostQueueNextEntryFreeWrite);
    pLsiLogic->uReplyPostQueueNextEntryFreeWrite %= pLsiLogic->cReplyQueueEntries;

    PDMCritSectLeave(&pLsiLogic->ReplyPostQueueCritSect);

    /* Set interrupt. */
    lsilogicSetInterrupt(pLsiLogic, LSILOGIC_REG_HOST_INTR_STATUS_REPLY_INTR);
}
#endif /* IN_RING3 */

/**
 * Takes neccessary steps to finish a reply frame.
 *
 * @returns nothing
 * @param   pLsiLogic       Pointer to the device instance
 * @param   pReply          Pointer to the reply message.
 * @param   fForceReplyFifo Flag whether the use of the reply post fifo is forced.
 */
static void lsilogicFinishAddressReply(PLSILOGICSCSI pLsiLogic, PMptReplyUnion pReply, bool fForceReplyFifo)
{
    /*
     * If we are in a doorbell function we set the reply size now and
     * set the system doorbell status interrupt to notify the guest that
     * we are ready to send the reply.
     */
    if (pLsiLogic->fDoorbellInProgress && !fForceReplyFifo)
    {
        /* Set size of the reply in 16bit words. The size in the reply is in 32bit dwords. */
        pLsiLogic->cReplySize = pReply->Header.u8MessageLength * 2;
        Log(("%s: cReplySize=%u\n", __FUNCTION__, pLsiLogic->cReplySize));
        pLsiLogic->uNextReplyEntryRead = 0;
        lsilogicSetInterrupt(pLsiLogic, LSILOGIC_REG_HOST_INTR_STATUS_SYSTEM_DOORBELL);
    }
    else
    {
        /*
         * The reply queues are only used if the request was fetched from the request queue.
         * Requests from the request queue are always transferred to R3. So it is not possible
         * that this case happens in R0 or GC.
         */
#ifdef IN_RING3
        int rc;
        /* Grab a free reply message from the queue. */
        rc = PDMCritSectEnter(&pLsiLogic->ReplyFreeQueueCritSect, VINF_SUCCESS);
        AssertRC(rc);

#if 0
        /* Check for a free reply frame. */
        if (RT_UNLIKELY(pLsiLogic->uReplyFreeQueueNextAddressRead != pLsiLogic->uReplyFreeQueueNextEntryFreeWrite))
        {
            /* Set error code. */
            lsilogicSetIOCFaultCode(pLsiLogic, LSILOGIC_IOCSTATUS_INSUFFICIENT_RESOURCES);
            PDMCritSectLeave(&pLsiLogic->ReplyFreeQueueCritSect);
            return;
        }
#endif

        uint32_t u32ReplyFrameAddressLow = pLsiLogic->CTX_SUFF(pReplyFreeQueueBase)[pLsiLogic->uReplyFreeQueueNextAddressRead];

        pLsiLogic->uReplyFreeQueueNextAddressRead++;
        pLsiLogic->uReplyFreeQueueNextAddressRead %= pLsiLogic->cReplyQueueEntries;

        PDMCritSectLeave(&pLsiLogic->ReplyFreeQueueCritSect);

        /* Build 64bit physical address. */
        RTGCPHYS GCPhysReplyMessage = LSILOGIC_RTGCPHYS_FROM_U32(pLsiLogic->u32HostMFAHighAddr, u32ReplyFrameAddressLow);
        size_t cbReplyCopied = (pLsiLogic->cbReplyFrame < sizeof(MptReplyUnion)) ? pLsiLogic->cbReplyFrame : sizeof(MptReplyUnion);

        /* Write reply to guest memory. */
        PDMDevHlpPhysWrite(pLsiLogic->CTX_SUFF(pDevIns), GCPhysReplyMessage, pReply, cbReplyCopied);

        /* Write low 32bits of reply frame into post reply queue. */
        rc = PDMCritSectEnter(&pLsiLogic->ReplyPostQueueCritSect, VINF_SUCCESS);
        AssertRC(rc);

#if 0
        /* Check for a entry in the queue. */
        if (RT_UNLIKELY(pLsiLogic->uReplyPostQueueNextAddressRead != pLsiLogic->uReplyPostQueueNextEntryFreeWrite))
        {
            /* Set error code. */
            lsilogicSetIOCFaultCode(pLsiLogic, LSILOGIC_IOCSTATUS_INSUFFICIENT_RESOURCES);
            PDMCritSectLeave(&pLsiLogic->ReplyPostQueueCritSect);
            return;
        }
#endif

        /* We have a address reply. Set the 31th bit to indicate that. */
        ASMAtomicWriteU32(&pLsiLogic->CTX_SUFF(pReplyPostQueueBase)[pLsiLogic->uReplyPostQueueNextEntryFreeWrite],
                          RT_BIT(31) | (u32ReplyFrameAddressLow >> 1));
        ASMAtomicIncU32(&pLsiLogic->uReplyPostQueueNextEntryFreeWrite);
        pLsiLogic->uReplyPostQueueNextEntryFreeWrite %= pLsiLogic->cReplyQueueEntries;

        PDMCritSectLeave(&pLsiLogic->ReplyPostQueueCritSect);

        if (fForceReplyFifo)
        {
            pLsiLogic->fDoorbellInProgress = false;
            lsilogicSetInterrupt(pLsiLogic, LSILOGIC_REG_HOST_INTR_STATUS_SYSTEM_DOORBELL);
        }

        /* Set interrupt. */
        lsilogicSetInterrupt(pLsiLogic, LSILOGIC_REG_HOST_INTR_STATUS_REPLY_INTR);
#else
        AssertMsgFailed(("This is not allowed to happen.\n"));
#endif
    }
}

#ifdef IN_RING3
/**
 * Processes a given Request from the guest
 *
 * @returns VBox status code.
 * @param   pLsiLogic    Pointer to the device instance.
 * @param   pMessageHdr  Pointer to the message header of the request.
 * @param   pReply       Pointer to the reply.
 */
static int lsilogicProcessMessageRequest(PLSILOGICSCSI pLsiLogic, PMptMessageHdr pMessageHdr, PMptReplyUnion pReply)
{
    int rc = VINF_SUCCESS;
    bool fForceReplyPostFifo = false;

#ifdef DEBUG
    if (pMessageHdr->u8Function < RT_ELEMENTS(g_apszMPTFunctionNames))
        Log(("Message request function: %s\n", g_apszMPTFunctionNames[pMessageHdr->u8Function]));
    else
        Log(("Message request function: <unknown>\n"));
#endif

    memset(pReply, 0, sizeof(MptReplyUnion));

    switch (pMessageHdr->u8Function)
    {
        case MPT_MESSAGE_HDR_FUNCTION_SCSI_TASK_MGMT:
        {
            PMptSCSITaskManagementRequest pTaskMgmtReq = (PMptSCSITaskManagementRequest)pMessageHdr;

            pReply->SCSITaskManagement.u8MessageLength     = 6;     /* 6 32bit dwords. */
            pReply->SCSITaskManagement.u8TaskType          = pTaskMgmtReq->u8TaskType;
            pReply->SCSITaskManagement.u32TerminationCount = 0;
            //AssertMsgFailed(("todo\n"));
            fForceReplyPostFifo = true;
            break;
        }
        case MPT_MESSAGE_HDR_FUNCTION_IOC_INIT:
        {
            /*
             * This request sets the I/O controller to the
             * operational state.
             */
            PMptIOCInitRequest pIOCInitReq = (PMptIOCInitRequest)pMessageHdr;

            /* Update configuration values. */
            pLsiLogic->enmWhoInit             = (LSILOGICWHOINIT)pIOCInitReq->u8WhoInit;
            pLsiLogic->cbReplyFrame           = pIOCInitReq->u16ReplyFrameSize;
            pLsiLogic->cMaxBuses              = pIOCInitReq->u8MaxBuses;
            pLsiLogic->cMaxDevices            = pIOCInitReq->u8MaxDevices;
            pLsiLogic->u32HostMFAHighAddr     = pIOCInitReq->u32HostMfaHighAddr;
            pLsiLogic->u32SenseBufferHighAddr = pIOCInitReq->u32SenseBufferHighAddr;

            if (pLsiLogic->enmState == LSILOGICSTATE_READY)
            {
                pLsiLogic->enmState = LSILOGICSTATE_OPERATIONAL;
            }

            /* Return reply. */
            pReply->IOCInit.u8MessageLength = 5;
            pReply->IOCInit.u8WhoInit       = pLsiLogic->enmWhoInit;
            pReply->IOCInit.u8MaxDevices    = pLsiLogic->cMaxDevices;
            pReply->IOCInit.u8MaxBuses      = pLsiLogic->cMaxBuses;
            break;
        }
        case MPT_MESSAGE_HDR_FUNCTION_IOC_FACTS:
        {
            pReply->IOCFacts.u8MessageLength      = 15;     /* 15 32bit dwords. */
            pReply->IOCFacts.u16MessageVersion    = 0x0102; /* Version from the specification. */
            pReply->IOCFacts.u8IOCNumber          = 0;      /* PCI function number. */
            pReply->IOCFacts.u16IOCExceptions     = 0;
            pReply->IOCFacts.u8MaxChainDepth      = LSILOGICSCSI_MAXIMUM_CHAIN_DEPTH;
            pReply->IOCFacts.u8WhoInit            = pLsiLogic->enmWhoInit;
            pReply->IOCFacts.u8BlockSize          = 12;     /* Block size in 32bit dwords. This is the largest request we can get (SCSI I/O). */
            pReply->IOCFacts.u8Flags              = 0;      /* Bit 0 is set if the guest must upload the FW prior to using the controller. Obviously not needed here. */
            pReply->IOCFacts.u16ReplyQueueDepth   = pLsiLogic->cReplyQueueEntries - 1; /* One entry is always free. */
            pReply->IOCFacts.u16RequestFrameSize  = 128;    /* @todo Figure out where it is needed. */
            pReply->IOCFacts.u16ProductID         = 0xcafe; /* Our own product ID :) */
            pReply->IOCFacts.u32CurrentHostMFAHighAddr = pLsiLogic->u32HostMFAHighAddr;
            pReply->IOCFacts.u16GlobalCredits     = pLsiLogic->cRequestQueueEntries - 1; /* One entry is always free. */
            pReply->IOCFacts.u8NumberOfPorts      = 1;
            pReply->IOCFacts.u8EventState         = 0; /* Event notifications not enabled. */
            pReply->IOCFacts.u32CurrentSenseBufferHighAddr = pLsiLogic->u32SenseBufferHighAddr;
            pReply->IOCFacts.u16CurReplyFrameSize = pLsiLogic->cbReplyFrame;
            pReply->IOCFacts.u8MaxDevices         = pLsiLogic->cMaxDevices;
            pReply->IOCFacts.u8MaxBuses           = pLsiLogic->cMaxBuses;
            pReply->IOCFacts.u32FwImageSize       = 0; /* No image needed. */
            pReply->IOCFacts.u32FWVersion         = 0;
            break;
        }
        case MPT_MESSAGE_HDR_FUNCTION_PORT_FACTS:
        {
            PMptPortFactsRequest pPortFactsReq = (PMptPortFactsRequest)pMessageHdr;

            pReply->PortFacts.u8MessageLength = 10;
            pReply->PortFacts.u8PortNumber    = pPortFactsReq->u8PortNumber;

            /* This controller only supports one bus with bus number 0. */
            if (pPortFactsReq->u8PortNumber != 0)
            {
                pReply->PortFacts.u8PortType = 0; /* Not existant. */
            }
            else
            {
                pReply->PortFacts.u8PortType             = 0x01; /* SCSI Port. */
                pReply->PortFacts.u16MaxDevices          = LSILOGIC_DEVICES_MAX;
                pReply->PortFacts.u16ProtocolFlags       = RT_BIT(3) | RT_BIT(0); /* SCSI initiator and LUN supported. */
                pReply->PortFacts.u16PortSCSIID          = 7; /* Default */
                pReply->PortFacts.u16MaxPersistentIDs    = 0;
                pReply->PortFacts.u16MaxPostedCmdBuffers = 0; /* Only applies for target mode which we dont support. */
                pReply->PortFacts.u16MaxLANBuckets       = 0; /* Only for the LAN controller. */
            }
            break;
        }
        case MPT_MESSAGE_HDR_FUNCTION_PORT_ENABLE:
        {
            /*
             * The port enable request notifies the IOC to make the port available and perform
             * appropriate discovery on the associated link.
             */
            PMptPortEnableRequest pPortEnableReq = (PMptPortEnableRequest)pMessageHdr;

            pReply->PortEnable.u8MessageLength = 5;
            pReply->PortEnable.u8PortNumber    = pPortEnableReq->u8PortNumber;
            break;
        }
        case MPT_MESSAGE_HDR_FUNCTION_EVENT_NOTIFICATION:
        {
            PMptEventNotificationRequest pEventNotificationReq = (PMptEventNotificationRequest)pMessageHdr;

            if (pEventNotificationReq->u8Switch)
                pLsiLogic->fEventNotificationEnabled = true;
            else
                pLsiLogic->fEventNotificationEnabled = false;

            pReply->EventNotification.u16EventDataLength = 1; /* 1 32bit D-Word. */
            pReply->EventNotification.u8MessageLength    = 8;
            pReply->EventNotification.u8MessageFlags     = (1 << 7);
            pReply->EventNotification.u8AckRequired      = 0;
            pReply->EventNotification.u32Event           = MPT_EVENT_EVENT_CHANGE;
            pReply->EventNotification.u32EventContext    = 0;
            pReply->EventNotification.u32EventData       = pLsiLogic->fEventNotificationEnabled ? 1 : 0;

            break;
        }
        case MPT_MESSAGE_HDR_FUNCTION_EVENT_ACK:
        {
            AssertMsgFailed(("todo"));
            break;
        }
        case MPT_MESSAGE_HDR_FUNCTION_CONFIG:
        {
            PMptConfigurationRequest pConfigurationReq = (PMptConfigurationRequest)pMessageHdr;

            rc = lsilogicProcessConfigurationRequest(pLsiLogic, pConfigurationReq, &pReply->Configuration);
            AssertRC(rc);
            break;
        }
        case MPT_MESSAGE_HDR_FUNCTION_SCSI_IO_REQUEST: /* Should be handled already. */
        default:
            AssertMsgFailed(("Invalid request function %#x\n", pMessageHdr->u8Function));
    }

    /* Copy common bits from request message frame to reply. */
    pReply->Header.u8Function        = pMessageHdr->u8Function;
    pReply->Header.u32MessageContext = pMessageHdr->u32MessageContext;

    lsilogicFinishAddressReply(pLsiLogic, pReply, fForceReplyPostFifo);
    return rc;
}
#endif

/**
 * Writes a value to a register at a given offset.
 *
 * @returns VBox status code.
 * @param   pThis    Pointer to the LsiLogic SCSI controller instance data.
 * @param   uOffset  Offset of the register to write.
 * @param   pv       Pointer to the value to write
 * @param   cb       Number of bytes to write.
 */
static int lsilogicRegisterWrite(PLSILOGICSCSI pThis, uint32_t uOffset, void *pv, unsigned cb)
{
    uint32_t u32 = *(uint32_t *)pv;

    LogFlowFunc(("pThis=%#p uOffset=%#x pv=%#p{%.*Rhxs} cb=%u\n", pThis, uOffset, pv, cb, pv, cb));

    switch (uOffset)
    {
        case LSILOGIC_REG_REPLY_QUEUE:
        {
            /* Add the entry to the reply free queue. */
            ASMAtomicWriteU32(&pThis->CTX_SUFF(pReplyFreeQueueBase)[pThis->uReplyFreeQueueNextEntryFreeWrite], u32);
            pThis->uReplyFreeQueueNextEntryFreeWrite++;
            pThis->uReplyFreeQueueNextEntryFreeWrite %= pThis->cReplyQueueEntries;
            break;
        }
        case LSILOGIC_REG_REQUEST_QUEUE:
        {
            ASMAtomicWriteU32(&pThis->CTX_SUFF(pRequestQueueBase)[pThis->uRequestQueueNextEntryFreeWrite], u32);
            pThis->uRequestQueueNextEntryFreeWrite++;
            pThis->uRequestQueueNextEntryFreeWrite %= pThis->cRequestQueueEntries;

            /* Send notification to R3 if there is not one send already. */
            if (!ASMAtomicXchgBool(&pThis->fNotificationSend, true))
            {
                PPDMQUEUEITEMCORE pNotificationItem = PDMQueueAlloc(pThis->CTX_SUFF(pNotificationQueue));
                AssertPtr(pNotificationItem);

                PDMQueueInsert(pThis->CTX_SUFF(pNotificationQueue), pNotificationItem);
            }
            break;
        }
        case LSILOGIC_REG_DOORBELL:
        {
            /*
             * When the guest writes to this register a real device would set the
             * doorbell status bit in the interrupt status register to indicate that the IOP
             * has still to process the message.
             * The guest needs to wait with posting new messages here until the bit is cleared.
             * Because the guest is not continuing execution while we are here we can skip this.
             */
            if (!pThis->fDoorbellInProgress)
            {
                uint32_t uFunction = LSILOGIC_REG_DOORBELL_GET_FUNCTION(u32);

                switch (uFunction)
                {
                    case LSILOGIC_DOORBELL_FUNCTION_IOC_MSG_UNIT_RESET:
                    {
                        pThis->enmState = LSILOGICSTATE_RESET;

                        /* Reset interrupt states. */
                        pThis->uInterruptMask   = 0;
                        pThis->uInterruptStatus = 0;
                        lsilogicUpdateInterrupt(pThis);

                        /* Reset the queues. */
                        pThis->uReplyFreeQueueNextEntryFreeWrite = 0;
                        pThis->uReplyFreeQueueNextAddressRead = 0;
                        pThis->uReplyPostQueueNextEntryFreeWrite = 0;
                        pThis->uReplyPostQueueNextAddressRead = 0;
                        pThis->uRequestQueueNextEntryFreeWrite = 0;
                        pThis->uRequestQueueNextAddressRead = 0;
                        pThis->enmState = LSILOGICSTATE_READY;
                        break;
                    }
                    case LSILOGIC_DOORBELL_FUNCTION_IO_UNIT_RESET:
                    {
                        AssertMsgFailed(("todo\n"));
                        break;
                    }
                    case LSILOGIC_DOORBELL_FUNCTION_HANDSHAKE:
                    {
                        pThis->cMessage = LSILOGIC_REG_DOORBELL_GET_SIZE(u32);
                        pThis->iMessage = 0;
                        AssertMsg(pThis->cMessage <= RT_ELEMENTS(pThis->aMessage),
                                  ("Message doesn't fit into the buffer, cMessage=%u", pThis->cMessage));
                        pThis->fDoorbellInProgress = true;
                        /* Update the interrupt status to notify the guest that a doorbell function was started. */
                        lsilogicSetInterrupt(pThis, LSILOGIC_REG_HOST_INTR_STATUS_SYSTEM_DOORBELL);
                        break;
                    }
                    case LSILOGIC_DOORBELL_FUNCTION_REPLY_FRAME_REMOVAL:
                    {
                        AssertMsgFailed(("todo\n"));
                        break;
                    }
                    default:
                        AssertMsgFailed(("Unknown function %u to perform\n", uFunction));
                }
            }
            else
            {
                /*
                 * We are already performing a doorbell function.
                 * Get the remaining parameters.
                 */
                AssertMsg(pThis->iMessage < RT_ELEMENTS(pThis->aMessage), ("Message is too big to fit into the buffer\n"));
                /*
                 * If the last byte of the message is written, force a switch to R3 because some requests might force
                 * a reply through the FIFO which cannot be handled in GC or R0.
                 */
#ifndef IN_RING3
                if (pThis->iMessage == pThis->cMessage - 1)
                    return VINF_IOM_HC_MMIO_WRITE;
#endif
                pThis->aMessage[pThis->iMessage++] = u32;
#ifdef IN_RING3
                if (pThis->iMessage == pThis->cMessage)
                {
                    int rc = lsilogicProcessMessageRequest(pThis, (PMptMessageHdr)pThis->aMessage, &pThis->ReplyBuffer);
                    AssertRC(rc);
                }
#endif
            }
            break;
        }
        case LSILOGIC_REG_HOST_INTR_STATUS:
        {
            /*
             * Clear the bits the guest wants except the system doorbell interrupt and the IO controller
             * status bit.
             * The former bit is always cleared no matter what the guest writes to the register and
             * the latter one is read only.
             */
            pThis->uInterruptStatus = pThis->uInterruptStatus & ~LSILOGIC_REG_HOST_INTR_STATUS_SYSTEM_DOORBELL;

            /*
             * Check if there is still a doorbell function in progress. Set the
             * system doorbell interrupt bit again if it is.
             * We do not use lsilogicSetInterrupt here because the interrupt status
             * is updated afterwards anyway.
             */
            if (   (pThis->fDoorbellInProgress)
                && (pThis->cMessage == pThis->iMessage))
            {
                if (pThis->uNextReplyEntryRead == pThis->cReplySize)
                {
                    /* Reply finished. Reset doorbell in progress status. */
                    Log(("%s: Doorbell function finished\n", __FUNCTION__));
                    pThis->fDoorbellInProgress = false;
                }
                ASMAtomicOrU32(&pThis->uInterruptStatus, LSILOGIC_REG_HOST_INTR_STATUS_SYSTEM_DOORBELL);
            }

            lsilogicUpdateInterrupt(pThis);
            break;
        }
        case LSILOGIC_REG_HOST_INTR_MASK:
        {
            pThis->uInterruptMask = (u32 & LSILOGIC_REG_HOST_INTR_MASK_W_MASK);
            lsilogicUpdateInterrupt(pThis);
            break;
        }
        case LSILOGIC_REG_WRITE_SEQUENCE:
        {
            if (pThis->fDiagnosticEnabled)
            {
                /* Any value will cause a reset and disabling access. */
                pThis->fDiagnosticEnabled = false;
                pThis->iDiagnosticAccess  = 0;
            }
            else if ((u32 & 0xf) == g_lsilogicDiagnosticAccess[pThis->iDiagnosticAccess])
            {
                pThis->iDiagnosticAccess++;
                if (pThis->iDiagnosticAccess == RT_ELEMENTS(g_lsilogicDiagnosticAccess))
                {
                    /*
                     * Key sequence successfully written. Enable access to diagnostic
                     * memory and register.
                     */
                    pThis->fDiagnosticEnabled = true;
                }
            }
            else
            {
                /* Wrong value written - reset to beginning. */
                pThis->iDiagnosticAccess = 0;
            }
            break;
        }
        case LSILOGIC_REG_HOST_DIAGNOSTIC:
        {
#ifndef IN_RING3
            return VINF_IOM_HC_IOPORT_WRITE;
#else
            if (u32 & LSILOGIC_REG_HOST_DIAGNOSTIC_RESET_ADAPTER)
            {
                lsilogicHardReset(pThis);
            }
            break;
#endif
        }
        default: /* Ignore. */
        {
            break;
        }
    }
    return VINF_SUCCESS;
}

/**
 * Reads the content of a register at a given offset.
 *
 * @returns VBox status code.
 * @param   pThis    Pointer to the LsiLogic SCSI controller instance data.
 * @param   uOffset  Offset of the register to read.
 * @param   pv       Where to store the content of the register.
 * @param   cb       Number of bytes to read.
 */
static int lsilogicRegisterRead(PLSILOGICSCSI pThis, uint32_t uOffset, void *pv, unsigned cb)
{
    uint32_t u32 = 0;

    /* Align to a 4 byte offset. */ 
    switch (uOffset & ~3)
    {
        case LSILOGIC_REG_REPLY_QUEUE:
        {
            /*
             * Non 4-byte access may cause real strange behavior because the data is part of a physical guest address.
             * But some drivers use 1-byte access to scan for SCSI controllers.
             */
            if (RT_UNLIKELY(cb != 4))
                LogFlowFunc((": cb is not 4 (%u)\n", cb));

            if (pThis->uReplyPostQueueNextEntryFreeWrite != pThis->uReplyPostQueueNextAddressRead)
            {
                u32 = pThis->CTX_SUFF(pReplyPostQueueBase)[pThis->uReplyPostQueueNextAddressRead];
                pThis->uReplyPostQueueNextAddressRead++;
                pThis->uReplyPostQueueNextAddressRead %= pThis->cReplyQueueEntries;
            }
            else
            {
                /* The reply post queue is empty. Reset interrupt. */
                u32 = UINT32_C(0xffffffff);
                lsilogicClearInterrupt(pThis, LSILOGIC_REG_HOST_INTR_STATUS_REPLY_INTR);
            }
            Log(("%s: Returning address %#x\n", __FUNCTION__, u32));
            break;
        }
        case LSILOGIC_REG_DOORBELL:
        {
            u32  = LSILOGIC_REG_DOORBELL_SET_STATE(pThis->enmState);
            u32 |= LSILOGIC_REG_DOORBELL_SET_USED(pThis->fDoorbellInProgress);
            u32 |= LSILOGIC_REG_DOORBELL_SET_WHOINIT(pThis->enmWhoInit);
            /*
             * If there is a doorbell function in progress we pass the return value
             * instead of the status code. We transfer 16bit of the reply
             * during one read.
             */
            if (pThis->fDoorbellInProgress)
            {
                /* Return next 16bit value. */
                u32 |= pThis->ReplyBuffer.au16Reply[pThis->uNextReplyEntryRead++];
            }
            else
            {
                /* We return the status code of the I/O controller. */
                u32 |= pThis->u16IOCFaultCode;
            }
            break;
        }
        case LSILOGIC_REG_HOST_INTR_STATUS:
        {
            u32 = pThis->uInterruptStatus;
            break;
        }
        case LSILOGIC_REG_HOST_INTR_MASK:
        {
            u32 = pThis->uInterruptMask;
            break;
        }
        case LSILOGIC_REG_HOST_DIAGNOSTIC:
        {
            if (pThis->fDiagnosticEnabled)
                u32 = LSILOGIC_REG_HOST_DIAGNOSTIC_DRWE;
            else
                u32 = 0;
            break;
        }
        case LSILOGIC_REG_TEST_BASE_ADDRESS: /* The spec doesn't say anything about these registers, so we just ignore them */
        case LSILOGIC_REG_DIAG_RW_DATA:
        case LSILOGIC_REG_DIAG_RW_ADDRESS:
        default: /* Ignore. */
        {
            break;
        }
    }

    /* Clip data according to the read size. */
    switch (cb)
    {
        case 4:
        {
            *(uint32_t *)pv = u32;
            break;
        }
        case 2:
        {
            uint8_t uBitsOff = (uOffset - (uOffset & 3))*8;

            u32 &= (0xffff << uBitsOff);
            *(uint16_t *)pv = (uint16_t)(u32 >> uBitsOff);
            break;
        }
        case 1:
        {
            uint8_t uBitsOff = (uOffset - (uOffset & 3))*8;

            u32 &= (0xff << uBitsOff);
            *(uint8_t *)pv = (uint8_t)(u32 >> uBitsOff);
            break;
        }
        default:
            AssertMsgFailed(("Invalid access size %u\n", cb));
    }

    LogFlowFunc(("pThis=%#p uOffset=%#x pv=%#p{%.*Rhxs} cb=%u\n", pThis, uOffset, pv, cb, pv, cb));

    return VINF_SUCCESS;
}

PDMBOTHCBDECL(int) lsilogicIOPortWrite (PPDMDEVINS pDevIns, void *pvUser,
                                        RTIOPORT Port, uint32_t u32, unsigned cb)
{
    PLSILOGICSCSI  pThis = PDMINS_2_DATA(pDevIns, PLSILOGICSCSI);
    uint32_t   uOffset = Port - pThis->IOPortBase;

    Assert(cb <= 4);

    int rc = lsilogicRegisterWrite(pThis, uOffset, &u32, cb);
    if (rc == VINF_IOM_HC_MMIO_WRITE)
        rc = VINF_IOM_HC_IOPORT_WRITE;

    return rc;
}

PDMBOTHCBDECL(int) lsilogicIOPortRead (PPDMDEVINS pDevIns, void *pvUser,
                                       RTIOPORT Port, uint32_t *pu32, unsigned cb)
{
    PLSILOGICSCSI  pThis = PDMINS_2_DATA(pDevIns, PLSILOGICSCSI);
    uint32_t   uOffset = Port - pThis->IOPortBase;

    Assert(cb <= 4);

    return lsilogicRegisterRead(pThis, uOffset, pu32, cb);
}

PDMBOTHCBDECL(int) lsilogicMMIOWrite(PPDMDEVINS pDevIns, void *pvUser,
                                     RTGCPHYS GCPhysAddr, void *pv, unsigned cb)
{
    PLSILOGICSCSI  pThis = PDMINS_2_DATA(pDevIns, PLSILOGICSCSI);
    uint32_t   uOffset = GCPhysAddr - pThis->GCPhysMMIOBase;

    return lsilogicRegisterWrite(pThis, uOffset, pv, cb);
}

PDMBOTHCBDECL(int) lsilogicMMIORead(PPDMDEVINS pDevIns, void *pvUser,
                                    RTGCPHYS GCPhysAddr, void *pv, unsigned cb)
{
    PLSILOGICSCSI  pThis = PDMINS_2_DATA(pDevIns, PLSILOGICSCSI);
    uint32_t   uOffset = GCPhysAddr - pThis->GCPhysMMIOBase;

    return lsilogicRegisterRead(pThis, uOffset, pv, cb);
}

PDMBOTHCBDECL(int) lsilogicDiagnosticWrite(PPDMDEVINS pDevIns, void *pvUser,
                                           RTGCPHYS GCPhysAddr, void *pv, unsigned cb)
{
    PLSILOGICSCSI  pThis = PDMINS_2_DATA(pDevIns, PLSILOGICSCSI);

    LogFlowFunc(("pThis=%#p GCPhysAddr=%RGp pv=%#p{%.*Rhxs} cb=%u\n", pThis, GCPhysAddr, pv, cb, pv, cb));

    return VINF_SUCCESS;
}

PDMBOTHCBDECL(int) lsilogicDiagnosticRead(PPDMDEVINS pDevIns, void *pvUser,
                                          RTGCPHYS GCPhysAddr, void *pv, unsigned cb)
{
    PLSILOGICSCSI  pThis = PDMINS_2_DATA(pDevIns, PLSILOGICSCSI);

    LogFlowFunc(("pThis=%#p GCPhysAddr=%RGp pv=%#p{%.*Rhxs} cb=%u\n", pThis, GCPhysAddr, pv, cb, pv, cb));

    return VINF_SUCCESS;
}

#ifdef IN_RING3

/**
 * Copies a contigous buffer into the scatter gather list provided by the guest.
 *
 * @returns nothing
 * @param   pTaskState    Pointer to the task state which contains the SGL.
 * @param   pvBuf         Pointer to the buffer to copy.
 * @param   cbCopy        Number of bytes to copy.
 */
static void lsilogicScatterGatherListCopyFromBuffer(PLSILOGICTASKSTATE pTaskState, void *pvBuf, size_t cbCopy)
{
    unsigned cSGEntry = 0;
    PPDMDATASEG pSGEntry = &pTaskState->pSGListHead[cSGEntry];
    uint8_t *pu8Buf = (uint8_t *)pvBuf;

    while (cSGEntry < pTaskState->cSGListEntries)
    {
        size_t cbToCopy = (cbCopy < pSGEntry->cbSeg) ? cbCopy : pSGEntry->cbSeg;

        memcpy(pSGEntry->pvSeg, pu8Buf, cbToCopy);

        cbCopy -= cbToCopy;
        /* We finished. */
        if (!cbCopy)
            break;

        /* Advance the buffer. */
        pu8Buf += cbToCopy;

        /* Go to the next entry in the list. */
        pSGEntry++;
        cSGEntry++;
    }
}

/**
 * Copy a temporary buffer into a part of the guest scatter gather list
 * described by the given descriptor entry.
 *
 * @returns nothing.
 * @param   pDevIns    Pointer to the device instance data.
 * @param   pSGInfo    Pointer to the segment info structure which describes the guest segments
 *                     to write to which are unaligned.
 */
static void lsilogicCopyFromBufferIntoSGList(PPDMDEVINS pDevIns, PLSILOGICTASKSTATESGENTRY pSGInfo)
{
    RTGCPHYS GCPhysBuffer = pSGInfo->u.GCPhysAddrBufferUnaligned;

    AssertMsg(!pSGInfo->fGuestMemory, ("This is not possible\n"));

    /* Copy into SG entry. */
    PDMDevHlpPhysWrite(pDevIns, GCPhysBuffer, pSGInfo->pvBuf, pSGInfo->cbBuf);

}

/**
 * Copy a part of the guest scatter gather list into a temporary buffer.
 *
 * @returns nothing.
 * @param   pDevIns    Pointer to the device instance data.
 * @param   pSGInfo    Pointer to the segment info structure which describes the guest segments
 *                     to read from which are unaligned.
 */
static void lsilogicCopyFromSGListIntoBuffer(PPDMDEVINS pDevIns, PLSILOGICTASKSTATESGENTRY pSGInfo)
{
    RTGCPHYS GCPhysBuffer = pSGInfo->u.GCPhysAddrBufferUnaligned;

    AssertMsg(!pSGInfo->fGuestMemory, ("This is not possible\n"));

    /* Copy into temporary buffer. */
    PDMDevHlpPhysRead(pDevIns, GCPhysBuffer, pSGInfo->pvBuf, pSGInfo->cbBuf);
}

static int lsilogicScatterGatherListAllocate(PLSILOGICTASKSTATE pTaskState, uint32_t cSGList, uint32_t cSGInfo, uint32_t cbUnaligned)
{
    if (pTaskState->cSGListSize < cSGList)
    {
        /* The entries are not allocated yet or the number is too small. */
        if (pTaskState->cSGListSize)
            RTMemFree(pTaskState->pSGListHead);

        /* Allocate R3 scatter gather list. */
        pTaskState->pSGListHead = (PPDMDATASEG)RTMemAllocZ(cSGList * sizeof(PDMDATASEG));
        if (!pTaskState->pSGListHead)
            return VERR_NO_MEMORY;

        /* Reset usage statistics. */
        pTaskState->cSGListSize     = cSGList;
        pTaskState->cSGListEntries  = cSGList;
        pTaskState->cSGListTooBig = 0;
    }
    else if (pTaskState->cSGListSize > cSGList)
    {
        /*
         * The list is too big. Increment counter.
         * So that the destroying function can free
         * the list if it is too big too many times
         * in a row.
         */
        pTaskState->cSGListEntries = cSGList;
        pTaskState->cSGListTooBig++;
    }
    else
    {
        /*
         * Needed entries matches current size.
         * Reset counter.
         */
        pTaskState->cSGListEntries = cSGList;
        pTaskState->cSGListTooBig  = 0;
    }

    if (pTaskState->cSGInfoSize < cSGInfo)
    {
        /* The entries are not allocated yet or the number is too small. */
        if (pTaskState->cSGInfoSize)
            RTMemFree(pTaskState->paSGEntries);

        pTaskState->paSGEntries = (PLSILOGICTASKSTATESGENTRY)RTMemAllocZ(cSGInfo * sizeof(LSILOGICTASKSTATESGENTRY));
        if (!pTaskState->paSGEntries)
            return VERR_NO_MEMORY;

        /* Reset usage statistics. */
        pTaskState->cSGInfoSize = cSGInfo;
        pTaskState->cSGInfoEntries  = cSGInfo;
        pTaskState->cSGInfoTooBig = 0;
    }
    else if (pTaskState->cSGInfoSize > cSGInfo)
    {
        /*
         * The list is too big. Increment counter.
         * So that the destroying function can free
         * the list if it is too big too many times
         * in a row.
         */
        pTaskState->cSGInfoEntries = cSGInfo;
        pTaskState->cSGInfoTooBig++;
    }
    else
    {
        /*
         * Needed entries matches current size.
         * Reset counter.
         */
        pTaskState->cSGInfoEntries  = cSGInfo;
        pTaskState->cSGInfoTooBig = 0;
    }


    if (pTaskState->cbBufferUnaligned < cbUnaligned)
    {
        if (pTaskState->pvBufferUnaligned)
            RTMemFree(pTaskState->pvBufferUnaligned);

        Log(("%s: Allocating buffer for unaligned segments cbUnaligned=%u\n", __FUNCTION__, cbUnaligned));

        pTaskState->pvBufferUnaligned = RTMemAllocZ(cbUnaligned);
        if (!pTaskState->pvBufferUnaligned)
            return VERR_NO_MEMORY;

        pTaskState->cbBufferUnaligned = cbUnaligned;
    }

    /* Make debugging easier. */
#ifdef DEBUG
    memset(pTaskState->pSGListHead, 0, pTaskState->cSGListSize * sizeof(PDMDATASEG));
    memset(pTaskState->paSGEntries, 0, pTaskState->cSGInfoSize * sizeof(LSILOGICTASKSTATESGENTRY));
    if (pTaskState->pvBufferUnaligned)
        memset(pTaskState->pvBufferUnaligned, 0, pTaskState->cbBufferUnaligned);
#endif
    return VINF_SUCCESS;
}

/**
 * Destroy a scatter gather list.
 *
 * @returns nothing.
 * @param   pLsiLogic    Pointer to the LsiLogic SCSI controller.
 * @param   pTaskState   Pointer to the task state.
 */
static void lsilogicScatterGatherListDestroy(PLSILOGICSCSI pLsiLogic, PLSILOGICTASKSTATE pTaskState)
{
    PPDMDEVINS                pDevIns     = pLsiLogic->CTX_SUFF(pDevIns);
    PLSILOGICTASKSTATESGENTRY pSGInfoCurr = pTaskState->paSGEntries;

    for (unsigned i = 0; i < pTaskState->cSGInfoEntries; i++)
    {
        if (pSGInfoCurr->fGuestMemory)
        {
            /* Release the lock. */
            PDMDevHlpPhysReleasePageMappingLock(pDevIns, &pSGInfoCurr->u.PageLock);
        }
        else if (!pSGInfoCurr->fBufferContainsData)
        {
            /* Copy the data into the guest segments now. */
            lsilogicCopyFromBufferIntoSGList(pLsiLogic->CTX_SUFF(pDevIns), pSGInfoCurr);
        }

        pSGInfoCurr++;
    }

    /* Free allocated memory if the list was too big too many times. */
    if (pTaskState->cSGListTooBig >= LSILOGIC_NR_OF_ALLOWED_BIGGER_LISTS)
    {
        RTMemFree(pTaskState->pSGListHead);
        RTMemFree(pTaskState->paSGEntries);
        if (pTaskState->pvBufferUnaligned)
            RTMemFree(pTaskState->pvBufferUnaligned);
        pTaskState->cSGListSize = 0;
        pTaskState->cSGInfoSize = 0;
        pTaskState->cSGInfoEntries = 0;
        pTaskState->cSGListTooBig  = 0;
        pTaskState->pSGListHead = NULL;
        pTaskState->paSGEntries = NULL;
        pTaskState->pvBufferUnaligned = NULL;
        pTaskState->cbBufferUnaligned = 0;
    }
}

#ifdef DEBUG
/**
 * Dump an SG entry.
 *
 * @returns nothing.
 * @param   pSGEntry    Pointer to the SG entry to dump
 */
static void lsilogicDumpSGEntry(PMptSGEntryUnion pSGEntry)
{
    switch (pSGEntry->Simple32.u2ElementType)
    {
        case MPTSGENTRYTYPE_SIMPLE:
        {
            Log(("%s: Dumping info for SIMPLE SG entry:\n", __FUNCTION__));
            Log(("%s: u24Length=%u\n", __FUNCTION__, pSGEntry->Simple32.u24Length));
            Log(("%s: fEndOfList=%d\n", __FUNCTION__, pSGEntry->Simple32.fEndOfList));
            Log(("%s: f64BitAddress=%d\n", __FUNCTION__, pSGEntry->Simple32.f64BitAddress));
            Log(("%s: fBufferContainsData=%d\n", __FUNCTION__, pSGEntry->Simple32.fBufferContainsData));
            Log(("%s: fLocalAddress=%d\n", __FUNCTION__, pSGEntry->Simple32.fLocalAddress));
            Log(("%s: fEndOfBuffer=%d\n", __FUNCTION__, pSGEntry->Simple32.fEndOfBuffer));
            Log(("%s: fLastElement=%d\n", __FUNCTION__, pSGEntry->Simple32.fLastElement));
            Log(("%s: u32DataBufferAddressLow=%u\n", __FUNCTION__, pSGEntry->Simple32.u32DataBufferAddressLow));
            if (pSGEntry->Simple32.f64BitAddress)
            {
                Log(("%s: u32DataBufferAddressHigh=%u\n", __FUNCTION__, pSGEntry->Simple64.u32DataBufferAddressHigh));
                Log(("%s: GCDataBufferAddress=%RGp\n", __FUNCTION__,
                    ((uint64_t)pSGEntry->Simple64.u32DataBufferAddressHigh << 32) | pSGEntry->Simple64.u32DataBufferAddressLow));
            }
            else
                Log(("%s: GCDataBufferAddress=%RGp\n", __FUNCTION__, pSGEntry->Simple32.u32DataBufferAddressLow));

            break;
        }
        case MPTSGENTRYTYPE_CHAIN:
        {
            Log(("%s: Dumping info for CHAIN SG entry:\n", __FUNCTION__));
            Log(("%s: u16Length=%u\n", __FUNCTION__, pSGEntry->Chain.u16Length));
            Log(("%s: u8NExtChainOffset=%d\n", __FUNCTION__, pSGEntry->Chain.u8NextChainOffset));
            Log(("%s: f64BitAddress=%d\n", __FUNCTION__, pSGEntry->Chain.f64BitAddress));
            Log(("%s: fLocalAddress=%d\n", __FUNCTION__, pSGEntry->Chain.fLocalAddress));
            Log(("%s: u32SegmentAddressLow=%u\n", __FUNCTION__, pSGEntry->Chain.u32SegmentAddressLow));
            Log(("%s: u32SegmentAddressHigh=%u\n", __FUNCTION__, pSGEntry->Chain.u32SegmentAddressHigh));
            if (pSGEntry->Chain.f64BitAddress)
                Log(("%s: GCSegmentAddress=%RGp\n", __FUNCTION__,
                    ((uint64_t)pSGEntry->Chain.u32SegmentAddressHigh << 32) | pSGEntry->Chain.u32SegmentAddressLow));
            else
                Log(("%s: GCSegmentAddress=%RGp\n", __FUNCTION__, pSGEntry->Chain.u32SegmentAddressLow));
            break;
        }
    }
}
#endif

/**
 * Create scatter gather list descriptors.
 *
 * @returns VBox status code.
 * @param   pLsiLogic      Pointer to the LsiLogic SCSI controller.
 * @param   pTaskState     Pointer to the task state.
 * @param   GCPhysSGLStart Guest physical address of the first SG entry.
 * @param   uChainOffset   Offset in bytes from the beginning of the SGL segment to the chain element.
 * @thread  EMT
 */
static int lsilogicScatterGatherListCreate(PLSILOGICSCSI pLsiLogic, PLSILOGICTASKSTATE pTaskState,
                                           RTGCPHYS GCPhysSGLStart, uint32_t uChainOffset)
{
    int                        rc           = VINF_SUCCESS;
    PPDMDEVINS                 pDevIns      = pLsiLogic->CTX_SUFF(pDevIns);
    PVM                        pVM          = PDMDevHlpGetVM(pDevIns);
    bool                       fUnaligned;     /* Flag whether the current buffer is unaligned. */
    uint32_t                   cbUnaligned;    /* Size of the unaligned buffers. */
    uint32_t                   cSGEntriesR3 = 0;
    uint32_t                   cSGInfo      = 0;
    uint32_t                   cbSegment    = 0;
    PLSILOGICTASKSTATESGENTRY  pSGInfoCurr  = NULL;
    uint8_t                   *pu8BufferUnalignedPos = NULL;
    uint8_t                   *pbBufferUnalignedSGInfoPos = NULL;
    uint32_t                   cbUnalignedComplete = 0;
    bool                       fDoMapping = false;
    bool                       fEndOfList;
    RTGCPHYS                   GCPhysSGEntryNext;
    RTGCPHYS                   GCPhysSegmentStart;
    uint32_t                   uChainOffsetNext;

    /*
     * Two passes - one to count needed scatter gather list entries and needed unaligned
     * buffers and one to actually map the SG list into R3.
     */
    for (int i = 0; i < 2; i++)
    {
        fUnaligned      = false;
        cbUnaligned     = 0;
        fEndOfList      = false;

        GCPhysSGEntryNext  = GCPhysSGLStart;
        uChainOffsetNext   = uChainOffset;
        GCPhysSegmentStart = GCPhysSGLStart;

        if (fDoMapping)
        {
            Log(("%s: cSGInfo=%u\n", __FUNCTION__, cSGInfo));

            /* The number of needed SG entries in R3 is known. Allocate needed memory. */
            rc = lsilogicScatterGatherListAllocate(pTaskState, cSGInfo, cSGInfo, cbUnalignedComplete);
            AssertMsgRC(rc, ("Failed to allocate scatter gather array rc=%Rrc\n", rc));

            /* We are now able to map the pages into R3. */
            pSGInfoCurr = pTaskState->paSGEntries;
            /* Initialize first segment to remove the need for additional if checks later in the code. */
            pSGInfoCurr->fGuestMemory= false;
            pu8BufferUnalignedPos = (uint8_t *)pTaskState->pvBufferUnaligned;
            pbBufferUnalignedSGInfoPos = pu8BufferUnalignedPos;
        }

        /* Go through the list until we reach the end. */
        while (!fEndOfList)
        {
            bool fEndOfSegment = false;

            while (!fEndOfSegment)
            {
                MptSGEntryUnion SGEntry;

                Log(("%s: Reading SG entry from %RGp\n", __FUNCTION__, GCPhysSGEntryNext));

                /* Read the entry. */
                PDMDevHlpPhysRead(pDevIns, GCPhysSGEntryNext, &SGEntry, sizeof(MptSGEntryUnion));

#ifdef DEBUG
                lsilogicDumpSGEntry(&SGEntry);
#endif

                AssertMsg(SGEntry.Simple32.u2ElementType == MPTSGENTRYTYPE_SIMPLE, ("Invalid SG entry type\n"));

                /* Check if this is a zero element. */
                if (   !SGEntry.Simple32.u24Length
                    && SGEntry.Simple32.fEndOfList
                    && SGEntry.Simple32.fEndOfBuffer)
                {
                    pTaskState->cSGListEntries = 0;
                    pTaskState->cSGInfoEntries = 0;
                    return VINF_SUCCESS;
                }

                uint32_t cbDataToTransfer     = SGEntry.Simple32.u24Length;
                bool     fBufferContainsData  = !!SGEntry.Simple32.fBufferContainsData;
                RTGCPHYS GCPhysAddrDataBuffer = SGEntry.Simple32.u32DataBufferAddressLow;

                if (SGEntry.Simple32.f64BitAddress)
                {
                    GCPhysAddrDataBuffer |= ((uint64_t)SGEntry.Simple64.u32DataBufferAddressHigh) << 32;
                    GCPhysSGEntryNext += sizeof(MptSGEntrySimple64);
                }
                else
                    GCPhysSGEntryNext += sizeof(MptSGEntrySimple32);

                if (fDoMapping)
                {
                    pSGInfoCurr->fGuestMemory = false;
                    pSGInfoCurr->fBufferContainsData = fBufferContainsData;
                    pSGInfoCurr->cbBuf = cbDataToTransfer;
                    pSGInfoCurr->pvBuf = pbBufferUnalignedSGInfoPos;
                    pbBufferUnalignedSGInfoPos += cbDataToTransfer;
                    pSGInfoCurr->u.GCPhysAddrBufferUnaligned = GCPhysAddrDataBuffer;
                    if (fBufferContainsData)
                        lsilogicCopyFromSGListIntoBuffer(pDevIns, pSGInfoCurr);
                    pSGInfoCurr++;
                }
                else
                {
                    cbUnalignedComplete += cbDataToTransfer;
                    cSGInfo++;
                }

                /* Check if we reached the end of the list. */
                if (SGEntry.Simple32.fEndOfList)
                {
                    /* We finished. */
                    fEndOfSegment = true;
                    fEndOfList = true;
                }
                else if (SGEntry.Simple32.fLastElement)
                {
                    fEndOfSegment = true;
                }
            } /* while (!fEndOfSegment) */

            /* Get next chain element. */
            if (uChainOffsetNext)
            {
                MptSGEntryChain SGEntryChain;

                PDMDevHlpPhysRead(pDevIns, GCPhysSegmentStart + uChainOffsetNext, &SGEntryChain, sizeof(MptSGEntryChain));

                AssertMsg(SGEntryChain.u2ElementType == MPTSGENTRYTYPE_CHAIN, ("Invalid SG entry type\n"));

               /* Set the next address now. */
                GCPhysSGEntryNext = SGEntryChain.u32SegmentAddressLow;
                if (SGEntryChain.f64BitAddress)
                    GCPhysSGEntryNext |= ((uint64_t)SGEntryChain.u32SegmentAddressHigh) << 32;

                GCPhysSegmentStart = GCPhysSGEntryNext;
                uChainOffsetNext   = SGEntryChain.u8NextChainOffset * sizeof(uint32_t);
            }

        } /* while (!fEndOfList) */

        fDoMapping = true;
        if (fUnaligned)
            cbUnalignedComplete += cbUnaligned;
    }

    uint32_t    cSGEntries;
    PPDMDATASEG pSGEntryCurr = pTaskState->pSGListHead;
    pSGInfoCurr              = pTaskState->paSGEntries;

    /* Initialize first entry. */
    pSGEntryCurr->pvSeg = pSGInfoCurr->pvBuf;
    pSGEntryCurr->cbSeg = pSGInfoCurr->cbBuf;
    pSGInfoCurr++;
    cSGEntries = 1;

    /* Construct the scatter gather list. */
    for (unsigned i = 0; i < (pTaskState->cSGInfoEntries-1); i++)
    {
        if (pSGEntryCurr->cbSeg % 512 != 0)
        {
            AssertMsg((uint8_t *)pSGEntryCurr->pvSeg + pSGEntryCurr->cbSeg == pSGInfoCurr->pvBuf,
                      ("Buffer ist not sector aligned but the buffer addresses are not adjacent\n"));

            pSGEntryCurr->cbSeg += pSGInfoCurr->cbBuf;
        }
        else
        {
            if (((uint8_t *)pSGEntryCurr->pvSeg + pSGEntryCurr->cbSeg) == pSGInfoCurr->pvBuf)
            {
                pSGEntryCurr->cbSeg += pSGInfoCurr->cbBuf;
            }
            else
            {
                pSGEntryCurr++;
                cSGEntries++;
                pSGEntryCurr->pvSeg = pSGInfoCurr->pvBuf;
                pSGEntryCurr->cbSeg = pSGInfoCurr->cbBuf;
            }
        }

        pSGInfoCurr++;
    }

    pTaskState->cSGListEntries = cSGEntries;

    return rc;
}

/*
 * Disabled because the sense buffer provided by the LsiLogic driver for Windows XP
 * crosses page boundaries.
 */
#if 0
/**
 * Free the sense buffer.
 *
 * @returns nothing.
 * @param   pTaskState   Pointer to the task state.
 */
static void lsilogicFreeGCSenseBuffer(PLSILOGICSCSI pLsiLogic, PLSILOGICTASKSTATE pTaskState)
{
    PVM pVM = PDMDevHlpGetVM(pLsiLogic->CTX_SUFF(pDevIns));

    PGMPhysReleasePageMappingLock(pVM, &pTaskState->PageLockSense);
    pTaskState->pbSenseBuffer = NULL;
}

/**
 * Map the sense buffer into R3.
 *
 * @returns VBox status code.
 * @param   pTaskState    Pointer to the task state.
 * @note Current assumption is that the sense buffer is not scattered and does not cross a page boundary.
 */
static int lsilogicMapGCSenseBufferIntoR3(PLSILOGICSCSI pLsiLogic, PLSILOGICTASKSTATE pTaskState)
{
    int rc = VINF_SUCCESS;
    PPDMDEVINS pDevIns = pLsiLogic->CTX_SUFF(pDevIns);
    RTGCPHYS GCPhysAddrSenseBuffer;

    GCPhysAddrSenseBuffer = pTaskState->GuestRequest.SCSIIO.u32SenseBufferLowAddress;
    GCPhysAddrSenseBuffer |= ((uint64_t)pLsiLogic->u32SenseBufferHighAddr << 32);

#ifdef RT_STRICT
    uint32_t cbSenseBuffer = pTaskState->GuestRequest.SCSIIO.u8SenseBufferLength;
#endif
    RTGCPHYS GCPhysAddrSenseBufferBase = PAGE_ADDRESS(GCPhysAddrSenseBuffer);

    AssertMsg(GCPhysAddrSenseBuffer >= GCPhysAddrSenseBufferBase,
              ("Impossible GCPhysAddrSenseBuffer < GCPhysAddrSenseBufferBase\n"));

    /* Sanity checks for the assumption. */
    AssertMsg(((GCPhysAddrSenseBuffer + cbSenseBuffer) <= (GCPhysAddrSenseBufferBase + PAGE_SIZE)),
              ("Sense buffer crosses page boundary\n"));

    rc = PDMDevHlpPhysGCPhys2CCPtr(pDevIns, GCPhysAddrSenseBufferBase, (void **)&pTaskState->pbSenseBuffer, &pTaskState->PageLockSense);
    AssertMsgRC(rc, ("Mapping sense buffer failed rc=%Rrc\n", rc));

    /* Correct start address of the sense buffer. */
    pTaskState->pbSenseBuffer += (GCPhysAddrSenseBuffer - GCPhysAddrSenseBufferBase);

    return rc;
}
#endif

#ifdef DEBUG
static void lsilogicDumpSCSIIORequest(PMptSCSIIORequest pSCSIIORequest)
{
    Log(("%s: u8TargetID=%d\n", __FUNCTION__, pSCSIIORequest->u8TargetID));
    Log(("%s: u8Bus=%d\n", __FUNCTION__, pSCSIIORequest->u8Bus));
    Log(("%s: u8ChainOffset=%d\n", __FUNCTION__, pSCSIIORequest->u8ChainOffset));
    Log(("%s: u8Function=%d\n", __FUNCTION__, pSCSIIORequest->u8Function));
    Log(("%s: u8CDBLength=%d\n", __FUNCTION__, pSCSIIORequest->u8CDBLength));
    Log(("%s: u8SenseBufferLength=%d\n", __FUNCTION__, pSCSIIORequest->u8SenseBufferLength));
    Log(("%s: u8MessageFlags=%d\n", __FUNCTION__, pSCSIIORequest->u8MessageFlags));
    Log(("%s: u32MessageContext=%#x\n", __FUNCTION__, pSCSIIORequest->u32MessageContext));
    for (unsigned i = 0; i < RT_ELEMENTS(pSCSIIORequest->au8LUN); i++)
        Log(("%s: u8LUN[%d]=%d\n", __FUNCTION__, i, pSCSIIORequest->au8LUN[i]));
    Log(("%s: u32Control=%#x\n", __FUNCTION__, pSCSIIORequest->u32Control));
    for (unsigned i = 0; i < RT_ELEMENTS(pSCSIIORequest->au8CDB); i++)
        Log(("%s: u8CDB[%d]=%d\n", __FUNCTION__, i, pSCSIIORequest->au8CDB[i]));
    Log(("%s: u32DataLength=%#x\n", __FUNCTION__, pSCSIIORequest->u32DataLength));
    Log(("%s: u32SenseBufferLowAddress=%#x\n", __FUNCTION__, pSCSIIORequest->u32SenseBufferLowAddress));
}
#endif

/**
 * Processes a SCSI I/O request by setting up the request
 * and sending it to the underlying SCSI driver.
 * Steps needed to complete request are done in the
 * callback called by the driver below upon completion of
 * the request.
 *
 * @returns VBox status code.
 * @param   pLsiLogic    Pointer to the device instance which sends the request.
 * @param   pTaskState   Pointer to the task state data.
 */
static int lsilogicProcessSCSIIORequest(PLSILOGICSCSI pLsiLogic, PLSILOGICTASKSTATE pTaskState)
{
    int rc = VINF_SUCCESS;

#ifdef DEBUG
    lsilogicDumpSCSIIORequest(&pTaskState->GuestRequest.SCSIIO);
#endif

    pTaskState->fBIOS = false;

    uint32_t uChainOffset = pTaskState->GuestRequest.SCSIIO.u8ChainOffset;

    if (uChainOffset)
        uChainOffset = uChainOffset * sizeof(uint32_t) - sizeof(MptSCSIIORequest);

    /* Create Scatter gather list. */
    rc = lsilogicScatterGatherListCreate(pLsiLogic, pTaskState,
                                         pTaskState->GCPhysMessageFrameAddr + sizeof(MptSCSIIORequest),
                                         uChainOffset);
    AssertRC(rc);

#if 0
    /* Map sense buffer. */
    rc = lsilogicMapGCSenseBufferIntoR3(pLsiLogic, pTaskState);
    AssertRC(rc);
#endif

    if (RT_LIKELY(   (pTaskState->GuestRequest.SCSIIO.u8TargetID < LSILOGIC_DEVICES_MAX)
                  && (pTaskState->GuestRequest.SCSIIO.u8Bus == 0)))
    {
        PLSILOGICDEVICE pTargetDevice;
        pTargetDevice = &pLsiLogic->aDeviceStates[pTaskState->GuestRequest.SCSIIO.u8TargetID];

        if (pTargetDevice->pDrvBase)
        {
            /* Setup the SCSI request. */
            pTaskState->pTargetDevice                        = pTargetDevice;
            pTaskState->PDMScsiRequest.uLogicalUnit          = pTaskState->GuestRequest.SCSIIO.au8LUN[1];

            uint8_t uDataDirection = MPT_SCSIIO_REQUEST_CONTROL_TXDIR_GET(pTaskState->GuestRequest.SCSIIO.u32Control);

            if (uDataDirection == MPT_SCSIIO_REQUEST_CONTROL_TXDIR_NONE)
                pTaskState->PDMScsiRequest.uDataDirection    = PDMSCSIREQUESTTXDIR_NONE;
            else if (uDataDirection == MPT_SCSIIO_REQUEST_CONTROL_TXDIR_WRITE)
                pTaskState->PDMScsiRequest.uDataDirection    = PDMSCSIREQUESTTXDIR_TO_DEVICE;
            else if (uDataDirection == MPT_SCSIIO_REQUEST_CONTROL_TXDIR_READ)
                pTaskState->PDMScsiRequest.uDataDirection    = PDMSCSIREQUESTTXDIR_FROM_DEVICE;

            pTaskState->PDMScsiRequest.cbCDB                 = pTaskState->GuestRequest.SCSIIO.u8CDBLength;
            pTaskState->PDMScsiRequest.pbCDB                 = pTaskState->GuestRequest.SCSIIO.au8CDB;
            pTaskState->PDMScsiRequest.cbScatterGather       = pTaskState->GuestRequest.SCSIIO.u32DataLength;
            pTaskState->PDMScsiRequest.cScatterGatherEntries = pTaskState->cSGListEntries;
            pTaskState->PDMScsiRequest.paScatterGatherHead   = pTaskState->pSGListHead;
            pTaskState->PDMScsiRequest.cbSenseBuffer         = sizeof(pTaskState->abSenseBuffer);
            memset(pTaskState->abSenseBuffer, 0, pTaskState->PDMScsiRequest.cbSenseBuffer);
            pTaskState->PDMScsiRequest.pbSenseBuffer         = pTaskState->abSenseBuffer;
            pTaskState->PDMScsiRequest.pvUser                = pTaskState;

            ASMAtomicIncU32(&pTargetDevice->cOutstandingRequests);
            rc = pTargetDevice->pDrvSCSIConnector->pfnSCSIRequestSend(pTargetDevice->pDrvSCSIConnector, &pTaskState->PDMScsiRequest);
            AssertMsgRC(rc, ("Sending request to SCSI layer failed rc=%Rrc\n", rc));
            return VINF_SUCCESS;
        }
        else
        {
            /* Device is not present report SCSI selection timeout. */
            pTaskState->IOCReply.SCSIIOError.u16IOCStatus = MPT_SCSI_IO_ERROR_IOCSTATUS_DEVICE_NOT_THERE;
        }
    }
    else
    {
        /* Report out of bounds target ID or bus. */
        if (pTaskState->GuestRequest.SCSIIO.u8Bus != 0)
            pTaskState->IOCReply.SCSIIOError.u16IOCStatus = MPT_SCSI_IO_ERROR_IOCSTATUS_INVALID_BUS;
        else
            pTaskState->IOCReply.SCSIIOError.u16IOCStatus = MPT_SCSI_IO_ERROR_IOCSTATUS_INVALID_TARGETID;
    }

    /* The rest is equal to both errors. */
    pTaskState->IOCReply.SCSIIOError.u8TargetID = pTaskState->GuestRequest.SCSIIO.u8TargetID;
    pTaskState->IOCReply.SCSIIOError.u8Bus = pTaskState->GuestRequest.SCSIIO.u8Bus;
    pTaskState->IOCReply.SCSIIOError.u8MessageLength = sizeof(MptSCSIIOErrorReply) / 4;
    pTaskState->IOCReply.SCSIIOError.u8Function = pTaskState->GuestRequest.SCSIIO.u8Function;
    pTaskState->IOCReply.SCSIIOError.u8CDBLength = pTaskState->GuestRequest.SCSIIO.u8CDBLength;
    pTaskState->IOCReply.SCSIIOError.u8SenseBufferLength = pTaskState->GuestRequest.SCSIIO.u8SenseBufferLength;
    pTaskState->IOCReply.SCSIIOError.u32MessageContext = pTaskState->GuestRequest.SCSIIO.u32MessageContext;
    pTaskState->IOCReply.SCSIIOError.u8SCSIStatus = SCSI_STATUS_OK;
    pTaskState->IOCReply.SCSIIOError.u8SCSIState  = MPT_SCSI_IO_ERROR_SCSI_STATE_TERMINATED;
    pTaskState->IOCReply.SCSIIOError.u32IOCLogInfo    = 0;
    pTaskState->IOCReply.SCSIIOError.u32TransferCount = 0;
    pTaskState->IOCReply.SCSIIOError.u32SenseCount    = 0;
    pTaskState->IOCReply.SCSIIOError.u32ResponseInfo  = 0;

    lsilogicFinishAddressReply(pLsiLogic, &pTaskState->IOCReply, false);
    RTCacheInsert(pLsiLogic->pTaskCache, pTaskState);

    return rc;
}

/**
 * Called upon completion of the request from the SCSI driver below.
 * This function frees all allocated ressources and notifies the guest
 * that the process finished by asserting an interrupt.
 *
 * @returns VBox status code.
 * @param   pInterface    Pointer to the interface the called funtion belongs to.
 * @param   pSCSIRequest  Pointer to the SCSI request which finished.
 */
static DECLCALLBACK(int) lsilogicDeviceSCSIRequestCompleted(PPDMISCSIPORT pInterface, PPDMSCSIREQUEST pSCSIRequest, int rcCompletion)
{
    PLSILOGICTASKSTATE pTaskState      = (PLSILOGICTASKSTATE)pSCSIRequest->pvUser;
    PLSILOGICDEVICE    pLsiLogicDevice = pTaskState->pTargetDevice;
    PLSILOGICSCSI      pLsiLogic       = pLsiLogicDevice->CTX_SUFF(pLsiLogic);

    ASMAtomicDecU32(&pLsiLogicDevice->cOutstandingRequests);

    if (RT_UNLIKELY(pTaskState->fBIOS))
    {
        int rc = vboxscsiRequestFinished(&pLsiLogic->VBoxSCSI, pSCSIRequest);
        AssertMsgRC(rc, ("Finishing BIOS SCSI request failed rc=%Rrc\n", rc));
    }
    else
    {
#if 0
        lsilogicFreeGCSenseBuffer(pLsiLogic, pTaskState);
#else
        RTGCPHYS GCPhysAddrSenseBuffer;

        GCPhysAddrSenseBuffer = pTaskState->GuestRequest.SCSIIO.u32SenseBufferLowAddress;
        GCPhysAddrSenseBuffer |= ((uint64_t)pLsiLogic->u32SenseBufferHighAddr << 32);

        /* Copy the sense buffer over. */
        PDMDevHlpPhysWrite(pLsiLogic->CTX_SUFF(pDevIns), GCPhysAddrSenseBuffer, pTaskState->abSenseBuffer,
                             RT_UNLIKELY(pTaskState->GuestRequest.SCSIIO.u8SenseBufferLength < pTaskState->PDMScsiRequest.cbSenseBuffer)
                           ? pTaskState->GuestRequest.SCSIIO.u8SenseBufferLength
                           : pTaskState->PDMScsiRequest.cbSenseBuffer);
#endif
        lsilogicScatterGatherListDestroy(pLsiLogic, pTaskState);


        if (RT_LIKELY(rcCompletion == SCSI_STATUS_OK))
            lsilogicFinishContextReply(pLsiLogic, pTaskState->GuestRequest.SCSIIO.u32MessageContext);
        else
        {
            /* The SCSI target encountered an error during processing post a reply. */
            memset(&pTaskState->IOCReply, 0, sizeof(MptReplyUnion));
            pTaskState->IOCReply.SCSIIOError.u8TargetID = pTaskState->GuestRequest.SCSIIO.u8TargetID;
            pTaskState->IOCReply.SCSIIOError.u8Bus      = pTaskState->GuestRequest.SCSIIO.u8Bus;
            pTaskState->IOCReply.SCSIIOError.u8MessageLength = 8;
            pTaskState->IOCReply.SCSIIOError.u8Function  = pTaskState->GuestRequest.SCSIIO.u8Function;
            pTaskState->IOCReply.SCSIIOError.u8CDBLength = pTaskState->GuestRequest.SCSIIO.u8CDBLength;
            pTaskState->IOCReply.SCSIIOError.u8SenseBufferLength = pTaskState->GuestRequest.SCSIIO.u8SenseBufferLength;
            pTaskState->IOCReply.SCSIIOError.u8MessageFlags = pTaskState->GuestRequest.SCSIIO.u8MessageFlags;
            pTaskState->IOCReply.SCSIIOError.u32MessageContext = pTaskState->GuestRequest.SCSIIO.u32MessageContext;
            pTaskState->IOCReply.SCSIIOError.u8SCSIStatus = rcCompletion;
            pTaskState->IOCReply.SCSIIOError.u8SCSIState  = MPT_SCSI_IO_ERROR_SCSI_STATE_AUTOSENSE_VALID;
            pTaskState->IOCReply.SCSIIOError.u16IOCStatus  = 0;
            pTaskState->IOCReply.SCSIIOError.u32IOCLogInfo = 0;
            pTaskState->IOCReply.SCSIIOError.u32TransferCount = 0;
            pTaskState->IOCReply.SCSIIOError.u32SenseCount    = sizeof(pTaskState->abSenseBuffer);
            pTaskState->IOCReply.SCSIIOError.u32ResponseInfo  = 0;

            lsilogicFinishAddressReply(pLsiLogic, &pTaskState->IOCReply, true);
        }
    }

    RTCacheInsert(pLsiLogic->pTaskCache, pTaskState);

    return VINF_SUCCESS;
}

/**
 * Return the configuration page header and data
 * which matches the given page type and number.
 *
 * @returns VINF_SUCCESS if successful
 *          VERR_NOT_FOUND if the requested page could be found.
 * @param   u8PageNumber  Number of the page to get.
 * @param   ppPageHeader  Where to store the pointer to the page header.
 * @param   ppbPageData   Where to store the pointer to the page data.
 */
static int lsilogicConfigurationIOUnitPageGetFromNumber(PLSILOGICSCSI pLsiLogic, uint8_t u8PageNumber,
                                                        PMptConfigurationPageHeader *ppPageHeader,
                                                        uint8_t **ppbPageData, size_t *pcbPage)
{
    int rc = VINF_SUCCESS;

    AssertMsg(VALID_PTR(ppPageHeader) && VALID_PTR(ppbPageData), ("Invalid parameters\n"));

    switch(u8PageNumber)
    {
        case 0:
            *ppPageHeader = &pLsiLogic->ConfigurationPages.SpiPages.IOUnitPage0.u.fields.Header;
            *ppbPageData  =  pLsiLogic->ConfigurationPages.SpiPages.IOUnitPage0.u.abPageData;
            *pcbPage      = sizeof(pLsiLogic->ConfigurationPages.SpiPages.IOUnitPage0);
            break;
        case 1:
            *ppPageHeader = &pLsiLogic->ConfigurationPages.SpiPages.IOUnitPage1.u.fields.Header;
            *ppbPageData  =  pLsiLogic->ConfigurationPages.SpiPages.IOUnitPage1.u.abPageData;
            *pcbPage      = sizeof(pLsiLogic->ConfigurationPages.SpiPages.IOUnitPage1);
            break;
        case 2:
            *ppPageHeader = &pLsiLogic->ConfigurationPages.SpiPages.IOUnitPage2.u.fields.Header;
            *ppbPageData  =  pLsiLogic->ConfigurationPages.SpiPages.IOUnitPage2.u.abPageData;
            *pcbPage      = sizeof(pLsiLogic->ConfigurationPages.SpiPages.IOUnitPage2);
            break;
        case 3:
            *ppPageHeader = &pLsiLogic->ConfigurationPages.SpiPages.IOUnitPage3.u.fields.Header;
            *ppbPageData  =  pLsiLogic->ConfigurationPages.SpiPages.IOUnitPage3.u.abPageData;
            *pcbPage      = sizeof(pLsiLogic->ConfigurationPages.SpiPages.IOUnitPage3);
            break;
        default:
            rc = VERR_NOT_FOUND;
    }

    return rc;
}

/**
 * Return the configuration page header and data
 * which matches the given page type and number.
 *
 * @returns VINF_SUCCESS if successful
 *          VERR_NOT_FOUND if the requested page could be found.
 * @param   u8PageNumber  Number of the page to get.
 * @param   ppPageHeader  Where to store the pointer to the page header.
 * @param   ppbPageData   Where to store the pointer to the page data.
 */
static int lsilogicConfigurationIOCPageGetFromNumber(PLSILOGICSCSI pLsiLogic, uint8_t u8PageNumber,
                                                     PMptConfigurationPageHeader *ppPageHeader,
                                                     uint8_t **ppbPageData, size_t *pcbPage)
{
    int rc = VINF_SUCCESS;

    AssertMsg(VALID_PTR(ppPageHeader) && VALID_PTR(ppbPageData), ("Invalid parameters\n"));

    switch(u8PageNumber)
    {
        case 0:
            *ppPageHeader = &pLsiLogic->ConfigurationPages.SpiPages.IOCPage0.u.fields.Header;
            *ppbPageData  =  pLsiLogic->ConfigurationPages.SpiPages.IOCPage0.u.abPageData;
            *pcbPage      = sizeof(pLsiLogic->ConfigurationPages.SpiPages.IOCPage0);
            break;
        case 1:
            *ppPageHeader = &pLsiLogic->ConfigurationPages.SpiPages.IOCPage1.u.fields.Header;
            *ppbPageData  =  pLsiLogic->ConfigurationPages.SpiPages.IOCPage1.u.abPageData;
            *pcbPage      = sizeof(pLsiLogic->ConfigurationPages.SpiPages.IOCPage1);
            break;
        case 2:
            *ppPageHeader = &pLsiLogic->ConfigurationPages.SpiPages.IOCPage2.u.fields.Header;
            *ppbPageData  =  pLsiLogic->ConfigurationPages.SpiPages.IOCPage2.u.abPageData;
            *pcbPage      = sizeof(pLsiLogic->ConfigurationPages.SpiPages.IOCPage2);
            break;
        case 3:
            *ppPageHeader = &pLsiLogic->ConfigurationPages.SpiPages.IOCPage3.u.fields.Header;
            *ppbPageData  =  pLsiLogic->ConfigurationPages.SpiPages.IOCPage3.u.abPageData;
            *pcbPage      = sizeof(pLsiLogic->ConfigurationPages.SpiPages.IOCPage3);
            break;
        case 4:
            *ppPageHeader = &pLsiLogic->ConfigurationPages.SpiPages.IOCPage4.u.fields.Header;
            *ppbPageData  =  pLsiLogic->ConfigurationPages.SpiPages.IOCPage4.u.abPageData;
            *pcbPage      = sizeof(pLsiLogic->ConfigurationPages.SpiPages.IOCPage4);
            break;
        case 6:
            *ppPageHeader = &pLsiLogic->ConfigurationPages.SpiPages.IOCPage6.u.fields.Header;
            *ppbPageData  =  pLsiLogic->ConfigurationPages.SpiPages.IOCPage6.u.abPageData;
            *pcbPage      = sizeof(pLsiLogic->ConfigurationPages.SpiPages.IOCPage6);
            break;
        default:
            rc = VERR_NOT_FOUND;
    }

    return rc;
}

/**
 * Return the configuration page header and data
 * which matches the given page type and number.
 *
 * @returns VINF_SUCCESS if successful
 *          VERR_NOT_FOUND if the requested page could be found.
 * @param   u8PageNumber  Number of the page to get.
 * @param   ppPageHeader  Where to store the pointer to the page header.
 * @param   ppbPageData   Where to store the pointer to the page data.
 */
static int lsilogicConfigurationManufacturingPageGetFromNumber(PLSILOGICSCSI pLsiLogic, uint8_t u8PageNumber,
                                                               PMptConfigurationPageHeader *ppPageHeader,
                                                               uint8_t **ppbPageData, size_t *pcbPage)
{
    int rc = VINF_SUCCESS;

    AssertMsg(VALID_PTR(ppPageHeader) && VALID_PTR(ppbPageData), ("Invalid parameters\n"));

    switch(u8PageNumber)
    {
        case 0:
            *ppPageHeader = &pLsiLogic->ConfigurationPages.SpiPages.ManufacturingPage0.u.fields.Header;
            *ppbPageData  =  pLsiLogic->ConfigurationPages.SpiPages.ManufacturingPage0.u.abPageData;
            *pcbPage      = sizeof(pLsiLogic->ConfigurationPages.SpiPages.ManufacturingPage0);
            break;
        case 1:
            *ppPageHeader = &pLsiLogic->ConfigurationPages.SpiPages.ManufacturingPage1.Header;
            *ppbPageData  =  pLsiLogic->ConfigurationPages.SpiPages.ManufacturingPage1.abVPDInfo;
            *pcbPage      = sizeof(pLsiLogic->ConfigurationPages.SpiPages.ManufacturingPage1);
            break;
        case 2:
            *ppPageHeader = &pLsiLogic->ConfigurationPages.SpiPages.ManufacturingPage2.u.fields.Header;
            *ppbPageData  =  pLsiLogic->ConfigurationPages.SpiPages.ManufacturingPage2.u.abPageData;
            *pcbPage      = sizeof(pLsiLogic->ConfigurationPages.SpiPages.ManufacturingPage2);
            break;
        case 3:
            *ppPageHeader = &pLsiLogic->ConfigurationPages.SpiPages.ManufacturingPage3.u.fields.Header;
            *ppbPageData  =  pLsiLogic->ConfigurationPages.SpiPages.ManufacturingPage3.u.abPageData;
            *pcbPage      = sizeof(pLsiLogic->ConfigurationPages.SpiPages.ManufacturingPage3);
            break;
        case 4:
            *ppPageHeader = &pLsiLogic->ConfigurationPages.SpiPages.ManufacturingPage4.u.fields.Header;
            *ppbPageData  =  pLsiLogic->ConfigurationPages.SpiPages.ManufacturingPage4.u.abPageData;
            *pcbPage      = sizeof(pLsiLogic->ConfigurationPages.SpiPages.ManufacturingPage4);
            break;
        default:
            rc = VERR_NOT_FOUND;
    }

    return rc;
}

/**
 * Return the configuration page header and data
 * which matches the given page type and number.
 *
 * @returns VINF_SUCCESS if successful
 *          VERR_NOT_FOUND if the requested page could be found.
 * @param   u8PageNumber  Number of the page to get.
 * @param   ppPageHeader  Where to store the pointer to the page header.
 * @param   ppbPageData   Where to store the pointer to the page data.
 */
static int lsilogicConfigurationSCSISPIPortPageGetFromNumber(PLSILOGICSCSI pLsiLogic, uint8_t u8Port,
                                                             uint8_t u8PageNumber,
                                                             PMptConfigurationPageHeader *ppPageHeader,
                                                             uint8_t **ppbPageData, size_t *pcbPage)
{
    int rc = VINF_SUCCESS;

    AssertMsg(VALID_PTR(ppPageHeader) && VALID_PTR(ppbPageData), ("Invalid parameters\n"));

    if (u8Port >= RT_ELEMENTS(pLsiLogic->ConfigurationPages.SpiPages.aPortPages))
        return VERR_NOT_FOUND;

    switch(u8PageNumber)
    {
        case 0:
            *ppPageHeader = &pLsiLogic->ConfigurationPages.SpiPages.aPortPages[u8Port].SCSISPIPortPage0.u.fields.Header;
            *ppbPageData  =  pLsiLogic->ConfigurationPages.SpiPages.aPortPages[u8Port].SCSISPIPortPage0.u.abPageData;
            *pcbPage      = sizeof(pLsiLogic->ConfigurationPages.SpiPages.aPortPages[u8Port].SCSISPIPortPage0);
            break;
        case 1:
            *ppPageHeader = &pLsiLogic->ConfigurationPages.SpiPages.aPortPages[u8Port].SCSISPIPortPage1.u.fields.Header;
            *ppbPageData  =  pLsiLogic->ConfigurationPages.SpiPages.aPortPages[u8Port].SCSISPIPortPage1.u.abPageData;
            *pcbPage      = sizeof(pLsiLogic->ConfigurationPages.SpiPages.aPortPages[u8Port].SCSISPIPortPage1);
            break;
        case 2:
            *ppPageHeader = &pLsiLogic->ConfigurationPages.SpiPages.aPortPages[u8Port].SCSISPIPortPage2.u.fields.Header;
            *ppbPageData  =  pLsiLogic->ConfigurationPages.SpiPages.aPortPages[u8Port].SCSISPIPortPage2.u.abPageData;
            *pcbPage      = sizeof(pLsiLogic->ConfigurationPages.SpiPages.aPortPages[u8Port].SCSISPIPortPage2);
            break;
        default:
            rc = VERR_NOT_FOUND;
    }

    return rc;
}

/**
 * Return the configuration page header and data
 * which matches the given page type and number.
 *
 * @returns VINF_SUCCESS if successful
 *          VERR_NOT_FOUND if the requested page could be found.
 * @param   u8PageNumber  Number of the page to get.
 * @param   ppPageHeader  Where to store the pointer to the page header.
 * @param   ppbPageData   Where to store the pointer to the page data.
 */
static int lsilogicConfigurationSCSISPIDevicePageGetFromNumber(PLSILOGICSCSI pLsiLogic, uint8_t u8Bus,
                                                               uint8_t u8TargetID, uint8_t u8PageNumber,
                                                               PMptConfigurationPageHeader *ppPageHeader,
                                                               uint8_t **ppbPageData, size_t *pcbPage)
{
    int rc = VINF_SUCCESS;

    AssertMsg(VALID_PTR(ppPageHeader) && VALID_PTR(ppbPageData), ("Invalid parameters\n"));

    if (u8Bus >= RT_ELEMENTS(pLsiLogic->ConfigurationPages.SpiPages.aBuses))
        return VERR_NOT_FOUND;

    if (u8TargetID >= RT_ELEMENTS(pLsiLogic->ConfigurationPages.SpiPages.aBuses[u8Bus].aDevicePages))
        return VERR_NOT_FOUND;

    switch(u8PageNumber)
    {
        case 0:
            *ppPageHeader = &pLsiLogic->ConfigurationPages.SpiPages.aBuses[u8Bus].aDevicePages[u8TargetID].SCSISPIDevicePage0.u.fields.Header;
            *ppbPageData  =  pLsiLogic->ConfigurationPages.SpiPages.aBuses[u8Bus].aDevicePages[u8TargetID].SCSISPIDevicePage0.u.abPageData;
            *pcbPage      = sizeof(pLsiLogic->ConfigurationPages.SpiPages.aBuses[u8Bus].aDevicePages[u8TargetID].SCSISPIDevicePage0);
            break;
        case 1:
            *ppPageHeader = &pLsiLogic->ConfigurationPages.SpiPages.aBuses[u8Bus].aDevicePages[u8TargetID].SCSISPIDevicePage1.u.fields.Header;
            *ppbPageData  =  pLsiLogic->ConfigurationPages.SpiPages.aBuses[u8Bus].aDevicePages[u8TargetID].SCSISPIDevicePage1.u.abPageData;
            *pcbPage      = sizeof(pLsiLogic->ConfigurationPages.SpiPages.aBuses[u8Bus].aDevicePages[u8TargetID].SCSISPIDevicePage1);
            break;
        case 2:
            *ppPageHeader = &pLsiLogic->ConfigurationPages.SpiPages.aBuses[u8Bus].aDevicePages[u8TargetID].SCSISPIDevicePage2.u.fields.Header;
            *ppbPageData  =  pLsiLogic->ConfigurationPages.SpiPages.aBuses[u8Bus].aDevicePages[u8TargetID].SCSISPIDevicePage2.u.abPageData;
            *pcbPage      = sizeof(pLsiLogic->ConfigurationPages.SpiPages.aBuses[u8Bus].aDevicePages[u8TargetID].SCSISPIDevicePage2);
            break;
        case 3:
            *ppPageHeader = &pLsiLogic->ConfigurationPages.SpiPages.aBuses[u8Bus].aDevicePages[u8TargetID].SCSISPIDevicePage3.u.fields.Header;
            *ppbPageData  =  pLsiLogic->ConfigurationPages.SpiPages.aBuses[u8Bus].aDevicePages[u8TargetID].SCSISPIDevicePage3.u.abPageData;
            *pcbPage      = sizeof(pLsiLogic->ConfigurationPages.SpiPages.aBuses[u8Bus].aDevicePages[u8TargetID].SCSISPIDevicePage3);
            break;
        default:
            rc = VERR_NOT_FOUND;
    }

    return rc;
}

/**
 * Processes a Configuration request.
 *
 * @returns VBox status code.
 * @param   pLsiLogic             Pointer to the device instance which sends the request.
 * @param   pConfigurationReq     Pointer to the request structure.
 * @param   pReply                Pointer to the reply message frame
 */
static int lsilogicProcessConfigurationRequest(PLSILOGICSCSI pLsiLogic, PMptConfigurationRequest pConfigurationReq,
                                               PMptConfigurationReply pReply)
{
    int rc = VINF_SUCCESS;
    uint8_t                     *pbPageData;
    PMptConfigurationPageHeader pPageHeader;
    uint8_t                     u8PageType, u8PageAttribute;
    size_t                      cbPage;

    LogFlowFunc(("pLsiLogic=%#p\n", pLsiLogic));

    u8PageType = MPT_CONFIGURATION_PAGE_TYPE_GET(pConfigurationReq->u8PageType);
    u8PageAttribute = MPT_CONFIGURATION_PAGE_ATTRIBUTE_GET(pConfigurationReq->u8PageType);

    /* Copy common bits from the request into the reply. */
    pReply->u8MessageLength   = 6; /* 6 32bit D-Words. */
    pReply->u8Action          = pConfigurationReq->u8Action;
    pReply->u8Function        = pConfigurationReq->u8Function;
    pReply->u32MessageContext = pConfigurationReq->u32MessageContext;

    switch (u8PageType)
    {
        case MPT_CONFIGURATION_PAGE_TYPE_IO_UNIT:
        {
            /* Get the page data. */
            rc = lsilogicConfigurationIOUnitPageGetFromNumber(pLsiLogic,
                                                              pConfigurationReq->u8PageNumber,
                                                              &pPageHeader, &pbPageData, &cbPage);
            break;
        }
        case MPT_CONFIGURATION_PAGE_TYPE_IOC:
        {
            /* Get the page data. */
            rc = lsilogicConfigurationIOCPageGetFromNumber(pLsiLogic,
                                                           pConfigurationReq->u8PageNumber,
                                                           &pPageHeader, &pbPageData, &cbPage);
            break;
        }
        case MPT_CONFIGURATION_PAGE_TYPE_MANUFACTURING:
        {
            /* Get the page data. */
            rc = lsilogicConfigurationManufacturingPageGetFromNumber(pLsiLogic,
                                                                     pConfigurationReq->u8PageNumber,
                                                                     &pPageHeader, &pbPageData, &cbPage);
            break;
        }
        case MPT_CONFIGURATION_PAGE_TYPE_SCSI_SPI_PORT:
        {
            /* Get the page data. */
            rc = lsilogicConfigurationSCSISPIPortPageGetFromNumber(pLsiLogic,
                                                                   pConfigurationReq->u.MPIPortNumber.u8PortNumber,
                                                                   pConfigurationReq->u8PageNumber,
                                                                   &pPageHeader, &pbPageData, &cbPage);
            break;
        }
        case MPT_CONFIGURATION_PAGE_TYPE_SCSI_SPI_DEVICE:
        {
            /* Get the page data. */
            rc = lsilogicConfigurationSCSISPIDevicePageGetFromNumber(pLsiLogic,
                                                                     pConfigurationReq->u.BusAndTargetId.u8Bus,
                                                                     pConfigurationReq->u.BusAndTargetId.u8TargetID,
                                                                     pConfigurationReq->u8PageNumber,
                                                                     &pPageHeader, &pbPageData, &cbPage);
            break;
        }
        default:
            rc = VERR_NOT_FOUND;
    }

    if (rc == VERR_NOT_FOUND)
    {
        //AssertMsgFailed(("todo\n"));
        pReply->u8PageType    = pConfigurationReq->u8PageType;
        pReply->u8PageNumber  = pConfigurationReq->u8PageNumber;
        pReply->u8PageLength  = pConfigurationReq->u8PageLength;
        pReply->u8PageVersion = pConfigurationReq->u8PageVersion;
        return VINF_SUCCESS;
    }

    pReply->u8PageType    = pPageHeader->u8PageType;
    pReply->u8PageNumber  = pPageHeader->u8PageNumber;
    pReply->u8PageLength  = pPageHeader->u8PageLength;
    pReply->u8PageVersion = pPageHeader->u8PageVersion;

    Log(("GuestRequest u8Action=%d\n", pConfigurationReq->u8Action));
    Log(("u8PageType=%d\n", pPageHeader->u8PageType));
    Log(("u8PageNumber=%d\n", pPageHeader->u8PageNumber));
    Log(("u8PageLength=%d\n", pPageHeader->u8PageLength));
    Log(("u8PageVersion=%d\n", pPageHeader->u8PageVersion));

    for (int i = 0; i < pReply->u8PageLength; i++)
        LogFlowFunc(("PageData[%d]=%#x\n", i, ((uint32_t *)pbPageData)[i]));

    /*
     * Don't use the scatter gather handling code as the configuration request always have only one
     * simple element.
     */
    switch (pConfigurationReq->u8Action)
    {
        case MPT_CONFIGURATION_REQUEST_ACTION_DEFAULT: /* Nothing to do. We are always using the defaults. */
        case MPT_CONFIGURATION_REQUEST_ACTION_HEADER:
        {
            /* Already copied above nothing to do. */
            break;
        }
        case MPT_CONFIGURATION_REQUEST_ACTION_READ_NVRAM:
        case MPT_CONFIGURATION_REQUEST_ACTION_READ_CURRENT:
        case MPT_CONFIGURATION_REQUEST_ACTION_READ_DEFAULT:
        {
            uint32_t cbBuffer = pConfigurationReq->SimpleSGElement.u24Length;
            if (cbBuffer != 0)
            {
                RTGCPHYS GCPhysAddrPageBuffer = pConfigurationReq->SimpleSGElement.u32DataBufferAddressLow;
                if (pConfigurationReq->SimpleSGElement.f64BitAddress)
                    GCPhysAddrPageBuffer |= (uint64_t)pConfigurationReq->SimpleSGElement.u32DataBufferAddressHigh << 32;

                PDMDevHlpPhysWrite(pLsiLogic->CTX_SUFF(pDevIns), GCPhysAddrPageBuffer, pbPageData,
                                   cbBuffer < cbPage ? cbBuffer : cbPage);
            }
            break;
        }
        case MPT_CONFIGURATION_REQUEST_ACTION_WRITE_CURRENT:
        case MPT_CONFIGURATION_REQUEST_ACTION_WRITE_NVRAM:
        {
            uint32_t cbBuffer = pConfigurationReq->SimpleSGElement.u24Length;
            if (cbBuffer != 0)
            {
                RTGCPHYS GCPhysAddrPageBuffer = pConfigurationReq->SimpleSGElement.u32DataBufferAddressLow;
                if (pConfigurationReq->SimpleSGElement.f64BitAddress)
                    GCPhysAddrPageBuffer |= (uint64_t)pConfigurationReq->SimpleSGElement.u32DataBufferAddressHigh << 32;

                PDMDevHlpPhysRead(pLsiLogic->CTX_SUFF(pDevIns), GCPhysAddrPageBuffer, pbPageData,
                                  cbBuffer < cbPage ? cbBuffer : cbPage);
            }
            break;
        }
        default:
            AssertMsgFailed(("todo\n"));
    }

    return VINF_SUCCESS;
}

/**
 * Initializes the configuration pages.
 *
 * @returns nothing
 * @param   pLsiLogic    Pointer to the Lsilogic SCSI instance.
 */
static void lsilogicInitializeConfigurationPages(PLSILOGICSCSI pLsiLogic)
{
    PMptConfigurationPagesSupportedSpi pPages = &pLsiLogic->ConfigurationPages.SpiPages;

    LogFlowFunc(("pLsiLogic=%#p\n", pLsiLogic));

    /* Clear everything first. */
    memset(pPages, 0, sizeof(MptConfigurationPagesSupportedSpi));

    /* Manufacturing Page 0. */
    pPages->ManufacturingPage0.u.fields.Header.u8PageType   =   MPT_CONFIGURATION_PAGE_ATTRIBUTE_PERSISTENT_READONLY
                                                              | MPT_CONFIGURATION_PAGE_TYPE_MANUFACTURING;
    pPages->ManufacturingPage0.u.fields.Header.u8PageNumber = 0;
    pPages->ManufacturingPage0.u.fields.Header.u8PageLength = sizeof(MptConfigurationPageManufacturing0) / 4;
    strncpy((char *)pPages->ManufacturingPage0.u.fields.abChipName,          "VBox MPT Fusion", 16);
    strncpy((char *)pPages->ManufacturingPage0.u.fields.abChipRevision,      "1.0", 8);
    strncpy((char *)pPages->ManufacturingPage0.u.fields.abBoardName,         "VBox MPT Fusion", 16);
    strncpy((char *)pPages->ManufacturingPage0.u.fields.abBoardAssembly,     "SUN", 8);
    strncpy((char *)pPages->ManufacturingPage0.u.fields.abBoardTracerNumber, "CAFECAFECAFECAFE", 16);

    /* Manufacturing Page 1 - I don't know what this contains so we leave it 0 for now. */
    pPages->ManufacturingPage1.Header.u8PageType   =   MPT_CONFIGURATION_PAGE_ATTRIBUTE_PERSISTENT_READONLY
                                                     | MPT_CONFIGURATION_PAGE_TYPE_MANUFACTURING;
    pPages->ManufacturingPage1.Header.u8PageNumber = 1;
    pPages->ManufacturingPage1.Header.u8PageLength = sizeof(MptConfigurationPageManufacturing1) / 4;

    /* Manufacturing Page 2. */
    pPages->ManufacturingPage2.u.fields.Header.u8PageType   =   MPT_CONFIGURATION_PAGE_ATTRIBUTE_READONLY
                                                              | MPT_CONFIGURATION_PAGE_TYPE_MANUFACTURING;
    pPages->ManufacturingPage2.u.fields.Header.u8PageNumber = 2;
    pPages->ManufacturingPage2.u.fields.Header.u8PageLength = sizeof(MptConfigurationPageManufacturing2) / 4;
    pPages->ManufacturingPage2.u.fields.u16PCIDeviceID = LSILOGICSCSI_PCI_SPI_DEVICE_ID;
    pPages->ManufacturingPage2.u.fields.u8PCIRevisionID = LSILOGICSCSI_PCI_SPI_REVISION_ID;
    /* Hardware specific settings - everything 0 for now. */

    /* Manufacturing Page 3. */
    pPages->ManufacturingPage3.u.fields.Header.u8PageType   =   MPT_CONFIGURATION_PAGE_ATTRIBUTE_READONLY
                                                              | MPT_CONFIGURATION_PAGE_TYPE_MANUFACTURING;
    pPages->ManufacturingPage3.u.fields.Header.u8PageNumber = 3;
    pPages->ManufacturingPage3.u.fields.Header.u8PageLength = sizeof(MptConfigurationPageManufacturing3) / 4;
    pPages->ManufacturingPage3.u.fields.u16PCIDeviceID = LSILOGICSCSI_PCI_SPI_DEVICE_ID;
    pPages->ManufacturingPage3.u.fields.u8PCIRevisionID = LSILOGICSCSI_PCI_SPI_REVISION_ID;
    /* Chip specific settings - everything 0 for now. */

    /* Manufacturing Page 4 - I don't know what this contains so we leave it 0 for now. */
    pPages->ManufacturingPage4.u.fields.Header.u8PageType   =   MPT_CONFIGURATION_PAGE_ATTRIBUTE_PERSISTENT_READONLY
                                                              | MPT_CONFIGURATION_PAGE_TYPE_MANUFACTURING;
    pPages->ManufacturingPage4.u.fields.Header.u8PageNumber = 4;
    pPages->ManufacturingPage4.u.fields.Header.u8PageLength = sizeof(MptConfigurationPageManufacturing4) / 4;


    /* I/O Unit page 0. */
    pPages->IOUnitPage0.u.fields.Header.u8PageType   =   MPT_CONFIGURATION_PAGE_ATTRIBUTE_READONLY
                                                       | MPT_CONFIGURATION_PAGE_TYPE_IO_UNIT;
    pPages->IOUnitPage0.u.fields.Header.u8PageNumber = 0;
    pPages->IOUnitPage0.u.fields.Header.u8PageLength = sizeof(MptConfigurationPageIOUnit0) / 4;
    pPages->IOUnitPage0.u.fields.u64UniqueIdentifier = 0xcafe;

    /* I/O Unit page 1. */
    pPages->IOUnitPage1.u.fields.Header.u8PageType   =   MPT_CONFIGURATION_PAGE_ATTRIBUTE_READONLY
                                                       | MPT_CONFIGURATION_PAGE_TYPE_IO_UNIT;
    pPages->IOUnitPage1.u.fields.Header.u8PageNumber = 1;
    pPages->IOUnitPage1.u.fields.Header.u8PageLength = sizeof(MptConfigurationPageIOUnit1) / 4;
    pPages->IOUnitPage1.u.fields.fSingleFunction         = true;
    pPages->IOUnitPage1.u.fields.fAllPathsMapped         = false;
    pPages->IOUnitPage1.u.fields.fIntegratedRAIDDisabled = true;
    pPages->IOUnitPage1.u.fields.f32BitAccessForced      = false;

    /* I/O Unit page 2. */
    pPages->IOUnitPage2.u.fields.Header.u8PageType   =   MPT_CONFIGURATION_PAGE_ATTRIBUTE_PERSISTENT
                                                       | MPT_CONFIGURATION_PAGE_TYPE_IO_UNIT;
    pPages->IOUnitPage2.u.fields.Header.u8PageNumber = 2;
    pPages->IOUnitPage2.u.fields.Header.u8PageLength = sizeof(MptConfigurationPageIOUnit2) / 4;
    pPages->IOUnitPage2.u.fields.fPauseOnError       = false;
    pPages->IOUnitPage2.u.fields.fVerboseModeEnabled = false;
    pPages->IOUnitPage2.u.fields.fDisableColorVideo  = false;
    pPages->IOUnitPage2.u.fields.fNotHookInt40h      = false;
    pPages->IOUnitPage2.u.fields.u32BIOSVersion      = 0xcafecafe;
    pPages->IOUnitPage2.u.fields.aAdapterOrder[0].fAdapterEnabled = true;
    pPages->IOUnitPage2.u.fields.aAdapterOrder[0].fAdapterEmbedded = true;
    pPages->IOUnitPage2.u.fields.aAdapterOrder[0].u8PCIBusNumber = 0;
    pPages->IOUnitPage2.u.fields.aAdapterOrder[0].u8PCIDevFn     = pLsiLogic->PciDev.devfn;

    /* I/O Unit page 3. */
    pPages->IOUnitPage3.u.fields.Header.u8PageType   =   MPT_CONFIGURATION_PAGE_ATTRIBUTE_CHANGEABLE
                                                       | MPT_CONFIGURATION_PAGE_TYPE_IO_UNIT;
    pPages->IOUnitPage3.u.fields.Header.u8PageNumber = 3;
    pPages->IOUnitPage3.u.fields.Header.u8PageLength = sizeof(MptConfigurationPageIOUnit3) / 4;
    pPages->IOUnitPage3.u.fields.u8GPIOCount = 0;

    /* IOC page 0. */
    pPages->IOCPage0.u.fields.Header.u8PageType   =   MPT_CONFIGURATION_PAGE_ATTRIBUTE_READONLY
                                                    | MPT_CONFIGURATION_PAGE_TYPE_IOC;
    pPages->IOCPage0.u.fields.Header.u8PageNumber = 0;
    pPages->IOCPage0.u.fields.Header.u8PageLength = sizeof(MptConfigurationPageIOC0) / 4;
    pPages->IOCPage0.u.fields.u32TotalNVStore      = 0;
    pPages->IOCPage0.u.fields.u32FreeNVStore       = 0;
    pPages->IOCPage0.u.fields.u16VendorId          = LSILOGICSCSI_PCI_VENDOR_ID;
    pPages->IOCPage0.u.fields.u16DeviceId          = LSILOGICSCSI_PCI_SPI_DEVICE_ID;
    pPages->IOCPage0.u.fields.u8RevisionId         = LSILOGICSCSI_PCI_SPI_REVISION_ID;
    pPages->IOCPage0.u.fields.u32ClassCode         = LSILOGICSCSI_PCI_SPI_CLASS_CODE;
    pPages->IOCPage0.u.fields.u16SubsystemVendorId = LSILOGICSCSI_PCI_SPI_SUBSYSTEM_VENDOR_ID;
    pPages->IOCPage0.u.fields.u16SubsystemId       = LSILOGICSCSI_PCI_SPI_SUBSYSTEM_ID;

    /* IOC page 1. */
    pPages->IOCPage1.u.fields.Header.u8PageType   =   MPT_CONFIGURATION_PAGE_ATTRIBUTE_CHANGEABLE
                                                    | MPT_CONFIGURATION_PAGE_TYPE_IOC;
    pPages->IOCPage1.u.fields.Header.u8PageNumber = 1;
    pPages->IOCPage1.u.fields.Header.u8PageLength = sizeof(MptConfigurationPageIOC1) / 4;
    pPages->IOCPage1.u.fields.fReplyCoalescingEnabled = false;
    pPages->IOCPage1.u.fields.u32CoalescingTimeout    = 0;
    pPages->IOCPage1.u.fields.u8CoalescingDepth       = 0;

    /* IOC page 2. */
    pPages->IOCPage2.u.fields.Header.u8PageType   =   MPT_CONFIGURATION_PAGE_ATTRIBUTE_READONLY
                                                    | MPT_CONFIGURATION_PAGE_TYPE_IOC;
    pPages->IOCPage2.u.fields.Header.u8PageNumber = 2;
    pPages->IOCPage2.u.fields.Header.u8PageLength = sizeof(MptConfigurationPageIOC2) / 4;
    /* Everything else here is 0. */

    /* IOC page 3. */
    pPages->IOCPage3.u.fields.Header.u8PageType   =   MPT_CONFIGURATION_PAGE_ATTRIBUTE_READONLY
                                                    | MPT_CONFIGURATION_PAGE_TYPE_IOC;
    pPages->IOCPage3.u.fields.Header.u8PageNumber = 3;
    pPages->IOCPage3.u.fields.Header.u8PageLength = sizeof(MptConfigurationPageIOC3) / 4;
    /* Everything else here is 0. */

    /* IOC page 4. */
    pPages->IOCPage4.u.fields.Header.u8PageType   =   MPT_CONFIGURATION_PAGE_ATTRIBUTE_READONLY
                                                    | MPT_CONFIGURATION_PAGE_TYPE_IOC;
    pPages->IOCPage4.u.fields.Header.u8PageNumber = 4;
    pPages->IOCPage4.u.fields.Header.u8PageLength = sizeof(MptConfigurationPageIOC4) / 4;
    /* Everything else here is 0. */

    /* IOC page 6. */
    pPages->IOCPage6.u.fields.Header.u8PageType   =   MPT_CONFIGURATION_PAGE_ATTRIBUTE_READONLY
                                                    | MPT_CONFIGURATION_PAGE_TYPE_IOC;
    pPages->IOCPage6.u.fields.Header.u8PageNumber = 6;
    pPages->IOCPage6.u.fields.Header.u8PageLength = sizeof(MptConfigurationPageIOC6) / 4;
    /* Everything else here is 0. */

    for (unsigned i = 0; i < RT_ELEMENTS(pPages->aPortPages); i++)
    {
        /* SCSI-SPI port page 0. */
        pPages->aPortPages[i].SCSISPIPortPage0.u.fields.Header.u8PageType   =   MPT_CONFIGURATION_PAGE_ATTRIBUTE_READONLY
                                                                              | MPT_CONFIGURATION_PAGE_TYPE_SCSI_SPI_PORT;
        pPages->aPortPages[i].SCSISPIPortPage0.u.fields.Header.u8PageNumber = 0;
        pPages->aPortPages[i].SCSISPIPortPage0.u.fields.Header.u8PageLength = sizeof(MptConfigurationPageSCSISPIPort0) / 4;
        pPages->aPortPages[i].SCSISPIPortPage0.u.fields.fInformationUnitTransfersCapable = true;
        pPages->aPortPages[i].SCSISPIPortPage0.u.fields.fDTCapable                       = true;
        pPages->aPortPages[i].SCSISPIPortPage0.u.fields.fQASCapable                      = true;
        pPages->aPortPages[i].SCSISPIPortPage0.u.fields.u8MinimumSynchronousTransferPeriod =  0;
        pPages->aPortPages[i].SCSISPIPortPage0.u.fields.u8MaximumSynchronousOffset         = 0xff;
        pPages->aPortPages[i].SCSISPIPortPage0.u.fields.fWide                              = true;
        pPages->aPortPages[i].SCSISPIPortPage0.u.fields.fAIPCapable                        = true;
        pPages->aPortPages[i].SCSISPIPortPage0.u.fields.u2SignalingType                    = 0x3; /* Single Ended. */

        /* SCSI-SPI port page 1. */
        pPages->aPortPages[i].SCSISPIPortPage1.u.fields.Header.u8PageType   =   MPT_CONFIGURATION_PAGE_ATTRIBUTE_CHANGEABLE
                                                                              | MPT_CONFIGURATION_PAGE_TYPE_SCSI_SPI_PORT;
        pPages->aPortPages[i].SCSISPIPortPage1.u.fields.Header.u8PageNumber = 1;
        pPages->aPortPages[i].SCSISPIPortPage1.u.fields.Header.u8PageLength = sizeof(MptConfigurationPageSCSISPIPort1) / 4;
        pPages->aPortPages[i].SCSISPIPortPage1.u.fields.u8SCSIID                  = 7;
        pPages->aPortPages[i].SCSISPIPortPage1.u.fields.u16PortResponseIDsBitmask = (1 << 7);
        pPages->aPortPages[i].SCSISPIPortPage1.u.fields.u32OnBusTimerValue        =  0;

        /* SCSI-SPI port page 2. */
        pPages->aPortPages[i].SCSISPIPortPage2.u.fields.Header.u8PageType   =   MPT_CONFIGURATION_PAGE_ATTRIBUTE_CHANGEABLE
                                                                              | MPT_CONFIGURATION_PAGE_TYPE_SCSI_SPI_PORT;
        pPages->aPortPages[i].SCSISPIPortPage2.u.fields.Header.u8PageNumber = 2;
        pPages->aPortPages[i].SCSISPIPortPage2.u.fields.Header.u8PageLength = sizeof(MptConfigurationPageSCSISPIPort2) / 4;
        pPages->aPortPages[i].SCSISPIPortPage2.u.fields.u4HostSCSIID           = 7;
        pPages->aPortPages[i].SCSISPIPortPage2.u.fields.u2InitializeHBA        = 0x3;
        pPages->aPortPages[i].SCSISPIPortPage2.u.fields.fTerminationDisabled   = true;
        for (unsigned iDevice = 0; iDevice < RT_ELEMENTS(pPages->aPortPages[i].SCSISPIPortPage2.u.fields.aDeviceSettings); iDevice++)
        {
            pPages->aPortPages[i].SCSISPIPortPage2.u.fields.aDeviceSettings[iDevice].fBootChoice   = true;
        }
        /* Everything else 0 for now. */
    }

    for (unsigned uBusCurr = 0; uBusCurr < RT_ELEMENTS(pPages->aBuses); uBusCurr++)
    {
        for (unsigned uDeviceCurr = 0; uDeviceCurr < RT_ELEMENTS(pPages->aBuses[uBusCurr].aDevicePages); uDeviceCurr++)
        {
            /* SCSI-SPI device page 0. */
            pPages->aBuses[uBusCurr].aDevicePages[uDeviceCurr].SCSISPIDevicePage0.u.fields.Header.u8PageType   =   MPT_CONFIGURATION_PAGE_ATTRIBUTE_READONLY
                                                                                                                 | MPT_CONFIGURATION_PAGE_TYPE_SCSI_SPI_DEVICE;
            pPages->aBuses[uBusCurr].aDevicePages[uDeviceCurr].SCSISPIDevicePage0.u.fields.Header.u8PageNumber = 0;
            pPages->aBuses[uBusCurr].aDevicePages[uDeviceCurr].SCSISPIDevicePage0.u.fields.Header.u8PageLength = sizeof(MptConfigurationPageSCSISPIDevice0) / 4;
            /* Everything else 0 for now. */

            /* SCSI-SPI device page 1. */
            pPages->aBuses[uBusCurr].aDevicePages[uDeviceCurr].SCSISPIDevicePage1.u.fields.Header.u8PageType   =   MPT_CONFIGURATION_PAGE_ATTRIBUTE_CHANGEABLE
                                                                                                                 | MPT_CONFIGURATION_PAGE_TYPE_SCSI_SPI_DEVICE;
            pPages->aBuses[uBusCurr].aDevicePages[uDeviceCurr].SCSISPIDevicePage1.u.fields.Header.u8PageNumber = 1;
            pPages->aBuses[uBusCurr].aDevicePages[uDeviceCurr].SCSISPIDevicePage1.u.fields.Header.u8PageLength = sizeof(MptConfigurationPageSCSISPIDevice1) / 4;
            /* Everything else 0 for now. */

            /* SCSI-SPI device page 2. */
            pPages->aBuses[uBusCurr].aDevicePages[uDeviceCurr].SCSISPIDevicePage2.u.fields.Header.u8PageType   =   MPT_CONFIGURATION_PAGE_ATTRIBUTE_CHANGEABLE
                                                                                                                 | MPT_CONFIGURATION_PAGE_TYPE_SCSI_SPI_DEVICE;
            pPages->aBuses[uBusCurr].aDevicePages[uDeviceCurr].SCSISPIDevicePage2.u.fields.Header.u8PageNumber = 2;
            pPages->aBuses[uBusCurr].aDevicePages[uDeviceCurr].SCSISPIDevicePage2.u.fields.Header.u8PageLength = sizeof(MptConfigurationPageSCSISPIDevice2) / 4;
            /* Everything else 0 for now. */

            pPages->aBuses[uBusCurr].aDevicePages[uDeviceCurr].SCSISPIDevicePage3.u.fields.Header.u8PageType   =   MPT_CONFIGURATION_PAGE_ATTRIBUTE_READONLY
                                                                                                                 | MPT_CONFIGURATION_PAGE_TYPE_SCSI_SPI_DEVICE;
            pPages->aBuses[uBusCurr].aDevicePages[uDeviceCurr].SCSISPIDevicePage3.u.fields.Header.u8PageNumber = 3;
            pPages->aBuses[uBusCurr].aDevicePages[uDeviceCurr].SCSISPIDevicePage3.u.fields.Header.u8PageLength = sizeof(MptConfigurationPageSCSISPIDevice3) / 4;
            /* Everything else 0 for now. */
        }
    }
}

/**
 * Transmit queue consumer
 * Queue a new async task.
 *
 * @returns Success indicator.
 *          If false the item will not be removed and the flushing will stop.
 * @param   pDevIns     The device instance.
 * @param   pItem       The item to consume. Upon return this item will be freed.
 */
static DECLCALLBACK(bool) lsilogicNotifyQueueConsumer(PPDMDEVINS pDevIns, PPDMQUEUEITEMCORE pItem)
{
    PLSILOGICSCSI pLsiLogic = PDMINS_2_DATA(pDevIns, PLSILOGICSCSI);
    int rc = VINF_SUCCESS;

    LogFlowFunc(("pDevIns=%#p pItem=%#p\n", pDevIns, pItem));

    /* Only process request which arrived before we received the notification. */
    uint32_t uRequestQueueNextEntryWrite = ASMAtomicReadU32(&pLsiLogic->uRequestQueueNextEntryFreeWrite);

    /* Reset notification event. */
    ASMAtomicXchgBool(&pLsiLogic->fNotificationSend, false);

    /* Go through the messages now and process them. */
    while (   RT_LIKELY(pLsiLogic->enmState == LSILOGICSTATE_OPERATIONAL)
           && (pLsiLogic->uRequestQueueNextAddressRead != uRequestQueueNextEntryWrite))
    {
        uint32_t  u32RequestMessageFrameDesc = pLsiLogic->CTX_SUFF(pRequestQueueBase)[pLsiLogic->uRequestQueueNextAddressRead];
        RTGCPHYS  GCPhysMessageFrameAddr = LSILOGIC_RTGCPHYS_FROM_U32(pLsiLogic->u32HostMFAHighAddr,
                                                                      (u32RequestMessageFrameDesc & ~0x07));

        PLSILOGICTASKSTATE pTaskState;

        /* Get new task state. */
        rc = RTCacheRequest(pLsiLogic->pTaskCache, (void **)&pTaskState);
        AssertRC(rc);

        pTaskState->GCPhysMessageFrameAddr = GCPhysMessageFrameAddr;

        /* Read the message header from the guest first. */
        PDMDevHlpPhysRead(pDevIns, GCPhysMessageFrameAddr, &pTaskState->GuestRequest, sizeof(MptMessageHdr));

        /* Determine the size of the request. */
        uint32_t cbRequest = 0;

        switch (pTaskState->GuestRequest.Header.u8Function)
        {
            case MPT_MESSAGE_HDR_FUNCTION_SCSI_IO_REQUEST:
                cbRequest = sizeof(MptSCSIIORequest);
                break;
            case MPT_MESSAGE_HDR_FUNCTION_SCSI_TASK_MGMT:
                cbRequest = sizeof(MptSCSITaskManagementRequest);
                break;
            case MPT_MESSAGE_HDR_FUNCTION_IOC_INIT:
                cbRequest = sizeof(MptIOCInitRequest);
                break;
            case MPT_MESSAGE_HDR_FUNCTION_IOC_FACTS:
                cbRequest = sizeof(MptIOCFactsRequest);
                break;
            case MPT_MESSAGE_HDR_FUNCTION_CONFIG:
                cbRequest = sizeof(MptConfigurationRequest);
                break;
            case MPT_MESSAGE_HDR_FUNCTION_PORT_FACTS:
                cbRequest = sizeof(MptPortFactsRequest);
                break;
            case MPT_MESSAGE_HDR_FUNCTION_PORT_ENABLE:
                cbRequest = sizeof(MptPortEnableRequest);
                break;
            case MPT_MESSAGE_HDR_FUNCTION_EVENT_NOTIFICATION:
                cbRequest = sizeof(MptEventNotificationRequest);
                break;
            case MPT_MESSAGE_HDR_FUNCTION_EVENT_ACK:
                AssertMsgFailed(("todo\n"));
                //cbRequest = sizeof(MptEventAckRequest);
                break;
            case MPT_MESSAGE_HDR_FUNCTION_FW_DOWNLOAD:
                AssertMsgFailed(("todo\n"));
                break;
            default:
                AssertMsgFailed(("Unknown function issued %u\n", pTaskState->GuestRequest.Header.u8Function));
                lsilogicSetIOCFaultCode(pLsiLogic, LSILOGIC_IOCSTATUS_INVALID_FUNCTION);
        }

        if (cbRequest != 0)
        {
            /* Read the complete message frame from guest memory now. */
            PDMDevHlpPhysRead(pDevIns, GCPhysMessageFrameAddr, &pTaskState->GuestRequest, cbRequest);

            /* Handle SCSI I/O requests now. */
            if (pTaskState->GuestRequest.Header.u8Function == MPT_MESSAGE_HDR_FUNCTION_SCSI_IO_REQUEST)
            {
               rc = lsilogicProcessSCSIIORequest(pLsiLogic, pTaskState);
               AssertRC(rc);
            }
            else
            {
                MptReplyUnion Reply;
                rc = lsilogicProcessMessageRequest(pLsiLogic, &pTaskState->GuestRequest.Header, &Reply);
                AssertRC(rc);
                RTCacheInsert(pLsiLogic->pTaskCache, pTaskState);
            }

            pLsiLogic->uRequestQueueNextAddressRead++;
            pLsiLogic->uRequestQueueNextAddressRead %= pLsiLogic->cRequestQueueEntries;
        }
    }

    return true;
}

/**
 * Sets the emulated controller type from a given string.
 *
 * @returns VBox status code.
 *
 * @param   pThis        The LsiLogic devi state.
 * @param   pcszCtrlType The string to use.
 */
static int lsilogicGetCtrlTypeFromString(PLSILOGICSCSI pThis, const char *pcszCtrlType)
{
    int rc = VERR_INVALID_PARAMETER;

    if (!RTStrCmp(pcszCtrlType, LSILOGICSCSI_PCI_SPI_CTRLNAME))
    {
        pThis->enmCtrlType = LSILOGICCTRLTYPE_SCSI_SPI;
        rc = VINF_SUCCESS;
    }
    else if (!RTStrCmp(pcszCtrlType, LSILOGICSCSI_PCI_SAS_CTRLNAME))
    {
        pThis->enmCtrlType = LSILOGICCTRLTYPE_SCSI_SAS;
        rc = VINF_SUCCESS;
    }

    return rc;
}

/**
 * Port I/O Handler for IN operations - legacy port.
 *
 * @returns VBox status code.
 *
 * @param   pDevIns     The device instance.
 * @param   pvUser      User argument.
 * @param   uPort       Port number used for the IN operation.
 * @param   pu32        Where to store the result.
 * @param   cb          Number of bytes read.
 */
static int  lsilogicIsaIOPortRead (PPDMDEVINS pDevIns, void *pvUser,
                                   RTIOPORT Port, uint32_t *pu32, unsigned cb)
{
    int rc;
    PLSILOGICSCSI pLsiLogic = PDMINS_2_DATA(pDevIns, PLSILOGICSCSI);

    Assert(cb == 1);

    rc = vboxscsiReadRegister(&pLsiLogic->VBoxSCSI, (Port - LSILOGIC_ISA_IO_PORT), pu32);

    Log2(("%s: pu32=%p:{%.*Rhxs} iRegister=%d rc=%Rrc\n",
          __FUNCTION__, pu32, 1, pu32, (Port - LSILOGIC_ISA_IO_PORT), rc));

    return rc;
}

/**
 * Prepares a request from the BIOS.
 *
 * @returns VBox status code.
 * @param   pLsiLogic    Pointer to the LsiLogic device instance.
 */
static int lsilogicPrepareBIOSSCSIRequest(PLSILOGICSCSI pLsiLogic)
{
    int rc;
    PLSILOGICTASKSTATE pTaskState;
    uint32_t           uTargetDevice;

    rc = RTCacheRequest(pLsiLogic->pTaskCache, (void **)&pTaskState);
    AssertMsgRCReturn(rc, ("Getting task from cache failed rc=%Rrc\n", rc), rc);

    pTaskState->fBIOS = true;

    rc = vboxscsiSetupRequest(&pLsiLogic->VBoxSCSI, &pTaskState->PDMScsiRequest, &uTargetDevice);
    AssertMsgRCReturn(rc, ("Setting up SCSI request failed rc=%Rrc\n", rc), rc);

    pTaskState->PDMScsiRequest.pvUser = pTaskState;

    pTaskState->pTargetDevice = &pLsiLogic->aDeviceStates[uTargetDevice];

    if (!pTaskState->pTargetDevice->pDrvBase)
    {
        /* Device is not present. */
        AssertMsg(pTaskState->PDMScsiRequest.pbCDB[0] == SCSI_INQUIRY,
                    ("Device is not present but command is not inquiry\n"));

        SCSIINQUIRYDATA ScsiInquiryData;

        memset(&ScsiInquiryData, 0, sizeof(SCSIINQUIRYDATA));
        ScsiInquiryData.u5PeripheralDeviceType = SCSI_INQUIRY_DATA_PERIPHERAL_DEVICE_TYPE_UNKNOWN;
        ScsiInquiryData.u3PeripheralQualifier = SCSI_INQUIRY_DATA_PERIPHERAL_QUALIFIER_NOT_CONNECTED_NOT_SUPPORTED;

        memcpy(pLsiLogic->VBoxSCSI.pBuf, &ScsiInquiryData, 5);

        rc = vboxscsiRequestFinished(&pLsiLogic->VBoxSCSI, &pTaskState->PDMScsiRequest);
        AssertMsgRCReturn(rc, ("Finishing BIOS SCSI request failed rc=%Rrc\n", rc), rc);

        rc = RTCacheInsert(pLsiLogic->pTaskCache, pTaskState);
        AssertMsgRCReturn(rc, ("Getting task from cache failed rc=%Rrc\n", rc), rc);
    }
    else
    {
        ASMAtomicIncU32(&pTaskState->pTargetDevice->cOutstandingRequests);

        rc = pTaskState->pTargetDevice->pDrvSCSIConnector->pfnSCSIRequestSend(pTaskState->pTargetDevice->pDrvSCSIConnector,
                                                                                &pTaskState->PDMScsiRequest);
        AssertMsgRCReturn(rc, ("Sending request to SCSI layer failed rc=%Rrc\n", rc), rc);
    }

    return rc;
}

/**
 * Port I/O Handler for OUT operations - legacy port.
 *
 * @returns VBox status code.
 *
 * @param   pDevIns     The device instance.
 * @param   pvUser      User argument.
 * @param   uPort       Port number used for the IN operation.
 * @param   u32         The value to output.
 * @param   cb          The value size in bytes.
 */
static int lsilogicIsaIOPortWrite (PPDMDEVINS pDevIns, void *pvUser,
                                   RTIOPORT Port, uint32_t u32, unsigned cb)
{
    int rc;
    PLSILOGICSCSI pLsiLogic = PDMINS_2_DATA(pDevIns, PLSILOGICSCSI);

    Log2(("#%d %s: pvUser=%#p cb=%d u32=%#x Port=%#x\n",
          pDevIns->iInstance, __FUNCTION__, pvUser, cb, u32, Port));

    Assert(cb == 1);

    rc = vboxscsiWriteRegister(&pLsiLogic->VBoxSCSI, (Port - LSILOGIC_ISA_IO_PORT), (uint8_t)u32);
    if (rc == VERR_MORE_DATA)
    {
        rc = lsilogicPrepareBIOSSCSIRequest(pLsiLogic);
        AssertRC(rc);
    }
    else if (RT_FAILURE(rc))
        AssertMsgFailed(("Writing BIOS register failed %Rrc\n", rc));

    return VINF_SUCCESS;
}

/**
 * Port I/O Handler for primary port range OUT string operations.
 * @see FNIOMIOPORTOUTSTRING for details.
 */
static DECLCALLBACK(int) lsilogicIsaIOPortWriteStr(PPDMDEVINS pDevIns, void *pvUser, RTIOPORT Port, RTGCPTR *pGCPtrSrc, PRTGCUINTREG pcTransfer, unsigned cb)
{
    PLSILOGICSCSI pLsiLogic = PDMINS_2_DATA(pDevIns, PLSILOGICSCSI);
    int rc;

    Log2(("#%d %s: pvUser=%#p cb=%d Port=%#x\n",
          pDevIns->iInstance, __FUNCTION__, pvUser, cb, Port));

    rc = vboxscsiWriteString(pDevIns, &pLsiLogic->VBoxSCSI, (Port - LSILOGIC_ISA_IO_PORT),
                             pGCPtrSrc, pcTransfer, cb);
    if (rc == VERR_MORE_DATA)
    {
        rc = lsilogicPrepareBIOSSCSIRequest(pLsiLogic);
        AssertRC(rc);
    }
    else if (RT_FAILURE(rc))
        AssertMsgFailed(("Writing BIOS register failed %Rrc\n", rc));

    return rc;
}

/**
 * Port I/O Handler for primary port range IN string operations.
 * @see FNIOMIOPORTINSTRING for details.
 */
static DECLCALLBACK(int) lsilogicIsaIOPortReadStr(PPDMDEVINS pDevIns, void *pvUser, RTIOPORT Port, RTGCPTR *pGCPtrDst, PRTGCUINTREG pcTransfer, unsigned cb)
{
    PLSILOGICSCSI pLsiLogic = PDMINS_2_DATA(pDevIns, PLSILOGICSCSI);

    LogFlowFunc(("#%d %s: pvUser=%#p cb=%d Port=%#x\n",
                 pDevIns->iInstance, __FUNCTION__, pvUser, cb, Port));

    return vboxscsiReadString(pDevIns, &pLsiLogic->VBoxSCSI, (Port - LSILOGIC_ISA_IO_PORT),
                              pGCPtrDst, pcTransfer, cb);
}

static DECLCALLBACK(int) lsilogicMap(PPCIDEVICE pPciDev, /*unsigned*/ int iRegion,
                                     RTGCPHYS GCPhysAddress, uint32_t cb,
                                     PCIADDRESSSPACE enmType)
{
    PPDMDEVINS pDevIns = pPciDev->pDevIns;
    PLSILOGICSCSI  pThis = PDMINS_2_DATA(pDevIns, PLSILOGICSCSI);
    int   rc = VINF_SUCCESS;

    Log2(("%s: registering area at GCPhysAddr=%RGp cb=%u\n", __FUNCTION__, GCPhysAddress, cb));

    AssertMsg(   (enmType == PCI_ADDRESS_SPACE_MEM && cb >= LSILOGIC_PCI_SPACE_MEM_SIZE)
              || (enmType == PCI_ADDRESS_SPACE_IO  && cb >= LSILOGIC_PCI_SPACE_IO_SIZE),
              ("PCI region type and size do not match\n"));

    if ((enmType == PCI_ADDRESS_SPACE_MEM) && (iRegion == 1))
    {
        /* We use the assigned size here, because we currently only support page aligned MMIO ranges. */
        rc = PDMDevHlpMMIORegister(pDevIns, GCPhysAddress, cb, NULL,
                                   lsilogicMMIOWrite, lsilogicMMIORead, NULL, "LsiLogic");
        if (RT_FAILURE(rc))
            return rc;

        if (pThis->fR0Enabled)
        {
            rc = PDMDevHlpMMIORegisterR0(pDevIns, GCPhysAddress, cb, 0,
                                         "lsilogicMMIOWrite", "lsilogicMMIORead", NULL);
            if (RT_FAILURE(rc))
                return rc;
        }

        if (pThis->fGCEnabled)
        {
            rc = PDMDevHlpMMIORegisterGC(pDevIns, GCPhysAddress, cb, 0,
                                         "lsilogicMMIOWrite", "lsilogicMMIORead", NULL);
            if (RT_FAILURE(rc))
                return rc;
        }

        pThis->GCPhysMMIOBase = GCPhysAddress;
    }
    else if ((enmType == PCI_ADDRESS_SPACE_MEM) && (iRegion == 2))
    {
        /* We use the assigned size here, because we currently only support page aligned MMIO ranges. */
        rc = PDMDevHlpMMIORegister(pDevIns, GCPhysAddress, cb, NULL,
                                   lsilogicDiagnosticWrite, lsilogicDiagnosticRead, NULL, "LsiLogicDiag");
        if (RT_FAILURE(rc))
            return rc;

        if (pThis->fR0Enabled)
        {
            rc = PDMDevHlpMMIORegisterR0(pDevIns, GCPhysAddress, cb, 0,
                                         "lsilogicDiagnosticWrite", "lsilogicDiagnosticRead", NULL);
            if (RT_FAILURE(rc))
                return rc;
        }

        if (pThis->fGCEnabled)
        {
            rc = PDMDevHlpMMIORegisterGC(pDevIns, GCPhysAddress, cb, 0,
                                         "lsilogicDiagnosticWrite", "lsilogicDiagnosticRead", NULL);
            if (RT_FAILURE(rc))
                return rc;
        }
    }
    else if (enmType == PCI_ADDRESS_SPACE_IO)
    {
        rc = PDMDevHlpIOPortRegister(pDevIns, (RTIOPORT)GCPhysAddress, LSILOGIC_PCI_SPACE_IO_SIZE,
                                     NULL, lsilogicIOPortWrite, lsilogicIOPortRead, NULL, NULL, "LsiLogic");
        if (RT_FAILURE(rc))
            return rc;

        if (pThis->fR0Enabled)
        {
            rc = PDMDevHlpIOPortRegisterR0(pDevIns, (RTIOPORT)GCPhysAddress, LSILOGIC_PCI_SPACE_IO_SIZE,
                                           0, "lsilogicIOPortWrite", "lsilogicIOPortRead", NULL, NULL, "LsiLogic");
            if (RT_FAILURE(rc))
                return rc;
        }

        if (pThis->fGCEnabled)
        {
            rc = PDMDevHlpIOPortRegisterGC(pDevIns, (RTIOPORT)GCPhysAddress, LSILOGIC_PCI_SPACE_IO_SIZE,
                                           0, "lsilogicIOPortWrite", "lsilogicIOPortRead", NULL, NULL, "LsiLogic");
            if (RT_FAILURE(rc))
                return rc;
        }

        pThis->IOPortBase = (RTIOPORT)GCPhysAddress;
    }
    else
        AssertMsgFailed(("Invalid enmType=%d iRegion=%d\n", enmType, iRegion));

    return rc;
}

static DECLCALLBACK(int) lsilogicLiveExec(PPDMDEVINS pDevIns, PSSMHANDLE pSSM, uint32_t uPass)
{
    PLSILOGICSCSI pThis = PDMINS_2_DATA(pDevIns, PLSILOGICSCSI);

    /* Save the device config. */
    for (unsigned i = 0; i < RT_ELEMENTS(pThis->aDeviceStates); i++)
        SSMR3PutBool(pSSM, pThis->aDeviceStates[i].pDrvBase != NULL);

    return VINF_SSM_DONT_CALL_AGAIN;
}

static DECLCALLBACK(int) lsilogicSaveExec(PPDMDEVINS pDevIns, PSSMHANDLE pSSM)
{
    PLSILOGICSCSI pLsiLogic = PDMINS_2_DATA(pDevIns, PLSILOGICSCSI);

    /* Every device first. */
    lsilogicLiveExec(pDevIns, pSSM, SSM_PASS_FINAL);
    for (unsigned i = 0; i < RT_ELEMENTS(pLsiLogic->aDeviceStates); i++)
    {
        PLSILOGICDEVICE pDevice = &pLsiLogic->aDeviceStates[i];

        AssertMsg(!pDevice->cOutstandingRequests,
                  ("There are still outstanding requests on this device\n"));
        SSMR3PutU32(pSSM, pDevice->cOutstandingRequests);
    }
    /* Now the main device state. */
    SSMR3PutU32   (pSSM, pLsiLogic->enmState);
    SSMR3PutU32   (pSSM, pLsiLogic->enmWhoInit);
    SSMR3PutBool  (pSSM, pLsiLogic->fDoorbellInProgress);
    SSMR3PutBool  (pSSM, pLsiLogic->fDiagnosticEnabled);
    SSMR3PutBool  (pSSM, pLsiLogic->fNotificationSend);
    SSMR3PutBool  (pSSM, pLsiLogic->fEventNotificationEnabled);
    SSMR3PutU32   (pSSM, pLsiLogic->uInterruptMask);
    SSMR3PutU32   (pSSM, pLsiLogic->uInterruptStatus);
    for (unsigned i = 0; i < RT_ELEMENTS(pLsiLogic->aMessage); i++)
        SSMR3PutU32   (pSSM, pLsiLogic->aMessage[i]);
    SSMR3PutU32   (pSSM, pLsiLogic->iMessage);
    SSMR3PutU32   (pSSM, pLsiLogic->cMessage);
    SSMR3PutMem   (pSSM, &pLsiLogic->ReplyBuffer, sizeof(pLsiLogic->ReplyBuffer));
    SSMR3PutU32   (pSSM, pLsiLogic->uNextReplyEntryRead);
    SSMR3PutU32   (pSSM, pLsiLogic->cReplySize);
    SSMR3PutU16   (pSSM, pLsiLogic->u16IOCFaultCode);
    SSMR3PutU32   (pSSM, pLsiLogic->u32HostMFAHighAddr);
    SSMR3PutU32   (pSSM, pLsiLogic->u32SenseBufferHighAddr);
    SSMR3PutU8    (pSSM, pLsiLogic->cMaxDevices);
    SSMR3PutU8    (pSSM, pLsiLogic->cMaxBuses);
    SSMR3PutU16   (pSSM, pLsiLogic->cbReplyFrame);
    SSMR3PutU32   (pSSM, pLsiLogic->iDiagnosticAccess);
    SSMR3PutU32   (pSSM, pLsiLogic->cReplyQueueEntries);
    SSMR3PutU32   (pSSM, pLsiLogic->cRequestQueueEntries);
    SSMR3PutU32   (pSSM, pLsiLogic->uReplyFreeQueueNextEntryFreeWrite);
    SSMR3PutU32   (pSSM, pLsiLogic->uReplyFreeQueueNextAddressRead);
    SSMR3PutU32   (pSSM, pLsiLogic->uReplyPostQueueNextEntryFreeWrite);
    SSMR3PutU32   (pSSM, pLsiLogic->uReplyPostQueueNextAddressRead);
    SSMR3PutU32   (pSSM, pLsiLogic->uRequestQueueNextEntryFreeWrite);
    SSMR3PutU32   (pSSM, pLsiLogic->uRequestQueueNextAddressRead);
    SSMR3PutMem   (pSSM, &pLsiLogic->ConfigurationPages.SpiPages,
                   sizeof(pLsiLogic->ConfigurationPages.SpiPages));
    /* Now the data for the BIOS interface. */
    SSMR3PutU8    (pSSM, pLsiLogic->VBoxSCSI.regIdentify);
    SSMR3PutU8    (pSSM, pLsiLogic->VBoxSCSI.uTargetDevice);
    SSMR3PutU8    (pSSM, pLsiLogic->VBoxSCSI.uTxDir);
    SSMR3PutU8    (pSSM, pLsiLogic->VBoxSCSI.cbCDB);
    SSMR3PutMem   (pSSM, pLsiLogic->VBoxSCSI.aCDB, sizeof(pLsiLogic->VBoxSCSI.aCDB));
    SSMR3PutU8    (pSSM, pLsiLogic->VBoxSCSI.iCDB);
    SSMR3PutU32   (pSSM, pLsiLogic->VBoxSCSI.cbBuf);
    SSMR3PutU32   (pSSM, pLsiLogic->VBoxSCSI.iBuf);
    SSMR3PutBool  (pSSM, pLsiLogic->VBoxSCSI.fBusy);
    SSMR3PutU8    (pSSM, pLsiLogic->VBoxSCSI.enmState);
    if (pLsiLogic->VBoxSCSI.cbCDB)
        SSMR3PutMem(pSSM, pLsiLogic->VBoxSCSI.pBuf, pLsiLogic->VBoxSCSI.cbBuf);

    return SSMR3PutU32(pSSM, ~0);
}

static DECLCALLBACK(int) lsilogicLoadExec(PPDMDEVINS pDevIns, PSSMHANDLE pSSM, uint32_t uVersion, uint32_t uPass)
{
    PLSILOGICSCSI   pLsiLogic = PDMINS_2_DATA(pDevIns, PLSILOGICSCSI);
    int             rc;

    if (    uVersion != LSILOGIC_SAVED_STATE_VERSION
        &&  uVersion != LSILOGIC_SAVED_STATE_VERSION_VBOX_30)
        return VERR_SSM_UNSUPPORTED_DATA_UNIT_VERSION;

    /* device config */
    if (uVersion > LSILOGIC_SAVED_STATE_VERSION_VBOX_30)
    {
        for (unsigned i = 0; i < RT_ELEMENTS(pLsiLogic->aDeviceStates); i++)
        {
            bool fPresent;
            rc = SSMR3GetBool(pSSM, &fPresent);
            AssertRCReturn(rc, rc);
            if (fPresent != (pLsiLogic->aDeviceStates[i].pDrvBase != NULL))
                return SSMR3SetCfgError(pSSM, RT_SRC_POS, N_("Target %u config mismatch: config=%RTbool state=%RTbool"),
                                         i, pLsiLogic->aDeviceStates[i].pDrvBase != NULL, fPresent);
        }
    }
    if (uPass != SSM_PASS_FINAL)
        return VINF_SUCCESS;

    /* Every device first. */
    for (unsigned i = 0; i < RT_ELEMENTS(pLsiLogic->aDeviceStates); i++)
    {
        PLSILOGICDEVICE pDevice = &pLsiLogic->aDeviceStates[i];

        AssertMsg(!pDevice->cOutstandingRequests,
                  ("There are still outstanding requests on this device\n"));
        SSMR3GetU32(pSSM, (uint32_t *)&pDevice->cOutstandingRequests);
    }
    /* Now the main device state. */
    SSMR3GetU32   (pSSM, (uint32_t *)&pLsiLogic->enmState);
    SSMR3GetU32   (pSSM, (uint32_t *)&pLsiLogic->enmWhoInit);
    SSMR3GetBool  (pSSM, &pLsiLogic->fDoorbellInProgress);
    SSMR3GetBool  (pSSM, &pLsiLogic->fDiagnosticEnabled);
    SSMR3GetBool  (pSSM, &pLsiLogic->fNotificationSend);
    SSMR3GetBool  (pSSM, &pLsiLogic->fEventNotificationEnabled);
    SSMR3GetU32   (pSSM, (uint32_t *)&pLsiLogic->uInterruptMask);
    SSMR3GetU32   (pSSM, (uint32_t *)&pLsiLogic->uInterruptStatus);
    for (unsigned i = 0; i < RT_ELEMENTS(pLsiLogic->aMessage); i++)
        SSMR3GetU32   (pSSM, &pLsiLogic->aMessage[i]);
    SSMR3GetU32   (pSSM, &pLsiLogic->iMessage);
    SSMR3GetU32   (pSSM, &pLsiLogic->cMessage);
    SSMR3GetMem   (pSSM, &pLsiLogic->ReplyBuffer, sizeof(pLsiLogic->ReplyBuffer));
    SSMR3GetU32   (pSSM, &pLsiLogic->uNextReplyEntryRead);
    SSMR3GetU32   (pSSM, &pLsiLogic->cReplySize);
    SSMR3GetU16   (pSSM, &pLsiLogic->u16IOCFaultCode);
    SSMR3GetU32   (pSSM, &pLsiLogic->u32HostMFAHighAddr);
    SSMR3GetU32   (pSSM, &pLsiLogic->u32SenseBufferHighAddr);
    SSMR3GetU8    (pSSM, &pLsiLogic->cMaxDevices);
    SSMR3GetU8    (pSSM, &pLsiLogic->cMaxBuses);
    SSMR3GetU16   (pSSM, &pLsiLogic->cbReplyFrame);
    SSMR3GetU32   (pSSM, &pLsiLogic->iDiagnosticAccess);
    SSMR3GetU32   (pSSM, &pLsiLogic->cReplyQueueEntries);
    SSMR3GetU32   (pSSM, &pLsiLogic->cRequestQueueEntries);
    SSMR3GetU32   (pSSM, (uint32_t *)&pLsiLogic->uReplyFreeQueueNextEntryFreeWrite);
    SSMR3GetU32   (pSSM, (uint32_t *)&pLsiLogic->uReplyFreeQueueNextAddressRead);
    SSMR3GetU32   (pSSM, (uint32_t *)&pLsiLogic->uReplyPostQueueNextEntryFreeWrite);
    SSMR3GetU32   (pSSM, (uint32_t *)&pLsiLogic->uReplyPostQueueNextAddressRead);
    SSMR3GetU32   (pSSM, (uint32_t *)&pLsiLogic->uRequestQueueNextEntryFreeWrite);
    SSMR3GetU32   (pSSM, (uint32_t *)&pLsiLogic->uRequestQueueNextAddressRead);
    SSMR3GetMem   (pSSM, &pLsiLogic->ConfigurationPages.SpiPages,
                   sizeof(pLsiLogic->ConfigurationPages.SpiPages));
    /* Now the data for the BIOS interface. */
    SSMR3GetU8  (pSSM, &pLsiLogic->VBoxSCSI.regIdentify);
    SSMR3GetU8  (pSSM, &pLsiLogic->VBoxSCSI.uTargetDevice);
    SSMR3GetU8  (pSSM, &pLsiLogic->VBoxSCSI.uTxDir);
    SSMR3GetU8  (pSSM, &pLsiLogic->VBoxSCSI.cbCDB);
    SSMR3GetMem (pSSM, pLsiLogic->VBoxSCSI.aCDB, sizeof(pLsiLogic->VBoxSCSI.aCDB));
    SSMR3GetU8  (pSSM, &pLsiLogic->VBoxSCSI.iCDB);
    SSMR3GetU32 (pSSM, &pLsiLogic->VBoxSCSI.cbBuf);
    SSMR3GetU32 (pSSM, &pLsiLogic->VBoxSCSI.iBuf);
    SSMR3GetBool(pSSM, (bool *)&pLsiLogic->VBoxSCSI.fBusy);
    SSMR3GetU8  (pSSM, (uint8_t *)&pLsiLogic->VBoxSCSI.enmState);
    if (pLsiLogic->VBoxSCSI.cbCDB)
    {
        pLsiLogic->VBoxSCSI.pBuf = (uint8_t *)RTMemAllocZ(pLsiLogic->VBoxSCSI.cbCDB);
        if (!pLsiLogic->VBoxSCSI.pBuf)
        {
            LogRel(("LsiLogic: Out of memory during restore.\n"));
            return PDMDEV_SET_ERROR(pDevIns, VERR_NO_MEMORY,
                                    N_("LsiLogic: Out of memory during restore\n"));
        }
        SSMR3GetMem(pSSM, pLsiLogic->VBoxSCSI.pBuf, pLsiLogic->VBoxSCSI.cbBuf);
    }

    uint32_t u32;
    rc = SSMR3GetU32(pSSM, &u32);
    if (RT_FAILURE(rc))
        return rc;
    AssertMsgReturn(u32 == ~0U, ("%#x\n", u32), VERR_SSM_DATA_UNIT_FORMAT_CHANGED);

    return VINF_SUCCESS;
}

/**
 * Gets the pointer to the status LED of a device - called from the SCSi driver.
 *
 * @returns VBox status code.
 * @param   pInterface      Pointer to the interface structure containing the called function pointer.
 * @param   iLUN            The unit which status LED we desire. Always 0 here as the driver
 *                          doesn't know about other LUN's.
 * @param   ppLed           Where to store the LED pointer.
 */
static DECLCALLBACK(int) lsilogicDeviceQueryStatusLed(PPDMILEDPORTS pInterface, unsigned iLUN, PPDMLED *ppLed)
{
    PLSILOGICDEVICE pDevice = PDMILEDPORTS_2_PLSILOGICDEVICE(pInterface);
    if (iLUN == 0)
    {
        *ppLed = &pDevice->Led;
        Assert((*ppLed)->u32Magic == PDMLED_MAGIC);
        return VINF_SUCCESS;
    }
    return VERR_PDM_LUN_NOT_FOUND;
}

/**
 * Queries an interface to the driver.
 *
 * @returns Pointer to interface.
 * @returns NULL if the interface was not supported by the device.
 * @param   pInterface          Pointer to LSILOGICDEVICE::IBase.
 * @param   enmInterface        The requested interface identification.
 */
static DECLCALLBACK(void *) lsilogicDeviceQueryInterface(PPDMIBASE pInterface, PDMINTERFACE enmInterface)
{
    PLSILOGICDEVICE pDevice = PDMIBASE_2_PLSILOGICDEVICE(pInterface);

    switch (enmInterface)
    {
        case PDMINTERFACE_SCSI_PORT:
            return &pDevice->ISCSIPort;
        case PDMINTERFACE_LED_PORTS:
            return &pDevice->ILed;
        default:
            return NULL;
    }
}

/**
 * Gets the pointer to the status LED of a unit.
 *
 * @returns VBox status code.
 * @param   pInterface      Pointer to the interface structure containing the called function pointer.
 * @param   iLUN            The unit which status LED we desire.
 * @param   ppLed           Where to store the LED pointer.
 */
static DECLCALLBACK(int) lsilogicStatusQueryStatusLed(PPDMILEDPORTS pInterface, unsigned iLUN, PPDMLED *ppLed)
{
    PLSILOGICSCSI pLsiLogic = PDMILEDPORTS_2_PLSILOGICSCSI(pInterface);
    if (iLUN < LSILOGIC_DEVICES_MAX)
    {
        *ppLed = &pLsiLogic->aDeviceStates[iLUN].Led;
        Assert((*ppLed)->u32Magic == PDMLED_MAGIC);
        return VINF_SUCCESS;
    }
    return VERR_PDM_LUN_NOT_FOUND;
}

/**
 * Queries an interface to the driver.
 *
 * @returns Pointer to interface.
 * @returns NULL if the interface was not supported by the device.
 * @param   pInterface          Pointer to ATADevState::IBase.
 * @param   enmInterface        The requested interface identification.
 */
static DECLCALLBACK(void *) lsilogicStatusQueryInterface(PPDMIBASE pInterface, PDMINTERFACE enmInterface)
{
    PLSILOGICSCSI pLsiLogic = PDMIBASE_2_PLSILOGICSCSI(pInterface);
    switch (enmInterface)
    {
        case PDMINTERFACE_BASE:
            return &pLsiLogic->IBase;
        case PDMINTERFACE_LED_PORTS:
            return &pLsiLogic->ILeds;
        default:
            return NULL;
    }
}

/**
 * Detach notification.
 *
 * One harddisk at one port has been unplugged.
 * The VM is suspended at this point.
 *
 * @param   pDevIns     The device instance.
 * @param   iLUN        The logical unit which is being detached.
 * @param   fFlags      Flags, combination of the PDMDEVATT_FLAGS_* \#defines.
 */
static DECLCALLBACK(void) lsilogicDetach(PPDMDEVINS pDevIns, unsigned iLUN, uint32_t fFlags)
{
    PLSILOGICSCSI   pThis = PDMINS_2_DATA(pDevIns, PLSILOGICSCSI);
    PLSILOGICDEVICE pDevice = &pThis->aDeviceStates[iLUN];

    AssertMsg(fFlags & PDM_TACH_FLAGS_NOT_HOT_PLUG,
              ("LsiLogic: Device does not support hotplugging\n"));

    Log(("%s:\n", __FUNCTION__));

    /*
     * Zero some important members.
     */
    pDevice->pDrvBase = NULL;
    pDevice->pDrvSCSIConnector = NULL;
}

/**
 * Attach command.
 *
 * This is called when we change block driver.
 *
 * @returns VBox status code.
 * @param   pDevIns     The device instance.
 * @param   iLUN        The logical unit which is being detached.
 * @param   fFlags      Flags, combination of the PDMDEVATT_FLAGS_* \#defines.
 */
static DECLCALLBACK(int)  lsilogicAttach(PPDMDEVINS pDevIns, unsigned iLUN, uint32_t fFlags)
{
    PLSILOGICSCSI   pThis = PDMINS_2_DATA(pDevIns, PLSILOGICSCSI);
    PLSILOGICDEVICE pDevice = &pThis->aDeviceStates[iLUN];
    int rc;

    AssertMsgReturn(fFlags & PDM_TACH_FLAGS_NOT_HOT_PLUG,
                    ("LsiLogic: Device does not support hotplugging\n"),
                    VERR_INVALID_PARAMETER);

    /* the usual paranoia */
    AssertRelease(!pDevice->pDrvBase);
    AssertRelease(!pDevice->pDrvSCSIConnector);
    Assert(pDevice->iLUN == iLUN);

    /*
     * Try attach the block device and get the interfaces,
     * required as well as optional.
     */
    rc = PDMDevHlpDriverAttach(pDevIns, pDevice->iLUN, &pDevice->IBase, &pDevice->pDrvBase, NULL);
    if (RT_SUCCESS(rc))
    {
        /* Get SCSI connector interface. */
        pDevice->pDrvSCSIConnector = (PPDMISCSICONNECTOR)pDevice->pDrvBase->pfnQueryInterface(pDevice->pDrvBase, PDMINTERFACE_SCSI_CONNECTOR);
        AssertMsgReturn(pDevice->pDrvSCSIConnector, ("Missing SCSI interface below\n"), VERR_PDM_MISSING_INTERFACE);
    }
    else
        AssertMsgFailed(("Failed to attach LUN#%d. rc=%Rrc\n", pDevice->iLUN, rc));

    if (RT_FAILURE(rc))
    {
        pDevice->pDrvBase = NULL;
        pDevice->pDrvSCSIConnector = NULL;
    }
    return rc;
}

/**
 * @copydoc FNPDMDEVRESET
 */
static DECLCALLBACK(void) lsilogicReset(PPDMDEVINS pDevIns)
{
    PLSILOGICSCSI pLsiLogic = PDMINS_2_DATA(pDevIns, PLSILOGICSCSI);
    int rc;

    rc = lsilogicHardReset(pLsiLogic);
    AssertRC(rc);

    vboxscsiInitialize(&pLsiLogic->VBoxSCSI);
}

/**
 * @copydoc FNPDMDEVRELOCATE
 */
static DECLCALLBACK(void) lsilogicRelocate(PPDMDEVINS pDevIns, RTGCINTPTR offDelta)
{
    PLSILOGICSCSI pThis = PDMINS_2_DATA(pDevIns, PLSILOGICSCSI);

    pThis->pDevInsRC        = PDMDEVINS_2_RCPTR(pDevIns);
    pThis->pNotificationQueueRC = PDMQueueRCPtr(pThis->pNotificationQueueR3);

    /* Relocate queues. */
    pThis->pReplyFreeQueueBaseRC += offDelta;
    pThis->pReplyPostQueueBaseRC += offDelta;
    pThis->pRequestQueueBaseRC   += offDelta;
}

/**
 * @copydoc FNPDMDEVDESTRUCT
 */
static DECLCALLBACK(int) lsilogicDestruct(PPDMDEVINS pDevIns)
{
    PLSILOGICSCSI pThis = PDMINS_2_DATA(pDevIns, PLSILOGICSCSI);
    int rc = VINF_SUCCESS;

    PDMR3CritSectDelete(&pThis->ReplyFreeQueueCritSect);
    PDMR3CritSectDelete(&pThis->ReplyPostQueueCritSect);

    /* Destroy task cache. */
    if (pThis->pTaskCache)
        rc = RTCacheDestroy(pThis->pTaskCache);

    return rc;
}

/**
 * @copydoc FNPDMDEVCONSTRUCT
 */
static DECLCALLBACK(int) lsilogicConstruct(PPDMDEVINS pDevIns, int iInstance, PCFGMNODE pCfgHandle)
{
    PLSILOGICSCSI pThis = PDMINS_2_DATA(pDevIns, PLSILOGICSCSI);
    int rc = VINF_SUCCESS;
    char *pszCtrlType = NULL;
    PVM pVM = PDMDevHlpGetVM(pDevIns);

    /*
     * Validate and read configuration.
     */
    rc = CFGMR3AreValuesValid(pCfgHandle, "GCEnabled\0"
                                          "R0Enabled\0"
                                          "ReplyQueueDepth\0"
                                          "RequestQueueDepth\0"
                                          "ControllerType\0");
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, VERR_PDM_DEVINS_UNKNOWN_CFG_VALUES,
                                N_("LsiLogic configuration error: unknown option specified"));
    rc = CFGMR3QueryBoolDef(pCfgHandle, "GCEnabled", &pThis->fGCEnabled, true);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc,
                                N_("LsiLogic configuration error: failed to read GCEnabled as boolean"));
    Log(("%s: fGCEnabled=%d\n", __FUNCTION__, pThis->fGCEnabled));

    rc = CFGMR3QueryBoolDef(pCfgHandle, "R0Enabled", &pThis->fR0Enabled, true);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc,
                                N_("LsiLogic configuration error: failed to read R0Enabled as boolean"));
    Log(("%s: fR0Enabled=%d\n", __FUNCTION__, pThis->fR0Enabled));

    rc = CFGMR3QueryU32Def(pCfgHandle, "ReplyQueueDepth",
                           &pThis->cReplyQueueEntries,
                           LSILOGICSCSI_REPLY_QUEUE_DEPTH_DEFAULT);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc,
                                N_("LsiLogic configuration error: failed to read ReplyQueue as integer"));
    Log(("%s: ReplyQueueDepth=%u\n", __FUNCTION__, pThis->cReplyQueueEntries));

    rc = CFGMR3QueryU32Def(pCfgHandle, "RequestQueueDepth",
                           &pThis->cRequestQueueEntries,
                           LSILOGICSCSI_REQUEST_QUEUE_DEPTH_DEFAULT);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc,
                                N_("LsiLogic configuration error: failed to read RequestQueue as integer"));
    Log(("%s: RequestQueueDepth=%u\n", __FUNCTION__, pThis->cRequestQueueEntries));

    rc = CFGMR3QueryStringAllocDef(pCfgHandle, "ControllerType",
                                   &pszCtrlType, LSILOGICSCSI_PCI_SPI_CTRLNAME);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc,
                                N_("LsiLogic configuration error: failed to read ControllerType as string"));
    Log(("%s: ControllerType=%s\n", __FUNCTION__, pszCtrlType));

    rc = lsilogicGetCtrlTypeFromString(pThis, pszCtrlType);
    MMR3HeapFree(pszCtrlType);

    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc,
                                N_("LsiLogic configuration error: failed to determine controller type from string"));

    /* Init static parts. */
    PCIDevSetVendorId(&pThis->PciDev, LSILOGICSCSI_PCI_VENDOR_ID); /* LsiLogic */

    if (pThis->enmCtrlType == LSILOGICCTRLTYPE_SCSI_SPI)
    {
        PCIDevSetDeviceId         (&pThis->PciDev, LSILOGICSCSI_PCI_SPI_DEVICE_ID); /* LSI53C1030 */
        PCIDevSetSubSystemVendorId(&pThis->PciDev, LSILOGICSCSI_PCI_SPI_SUBSYSTEM_VENDOR_ID);
        PCIDevSetSubSystemId      (&pThis->PciDev, LSILOGICSCSI_PCI_SPI_SUBSYSTEM_ID);
    }
    else if (pThis->enmCtrlType == LSILOGICCTRLTYPE_SCSI_SAS)
    {
        PCIDevSetDeviceId         (&pThis->PciDev, LSILOGICSCSI_PCI_SAS_DEVICE_ID); /* SAS1068 */
        PCIDevSetSubSystemVendorId(&pThis->PciDev, LSILOGICSCSI_PCI_SAS_SUBSYSTEM_VENDOR_ID);
        PCIDevSetSubSystemId      (&pThis->PciDev, LSILOGICSCSI_PCI_SAS_SUBSYSTEM_ID);
    }
    else
        AssertMsgFailed(("Invalid controller type: %d\n", pThis->enmCtrlType));

    PCIDevSetClassProg   (&pThis->PciDev,   0x00); /* SCSI */
    PCIDevSetClassSub    (&pThis->PciDev,   0x00); /* SCSI */
    PCIDevSetClassBase   (&pThis->PciDev,   0x01); /* Mass storage */
    PCIDevSetInterruptPin(&pThis->PciDev,   0x01); /* Interrupt pin A */

    pThis->pDevInsR3 = pDevIns;
    pThis->pDevInsR0 = PDMDEVINS_2_R0PTR(pDevIns);
    pThis->pDevInsRC = PDMDEVINS_2_RCPTR(pDevIns);
    pThis->IBase.pfnQueryInterface = lsilogicStatusQueryInterface;
    pThis->ILeds.pfnQueryStatusLed = lsilogicStatusQueryStatusLed;

    /*
     * Register the PCI device, it's I/O regions.
     */
    rc = PDMDevHlpPCIRegister (pDevIns, &pThis->PciDev);
    if (RT_FAILURE(rc))
        return rc;

    rc = PDMDevHlpPCIIORegionRegister(pDevIns, 0, LSILOGIC_PCI_SPACE_IO_SIZE, PCI_ADDRESS_SPACE_IO, lsilogicMap);
    if (RT_FAILURE(rc))
        return rc;

    rc = PDMDevHlpPCIIORegionRegister(pDevIns, 1, LSILOGIC_PCI_SPACE_MEM_SIZE, PCI_ADDRESS_SPACE_MEM, lsilogicMap);
    if (RT_FAILURE(rc))
        return rc;

    rc = PDMDevHlpPCIIORegionRegister(pDevIns, 2, LSILOGIC_PCI_SPACE_MEM_SIZE, PCI_ADDRESS_SPACE_MEM, lsilogicMap);
    if (RT_FAILURE(rc))
        return rc;

    /* Intialize task queue. */
    rc = PDMDevHlpPDMQueueCreate(pDevIns, sizeof(PDMQUEUEITEMCORE), 2, 0,
                                 lsilogicNotifyQueueConsumer, true, "LsiLogic-Task", &pThis->pNotificationQueueR3);
    if (RT_FAILURE(rc))
        return rc;
    pThis->pNotificationQueueR0 = PDMQueueR0Ptr(pThis->pNotificationQueueR3);
    pThis->pNotificationQueueRC = PDMQueueRCPtr(pThis->pNotificationQueueR3);

    /*
     * We need one entry free in the queue.
     */
    pThis->cReplyQueueEntries++;
    pThis->cRequestQueueEntries++;

    /*
     * Allocate memory for the queues.
     */
    uint32_t cbQueues;

    cbQueues  = 2*pThis->cReplyQueueEntries * sizeof(uint32_t);
    cbQueues += pThis->cRequestQueueEntries * sizeof(uint32_t);
    rc = MMHyperAlloc(pVM, cbQueues, 1, MM_TAG_PDM_DEVICE_USER,
                      (void **)&pThis->pReplyFreeQueueBaseR3);
    if (RT_FAILURE(rc))
        return VERR_NO_MEMORY;
    pThis->pReplyFreeQueueBaseR0 = MMHyperR3ToR0(pVM, (void *)pThis->pReplyFreeQueueBaseR3);
    pThis->pReplyFreeQueueBaseRC = MMHyperR3ToRC(pVM, (void *)pThis->pReplyFreeQueueBaseR3);

    pThis->pReplyPostQueueBaseR3 = pThis->pReplyFreeQueueBaseR3 + pThis->cReplyQueueEntries;
    pThis->pReplyPostQueueBaseR0 = MMHyperR3ToR0(pVM, (void *)pThis->pReplyPostQueueBaseR3);
    pThis->pReplyPostQueueBaseRC = MMHyperR3ToRC(pVM, (void *)pThis->pReplyPostQueueBaseR3);

    pThis->pRequestQueueBaseR3   = pThis->pReplyPostQueueBaseR3 + pThis->cReplyQueueEntries;
    pThis->pRequestQueueBaseR0   = MMHyperR3ToR0(pVM, (void *)pThis->pRequestQueueBaseR3);
    pThis->pRequestQueueBaseRC   = MMHyperR3ToRC(pVM, (void *)pThis->pRequestQueueBaseR3);

    /*
     * Create critical sections protecting the reply post and free queues.
     */
    rc = PDMDevHlpCritSectInit(pDevIns, &pThis->ReplyFreeQueueCritSect, "LsiLogicRFQ");
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc,
                                N_("LsiLogic: cannot create critical section for reply free queue"));

    rc = PDMDevHlpCritSectInit(pDevIns, &pThis->ReplyPostQueueCritSect, "LsiLogicRPQ");
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc,
                                N_("LsiLogic: cannot create critical section for reply post queue"));

    /*
     * Allocate task cache.
     */
    rc = RTCacheCreate(&pThis->pTaskCache, 0, sizeof(LSILOGICTASKSTATE), RTOBJCACHE_PROTECT_INSERT);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc,
                                N_("Cannot create task cache"));

    /* Initialize per device state. */
    for (unsigned i = 0; i < RT_ELEMENTS(pThis->aDeviceStates); i++)
    {
        char szName[24];
        PLSILOGICDEVICE pDevice = &pThis->aDeviceStates[i];

        RTStrPrintf(szName, sizeof(szName), "Device%d", i);

        /* Initialize static parts of the device. */
        pDevice->iLUN = i;
        pDevice->pLsiLogicR3  = pThis;
        pDevice->pLsiLogicR0  = PDMINS_2_DATA_R0PTR(pDevIns);
        pDevice->pLsiLogicRC  = PDMINS_2_DATA_RCPTR(pDevIns);
        pDevice->Led.u32Magic = PDMLED_MAGIC;
        pDevice->IBase.pfnQueryInterface           = lsilogicDeviceQueryInterface;
        pDevice->ISCSIPort.pfnSCSIRequestCompleted = lsilogicDeviceSCSIRequestCompleted;
        pDevice->ILed.pfnQueryStatusLed            = lsilogicDeviceQueryStatusLed;

        /* Attach SCSI driver. */
        rc = PDMDevHlpDriverAttach(pDevIns, pDevice->iLUN, &pDevice->IBase, &pDevice->pDrvBase, szName);
        if (RT_SUCCESS(rc))
        {
            /* Get SCSI connector interface. */
            pDevice->pDrvSCSIConnector = (PPDMISCSICONNECTOR)pDevice->pDrvBase->pfnQueryInterface(pDevice->pDrvBase, PDMINTERFACE_SCSI_CONNECTOR);
            AssertMsgReturn(pDevice->pDrvSCSIConnector, ("Missing SCSI interface below\n"), VERR_PDM_MISSING_INTERFACE);
        }
        else if (rc == VERR_PDM_NO_ATTACHED_DRIVER)
        {
            pDevice->pDrvBase = NULL;
            rc = VINF_SUCCESS;
            Log(("LsiLogic: no driver attached to device %s\n", szName));
        }
        else
        {
            AssertLogRelMsgFailed(("LsiLogic: Failed to attach %s\n", szName));
            return rc;
        }
    }

    /*
     * Attach status driver (optional).
     */
    PPDMIBASE pBase;
    rc = PDMDevHlpDriverAttach(pDevIns, PDM_STATUS_LUN, &pThis->IBase, &pBase, "Status Port");
    if (RT_SUCCESS(rc))
        pThis->pLedsConnector = (PDMILEDCONNECTORS *)pBase->pfnQueryInterface(pBase, PDMINTERFACE_LED_CONNECTORS);
    else if (rc != VERR_PDM_NO_ATTACHED_DRIVER)
    {
        AssertMsgFailed(("Failed to attach to status driver. rc=%Rrc\n", rc));
        return PDMDEV_SET_ERROR(pDevIns, rc, N_("LsiLogic cannot attach to status driver"));
    }

    /* Initialize the SCSI emulation for the BIOS. */
    rc = vboxscsiInitialize(&pThis->VBoxSCSI);
    AssertRC(rc);

    /* Register I/O port space in ISA region for BIOS access. */
    rc = PDMDevHlpIOPortRegister(pDevIns, LSILOGIC_ISA_IO_PORT, 3, NULL,
                                 lsilogicIsaIOPortWrite, lsilogicIsaIOPortRead,
                                 lsilogicIsaIOPortWriteStr, lsilogicIsaIOPortReadStr,
                                 "LsiLogic BIOS");
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc, N_("LsiLogic cannot register legacy I/O handlers"));

    /* Register save state handlers. */
    rc = PDMDevHlpSSMRegister3(pDevIns, LSILOGIC_SAVED_STATE_VERSION, sizeof(*pThis),
                               lsilogicLiveExec, lsilogicSaveExec, lsilogicLoadExec);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc, N_("LsiLogic cannot register save state handlers"));

    pThis->enmWhoInit = LSILOGICWHOINIT_SYSTEM_BIOS;

    /* Perform hard reset. */
    rc = lsilogicHardReset(pThis);
    AssertRC(rc);

    return rc;
}

/**
 * The device registration structure.
 */
const PDMDEVREG g_DeviceLsiLogicSCSI =
{
    /* u32Version */
    PDM_DEVREG_VERSION,
    /* szDeviceName */
    "lsilogicscsi",
    /* szRCMod */
    "VBoxDDGC.gc",
    /* szR0Mod */
    "VBoxDDR0.r0",
    /* pszDescription */
    "LSI Logic 53c1030 SCSI controller.\n",
    /* fFlags */
    PDM_DEVREG_FLAGS_DEFAULT_BITS | PDM_DEVREG_FLAGS_RC | PDM_DEVREG_FLAGS_R0,
    /* fClass */
    PDM_DEVREG_CLASS_STORAGE,
    /* cMaxInstances */
    ~0,
    /* cbInstance */
    sizeof(LSILOGICSCSI),
    /* pfnConstruct */
    lsilogicConstruct,
    /* pfnDestruct */
    lsilogicDestruct,
    /* pfnRelocate */
    lsilogicRelocate,
    /* pfnIOCtl */
    NULL,
    /* pfnPowerOn */
    NULL,
    /* pfnReset */
    lsilogicReset,
    /* pfnSuspend */
    NULL,
    /* pfnResume */
    NULL,
    /* pfnAttach */
    lsilogicAttach,
    /* pfnDetach */
    lsilogicDetach,
    /* pfnQueryInterface. */
    NULL,
    /* pfnInitComplete */
    NULL,
    /* pfnPowerOff */
    NULL,
    /* pfnSoftReset */
    NULL,
    /* u32VersionEnd */
    PDM_DEVREG_VERSION
};

#endif /* IN_RING3 */
#endif /* !VBOX_DEVICE_STRUCT_TESTCASE */

