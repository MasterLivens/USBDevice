/**
  ******************************************************************************
  * @file    usbd_hid.c
  * @author  Benedek Kupper
  * @version 0.1
  * @date    2018-01-31
  * @brief   USB Human Interface Device Class implementation
  *
  * Copyright (c) 2018 Benedek Kupper
  *
  * Licensed under the Apache License, Version 2.0 (the "License");
  * you may not use this file except in compliance with the License.
  * You may obtain a copy of the License at
  *
  *     http://www.apache.org/licenses/LICENSE-2.0
  *
  * Unless required by applicable law or agreed to in writing, software
  * distributed under the License is distributed on an "AS IS" BASIS,
  * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  * See the License for the specific language governing permissions and
  * limitations under the License.
  */
#include <usbd_internal.h>
#include <usbd_hid.h>

#define HID_SUB_DESC_COUNT              1

#if (USBD_HID_ALTSETTINGS != 0)
#define HID_APP(ITF)    ((USBD_HID_AppType*)(&(ITF)->App[(ITF)->Base.AltSelector]))
#else
#define HID_APP(ITF)    ((USBD_HID_AppType*)((ITF)->App))
#endif


typedef struct {
    uint8_t Id;
    USBD_HID_ReportType Type;
}__packed USBD_HID_ReportInfoType;

typedef struct
{
    uint8_t  bDescriptorType;
    uint16_t wItemLength;
}__packed USBD_HID_SubDescType;

typedef struct
{
    /* Human Interface Descriptor */
    USB_InterfaceDescType HID;
    /* HID Class Descriptor */
    struct {
        uint8_t  bLength;
        uint8_t  bDescriptorType;
        uint16_t bcdHID;
        uint8_t  bCountryCode;
        uint8_t  bNumDescriptors;
        USBD_HID_SubDescType sHIDD[HID_SUB_DESC_COUNT];
    }__packed HIDCD;
    /* Endpoint descriptors are dynamically added */
}__packed USBD_HID_DescType;

static const USBD_HID_DescType hid_desc = {
    .HID = { /* HID Interface Descriptor */
        .bLength            = sizeof(hid_desc.HID),
        .bDescriptorType    = USB_DESC_TYPE_INTERFACE,
        .bInterfaceNumber   = 0,
        .bAlternateSetting  = 0,
        .bNumEndpoints      = 1,
        .bInterfaceClass    = 0x03, /* bInterfaceClass: Human Interface Device Class */
        .bInterfaceSubClass = 0x00, /* bInterfaceSubClass: 1=BOOT, 0=no boot */
        .bInterfaceProtocol = 0x00, /* bInterfaceProtocol: 0=none, 1=keyboard, 2=mouse */
        .iInterface         = USBD_ISTR_INTERFACES,
    },
    .HIDCD = { /* HID Class Descriptor */
        .bLength            = sizeof(hid_desc.HIDCD),
        .bDescriptorType    = HID_DESC_TYPE_HID,
        .bcdHID             = 0x0111,   /* bcdHID: HID Class Spec release number */
        .bCountryCode       = 0x00,     /* bCountryCode: Hardware target country */
        .bNumDescriptors    = HID_SUB_DESC_COUNT,
        .sHIDD = {
            /* Report descriptor is mandatory minimum */
            {.bDescriptorType   = HID_DESC_TYPE_REPORT,
              .wItemLength      = 0, },
        },
    },
};

#if (USBD_HID_ALTSETTINGS != 0)
static uint16_t         hid_getAltsDesc (USBD_HID_IfHandleType *itf, uint8_t ifNum, uint8_t * dest);
#endif
static uint16_t         hid_getDesc     (USBD_HID_IfHandleType *itf, uint8_t ifNum, uint8_t * dest);
static const char *     hid_getString   (USBD_HID_IfHandleType *itf, uint8_t intNum);
static void             hid_init        (USBD_HID_IfHandleType *itf);
static void             hid_deinit      (USBD_HID_IfHandleType *itf);
static USBD_ReturnType  hid_setupStage  (USBD_HID_IfHandleType *itf);
static void             hid_dataStage   (USBD_HID_IfHandleType *itf);
#if (USBD_HID_OUT_SUPPORT == 1)
static void             hid_outData     (USBD_HID_IfHandleType *itf, USBD_EpHandleType *ep);
#endif


/* HID interface class callbacks structure */
static const USBD_ClassType hid_cbks = {
#if (USBD_HID_ALTSETTINGS != 0)
    .GetDescriptor  = (USBD_IfDescCbkType)  hid_getAltsDesc,
#else
    .GetDescriptor  = (USBD_IfDescCbkType)  hid_getDesc,
#endif
    .GetString      = (USBD_IfStrCbkType)   hid_getString,
    .Init           = (USBD_IfCbkType)      hid_init,
    .Deinit         = (USBD_IfCbkType)      hid_deinit,
    .SetupStage     = (USBD_IfSetupCbkType) hid_setupStage,
    .DataStage      = (USBD_IfCbkType)      hid_dataStage,
#if (USBD_HID_OUT_SUPPORT == 1)
    .OutData        = (USBD_IfEpCbkType)    hid_outData,
#endif
};

/** @ingroup USBD_HID
 * @defgroup USBD_HID_Private_Functions HID Private Functions
 * @{ */

#if (USBD_HID_ALTSETTINGS != 0)
/**
 * @brief Copies the interface descriptor to the destination buffer.
 * @param itf: reference of the HID interface
 * @param ifNum: the index of the current interface in the device
 * @param dest: the destination buffer
 * @return Length of the copied descriptor
 */
static uint16_t hid_getAltsDesc(USBD_HID_IfHandleType *itf, uint8_t ifNum, uint8_t * dest)
{
    uint8_t as;
    uint16_t len = 0;

    /* Copy the descriptors many times, add alternate setting indexes */
    for (as = 0; as < itf->Base.AltCount; as++)
    {
        USBD_HID_DescType *desc = (USBD_HID_DescType*)&dest[len];
        len += hid_getDesc(itf, ifNum, &dest[len]);

        desc->HID.bAlternateSetting = as;

        desc->HID.iInterface = USBD_IIF_INDEX(ifNum, as);
    }
    return len;
}
#endif /* (USBD_HID_ALTSETTINGS != 0) */

/**
 * @brief Copies the interface descriptor to the destination buffer.
 * @param itf: reference of the HID interface
 * @param ifNum: the index of the current interface in the device
 * @param dest: the destination buffer
 * @return Length of the copied descriptor
 */
static uint16_t hid_getDesc(USBD_HID_IfHandleType *itf, uint8_t ifNum, uint8_t * dest)
{
    USBD_HandleType *dev = itf->Base.Device;
    USBD_HID_DescType *desc = (USBD_HID_DescType*)dest;
    uint16_t len = sizeof(hid_desc);

    memcpy(dest, &hid_desc, sizeof(hid_desc));

    /* Set report size */
    desc->HIDCD.sHIDD[0].wItemLength = HID_APP(itf)->Report.Length;

#if (USBD_MAX_IF_COUNT > 1)
    /* Adjustment of interface indexes */
    desc->HID.bInterfaceNumber = ifNum;

    desc->HID.iInterface = USBD_IIF_INDEX(ifNum, 0);

#endif /* (USBD_MAX_IF_COUNT > 1) */

    /* Add endpoints */
    len += USBD_EpDesc(dev, itf->Config.InEp.Num, &dest[len]);
    dest[len - 1] = itf->Config.InEp.Interval;

#if (USBD_HID_OUT_SUPPORT == 1)
    if (itf->Config.OutEp.Size > 0)
    {
        len += USBD_EpDesc(dev, itf->Config.OutEp.Num, &dest[len]);
        dest[len - 1] = itf->Config.OutEp.Interval;
    }
#endif /* (USBD_HID_OUT_SUPPORT == 1) */

    return len;
}

/**
 * @brief Returns the selected interface string.
 * @param itf: reference of the HID interface
 * @param intNum: interface-internal string index
 * @return The referenced string
 */
static const char* hid_getString(USBD_HID_IfHandleType *itf, uint8_t intNum)
{
#if (USBD_HID_ALTSETTINGS != 0)
    if (intNum < itf->Base.AltCount)
    {
        return itf->App[intNum].Name;
    }
    else
    {
        return NULL;
    }
#else
    return itf->App->Name;
#endif
}

/**
 * @brief Initializes the interface by opening its endpoints,
 *        resetting the internal variables
 *        and initializing the attached application.
 * @param itf: reference of the HID interface
 */
static void hid_init(USBD_HID_IfHandleType *itf)
{
    USBD_HandleType *dev = itf->Base.Device;

    /* Open EPs */
    USBD_EpOpen(dev, itf->Config.InEp.Num,
            USB_EP_TYPE_INTERRUPT, itf->Config.InEp.Size);

#if (USBD_HID_OUT_SUPPORT == 1)
    if (itf->Config.OutEp.Size > 0)
    {
        USBD_EpOpen(dev, itf->Config.OutEp.Num,
                USB_EP_TYPE_INTERRUPT, itf->Config.OutEp.Size);
    }
#endif /* (USBD_HID_OUT_SUPPORT == 1) */

    /* Initialize state */
    itf->InGetReport = 0;
    itf->IdleRate = itf->Config.InEp.Interval / 4;

    /* Initialize application */
    USBD_SAFE_CALLBACK(HID_APP(itf)->Init, );
}

/**
 * @brief Deinitializes the interface by closing its endpoints
 *        and deinitializing the attached application.
 * @param itf: reference of the HID interface
 */
static void hid_deinit(USBD_HID_IfHandleType *itf)
{
    USBD_HandleType *dev = itf->Base.Device;

    /* Close EPs */
    USBD_EpClose(dev, itf->Config.InEp.Num);
#if (USBD_HID_OUT_SUPPORT == 1)
    if (itf->Config.OutEp.Size > 0)
    {
        USBD_EpClose(dev, itf->Config.OutEp.Num);
    }
#endif /* (USBD_HID_OUT_SUPPORT == 1) */

    /* Deinitialize application */
    USBD_SAFE_CALLBACK(HID_APP(itf)->Deinit, );

    /* Reset the endpoint MPS to the desired size */
    dev->EP.IN [itf->Config.InEp.Num & 0xF].MaxPacketSize = itf->Config.InEp.Size;
#if (USBD_HID_OUT_SUPPORT == 1)
    dev->EP.OUT[itf->Config.OutEp.Num     ].MaxPacketSize = itf->Config.OutEp.Size;
#endif /* (USBD_HID_OUT_SUPPORT == 1) */
}

/**
 * @brief Performs the interface-specific setup request handling.
 * @param itf: reference of the HID interface
 * @return OK if the setup request is accepted, INVALID otherwise
 */
static USBD_ReturnType hid_setupStage(USBD_HID_IfHandleType *itf)
{
    USBD_ReturnType retval = USBD_E_ERROR;
    USBD_HandleType *dev = itf->Base.Device;

    switch (dev->Setup.RequestType.Type)
    {
        case USB_REQ_TYPE_STANDARD:
        {
            /* HID specific descriptors can be requested */
            if (dev->Setup.Request == USB_REQ_GET_DESCRIPTOR)
            {
                switch (dev->Setup.Value >> 8)
                {
                    /* Return HID class descriptor */
                    case HID_DESC_TYPE_HID:
                    {
                        retval = USBD_CtrlSendData(dev, (uint8_t*)&hid_desc.HIDCD,
                                sizeof(hid_desc.HIDCD));
                        break;
                    }
                    /* Return HID report descriptor */
                    case HID_DESC_TYPE_REPORT:
                    {
                        retval = USBD_CtrlSendData(dev,
                                HID_APP(itf)->Report.Desc,
                                HID_APP(itf)->Report.Length);
                        break;
                    }
                    default:
                        break;
                }
            }
            break;
        }

        case USB_REQ_TYPE_CLASS:
        {
            switch (dev->Setup.Request)
            {
                /* HID report IN */
                case HID_REQ_GET_REPORT:
                {
                    uint8_t reportId = (uint8_t)dev->Setup.Value;

                    /* Set flag, invoke callback which should provide data
                     * via USBD_HID_ReportIn() */
                    itf->InGetReport = USBD_E_BUSY;
                    USBD_SAFE_CALLBACK(HID_APP(itf)->GetReport, reportId);

                    retval = itf->InGetReport;
                    itf->InGetReport = USBD_E_OK;
                    break;
                }

                /* HID report OUT */
                case HID_REQ_SET_REPORT:
                {
                    uint8_t* data = dev->CtrlData;

                    /* If report IDs are used, the ID shall be placed
                     * on the first byte */
                    if (HID_APP(itf)->Report.IDs > 0)
                    {   data++; }

                    retval = USBD_CtrlReceiveData(dev, data);
                    break;
                }

                /* Send 1 byte idle rate */
                case HID_REQ_GET_IDLE:
                    retval = USBD_CtrlSendData(dev,
                            itf->IdleRate, sizeof(itf->IdleRate));
                    break;

                case HID_REQ_SET_IDLE:
                {
                    /* wValue upper: Duration
                     * 0 - indefinite, only report when input changes
                     * x - 4*x ms idle, then reverts
                     * wValue lower: ID
                     * 0 - applies to all records
                     * x - record ID x only */
                    uint16_t idleRate_ms = HID_IDLE_RATE_INDEFINITE;
                    uint8_t idleRate = dev->Setup.Value >> 8;
                    uint8_t reportId = dev->Setup.Value;

                    /* Save only global config */
                    if (reportId == 0)
                    {   itf->IdleRate = idleRate; }

                    if (idleRate > 0)
                    {   idleRate_ms = 4 * itf->IdleRate; }

                    USBD_SAFE_CALLBACK(HID_APP(itf)->SetIdle,
                            idleRate_ms, reportId);
                    retval = USBD_E_OK;
                    break;
                }

#if (USBD_HID_BOOT_SUPPORT != 0)
                case HID_REQ_GET_PROTOCOL:
                    /* 1 byte active protocol */
                    break;

                case HID_REQ_SET_PROTOCOL:
                    /* wValue: 1 - report protocol, 0 - boot protocol */
                    break;
#endif
                default:
                    break;
            }
            break;
        }
        default:
            break;
    }
    return retval;
}

/**
 * @brief Passes the received set report to the application.
 * @param itf: reference of the HID interface
 */
static void hid_dataStage(USBD_HID_IfHandleType *itf)
{
    USBD_HandleType *dev = itf->Base.Device;

    if (dev->Setup.RequestType.Direction == USB_DIRECTION_OUT)
    {
        uint16_t len = dev->Setup.Length;

        if (HID_APP(itf)->Report.IDs > 0)
        {
            /* First byte is report ID from setup */
            dev->CtrlData[0] = (uint8_t)dev->Setup.Value;
            len++;
        }

        /* Hand over received data to App */
        USBD_SAFE_CALLBACK(HID_APP(itf)->SetReport, dev->CtrlData, len);
    }
}

#if (USBD_HID_OUT_SUPPORT == 1)
/**
 * @brief Notifies the application of a completed OUT transfer.
 * @param itf: reference of the HID interface
 * @param ep: reference to the endpoint structure
 */
static void hid_outData(USBD_HID_IfHandleType *itf, USBD_EpHandleType *ep)
{
    USBD_SAFE_CALLBACK(HID_APP(itf)->SetReport,
            ep->Transfer.Data - ep->Transfer.Length, ep->Transfer.Length);
}
#endif /* (USBD_HID_OUT_SUPPORT == 1) */

/** @} */

/** @defgroup USBD_HID_Exported_Functions HID Exported Functions
 * @{ */

/**
 * @brief Mounts the HID interface to the USB Device at the next interface slot.
 * @note  The interface reference shall have its Config structure and App reference
 *        properly set before this function is called.
 * @param itf: reference of the HID interface
 * @param dev: reference of the USB Device
 * @return OK if the mounting was successful,
 *         ERROR if it failed due to insufficient device interface slots
 */
USBD_ReturnType USBD_HID_MountInterface(USBD_HID_IfHandleType *itf, USBD_HandleType *dev)
{
    USBD_ReturnType retval = USBD_E_ERROR;

    if (dev->IfCount < USBD_MAX_IF_COUNT)
    {
        /* Binding interfaces */
        itf->Base.Device = dev;
        itf->Base.Class  = &hid_cbks;
        itf->Base.AltCount = 1;
        itf->Base.AltSelector = 0;

        {
            USBD_EpHandleType *ep;

            ep = &dev->EP.IN [itf->Config.InEp.Num & 0xF];
            ep->Type            = USB_EP_TYPE_INTERRUPT;
            ep->MaxPacketSize   = itf->Config.InEp.Size;
            ep->IfNum           = dev->IfCount;

#if (USBD_HID_OUT_SUPPORT == 1)
            /* OUT EP is optional */
            if (itf->Config.OutEp.Size > 0)
            {
                ep = &dev->EP.OUT[itf->Config.OutEp.Num];
                ep->Type            = USB_EP_TYPE_INTERRUPT;
                ep->MaxPacketSize   = itf->Config.OutEp.Size;
                ep->IfNum           = dev->IfCount;
            }
#endif /* (USBD_HID_OUT_SUPPORT == 1) */
        }

        dev->IF[dev->IfCount] = (USBD_IfHandleType*)itf;
        dev->IfCount++;

        retval = USBD_E_OK;
    }

    return retval;
}

/**
 * @brief Sends a HID report either through the HID IN endpoint,
 *        or through the control endpoint if called in the application's GetReport() context.
 * @param itf: reference of the HID interface
 * @param data: pointer to the data to send
 * @param length: length of the data
 * @return BUSY if the previous transfer is still ongoing, OK if successful
 */
USBD_ReturnType USBD_HID_ReportIn(USBD_HID_IfHandleType *itf, uint8_t *data, uint16_t length)
{
    USBD_ReturnType retval;
    USBD_HandleType *dev = itf->Base.Device;

    /* If the function is invoked in the EP0 GetReport() callback context,
     * use EP0 to transfer the report */
    if (itf->InGetReport != USBD_E_OK)
    {
        retval = USBD_CtrlSendData(dev, data, length);
        itf->InGetReport = USBD_E_OK;
    }
    else
    {
        retval = USBD_EpSend(dev, itf->Config.InEp.Num, data, length);
    }
    return retval;
}

#if (USBD_HID_OUT_SUPPORT == 1)
/**
 * @brief Receives a report through the HID OUT endpoint.
 * @param itf: reference of the HID interface
 * @param data: pointer to the data to receive
 * @param length: length of the data
 * @return BUSY if the previous transfer is still ongoing, OK if successful
 */
USBD_ReturnType USBD_HID_ReportOut(USBD_HID_IfHandleType *itf, uint8_t *data, uint16_t length)
{
    USBD_ReturnType retval = USBD_E_ERROR;
    USBD_HandleType *dev = itf->Base.Device;

    if (itf->Config.OutEp.Size > 0)
    {
        retval = USBD_EpReceive(dev, itf->Config.OutEp.Num, data, length);
    }
    return retval;
}
#endif /* (USBD_HID_OUT_SUPPORT == 1) */

/** @} */
