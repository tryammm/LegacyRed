//  Copyright © 2022-2023 ChefKiss Inc. Licensed under the Thou Shalt Not Profit License version 1.5. See LICENSE for
//  details.

#include "kern_support.hpp"
#include "kern_lred.hpp"
#include "kern_model.hpp"
#include "kern_patches.hpp"
#include "kern_vbios.hpp"
#include <Headers/kern_api.hpp>

static const char *pathRadeonSupport = "/System/Library/Extensions/AMDSupport.kext/Contents/MacOS/AMDSupport";

static KernelPatcher::KextInfo kextRadeonSupport = {"com.apple.kext.AMDSupport", &pathRadeonSupport, 1, {}, {},
    KernelPatcher::KextInfo::Unloaded};

static const char *powerGatingFlags[] {"CAIL_DisableDrmdmaPowerGating", "CAIL_DisableGfxCGPowerGating",
    "CAIL_DisableUVDPowerGating", "CAIL_DisableVCEPowerGating", "CAIL_DisableDynamicGfxMGPowerGating",
    "CAIL_DisableGmcPowerGating", "CAIL_DisableAcpPowerGating", "CAIL_DisableSAMUPowerGating"};

Support *Support::callback = nullptr;

void Support::init() {
    callback = this;
    lilu.onKextLoadForce(&kextRadeonSupport);
    dviSingleLink = checkKernelArgument("-lreddvi");
}

bool Support::processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
    if (kextRadeonSupport.loadIndex == index) {
        LRed::callback->setRMMIOIfNecessary();
        auto vbiosdbg = checkKernelArgument("-lredvbiosdbg");
        auto condbg = checkKernelArgument("-lredcondbg");
        auto adcpatch = checkKernelArgument("-lredadcpatch");
        auto gpiodbg = checkKernelArgument("-lredgpiodbg");
        auto isCarrizo = (LRed::callback->chipType >= ChipType::Carrizo);

        RouteRequestPlus requests[] = {
            {"__ZN13ATIController20populateDeviceMemoryE13PCI_REG_INDEX", wrapPopulateDeviceMemory,
                orgPopulateDeviceMemory},
            {"__ZN16AtiDeviceControl16notifyLinkChangeE31kAGDCRegisterLinkControlEvent_tmj", wrapNotifyLinkChange,
                orgNotifyLinkChange},
            {"__ZN13ATIController8TestVRAME13PCI_REG_INDEXb", doNotTestVram},
            {"__ZN30AtiObjectInfoTableInterface_V120getAtomConnectorInfoEjRNS_17AtomConnectorInfoE",
                wrapGetAtomConnectorInfo, orgGetAtomConnectorInfo, condbg},
            {"__ZN30AtiObjectInfoTableInterface_V121getNumberOfConnectorsEv", wrapGetNumberOfConnectors,
                orgGetNumberOfConnectors},
            {"__ZN24AtiAtomFirmwareInterface16createAtomParserEP18BiosParserServicesPh11DCE_Version",
                wrapCreateAtomBiosParser, orgCreateAtomBiosParser, vbiosdbg},
            {"__ZN25AtiGpioPinLutInterface_V114getGpioPinInfoEjRNS_11GpioPinInfoE", wrapGetGpioPinInfo,
                orgGetGpioPinInfo, gpiodbg},
            {"__ZN14AtiBiosParser116getConnectorInfoEP13ConnectorInfoRh", wrapGetConnectorsInfo, orgGetConnectorsInfo},
            {"__ZN14AtiBiosParser126translateAtomConnectorInfoERN30AtiObjectInfoTableInterface_"
             "V117AtomConnectorInfoER13ConnectorInfo",
                wrapTranslateAtomConnectorInfo, orgTranslateAtomConnectorInfo},
            {"__ZN13ATIController5startEP9IOService", wrapATIControllerStart, orgATIControllerStart},
            {"__ZN14AtiGpuWrangler5startEP9IOService", wrapAtiGpuWranglerStart, orgAtiGpuWranglerStart},
            {"__ZN13ATIController10doGPUPanicEPKcz", wrapDoGPUPanic},
            {"__ZN14AtiVBiosHelper8getImageEjj", wrapGetImage, orgGetImage},
            {"__ZN30AtiObjectInfoTableInterface_V14initERN21AtiDataTableBaseClass17DataTableInitInfoE",
                wrapObjectInfoTableInit, orgObjectInfoTableInit},
        };
        PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, requests, address, size), "support",
            "Failed to route symbols");

        LookupPatchPlus const patches[] = {
            {&kextRadeonSupport, kAtiDeviceControlGetVendorInfoOriginal, kAtiDeviceControlGetVendorInfoMask,
                kAtiDeviceControlGetVendorInfoPatched, kAtiDeviceControlGetVendorInfoMask,
                arrsize(kAtiDeviceControlGetVendorInfoOriginal), 1, adcpatch},
            {&kextRadeonSupport, kAtiBiosParser1SetDisplayClockOriginal, kAtiBiosParser1SetDisplayClockPatched,
                arrsize(kAtiBiosParser1SetDisplayClockOriginal), 1, isCarrizo},
        };
        PANIC_COND(!LookupPatchPlus::applyAll(patcher, patches, address, size), "support",
            "Failed to apply patches: %d", patcher.getError());
        DBGLOG("support", "Applied patches.");

        return true;
    }

    return false;
}

IOReturn Support::wrapPopulateDeviceMemory(void *that, uint32_t reg) {
    DBGLOG("support", "populateDeviceMemory: this = %p reg = 0x%X", that, reg);
    auto ret = FunctionCast(wrapPopulateDeviceMemory, callback->orgPopulateDeviceMemory)(that, reg);
    DBGLOG("support", "populateDeviceMemory returned 0x%X", ret);
    (void)ret;
    return kIOReturnSuccess;
}

bool Support::wrapNotifyLinkChange(void *atiDeviceControl, kAGDCRegisterLinkControlEvent_t event, void *eventData,
    uint32_t eventFlags) {
    auto ret = FunctionCast(wrapNotifyLinkChange, callback->orgNotifyLinkChange)(atiDeviceControl, event, eventData,
        eventFlags);

    if (event == kAGDCValidateDetailedTiming) {
        auto cmd = static_cast<AGDCValidateDetailedTiming_t *>(eventData);
        DBGLOG("support", "AGDCValidateDetailedTiming %u -> %d (%u)", cmd->framebufferIndex, ret, cmd->modeStatus);
        if (ret == false || cmd->modeStatus < 1 || cmd->modeStatus > 3) {
            cmd->modeStatus = 2;
            ret = true;
        }
    }

    return ret;
}

bool Support::doNotTestVram([[maybe_unused]] IOService *ctrl, [[maybe_unused]] uint32_t reg,
    [[maybe_unused]] bool retryOnFail) {
    DBGLOG("support", "TestVRAM called! Returning true");
    auto *model = getBranding(LRed::callback->deviceId, LRed::callback->pciRevision);
    // Why do we set it here?
    // Our controller kexts override it, and since TestVRAM is later on after the controllers have started up, it's
    // desirable to do it here rather than later
    if (model) {
        auto len = static_cast<uint32_t>(strlen(model) + 1);
        LRed::callback->iGPU->setProperty("model", const_cast<char *>(model), len);
        LRed::callback->iGPU->setProperty("ATY,FamilyName", const_cast<char *>("Radeon"), 7);
        LRed::callback->iGPU->setProperty("ATY,DeviceName", const_cast<char *>(model) + 11, len - 11);
    }
    return true;
}

bool Support::wrapAtiGpuWranglerStart(IOService *ctrl, IOService *provider) {
    callback->count = callback->count++;
    if (callback->count == 2) {
        IOSleep(3600000);    // keep AMD9000Controller in a limbo state, lasts for around an hour
        return false;        // we don't want AMD9000Controller overriding AMD9500Controller.
    }
    DBGLOG("support", "starting wrangler " PRIKADDR, CASTKADDR(current_thread()));
    bool r = FunctionCast(wrapAtiGpuWranglerStart, callback->orgAtiGpuWranglerStart)(ctrl, provider);
    DBGLOG("support", "starting wrangler done %d " PRIKADDR, r, CASTKADDR(current_thread()));
    return r;
}

bool Support::wrapATIControllerStart(IOService *ctrl, IOService *provider) {
    if (callback->count == 2) {
        IOSleep(3600000);    // keep AMD9000Controller in a limbo state, lasts for around an hour
        return false;        // we don't want AMD9000Controller overriding AMD9500Controller.
    }
    DBGLOG("support", "starting controller " PRIKADDR, CASTKADDR(current_thread()));
    callback->currentPropProvider.set(provider);
    bool r = FunctionCast(wrapATIControllerStart, callback->orgATIControllerStart)(ctrl, provider);
    DBGLOG("support", "starting controller done %d " PRIKADDR, r, CASTKADDR(current_thread()));
    callback->currentPropProvider.erase();
    return r;
}

void Support::applyPropertyFixes(IOService *service, uint32_t connectorNum) {
    if (service) {
        // Starting with 10.13.2 this is important to fix sleep issues due to enforced 6 screens
        if (!service->getProperty("CFG,CFG_FB_LIMIT")) {
            DBGLOG("support", "setting fb limit to %u", connectorNum);
            service->setProperty("CFG_FB_LIMIT", connectorNum, 32);
        }

        // In the past we set CFG_USE_AGDC to false, which caused visual glitches and broken multimonitor support.
        // A better workaround is to disable AGDP just like we do globally.
    }
}

void Support::autocorrectConnector(uint8_t connector, uint8_t sense, uint8_t txmit, uint8_t enc, Connector *connectors,
    uint8_t sz) {
    if (callback->dviSingleLink) {
        if (connector != CONNECTOR_OBJECT_ID_DUAL_LINK_DVI_I && connector != CONNECTOR_OBJECT_ID_DUAL_LINK_DVI_D &&
            connector != CONNECTOR_OBJECT_ID_LVDS) {
            DBGLOG("support", "autocorrectConnector found unsupported connector type %02X", connector);
            return;
        }

        auto fixTransmit = [](auto &con, uint8_t idx, uint8_t sense, uint8_t txmit) {
            if (con.sense == sense) {
                if (con.transmitter != txmit && (con.transmitter & 0xCF) == con.transmitter) {
                    DBGLOG("support", "autocorrectConnector replacing txmit %02X with %02X for %u connector sense %02X",
                        con.transmitter, txmit, idx, sense);
                    con.transmitter = txmit;
                }
                return true;
            }
            return false;
        };

        for (uint8_t j = 0; j < sz; j++) {
            auto &con = (&connectors->modern)[j];
            fixTransmit(con, j, sense, txmit);
        }
    } else {
        DBGLOG("support", "autocorrectConnector use -lreddvi to enable dvi autocorrection");
    }
}

void Support::updateConnectorsInfo(void *atomutils, t_getAtomObjectTableForType gettable, IOService *ctrl,
    Connector *connectors, uint8_t *sz) {
    if (atomutils) {
        DBGLOG("support", "getConnectorsInfo found %u connectors", *sz);
        print(connectors, *sz);
    }

    // Check if the user wants to override automatically detected connectors
    auto cons = ctrl->getProperty("connectors");
    if (cons) {
        auto consData = OSDynamicCast(OSData, cons);
        if (consData) {
            auto consPtr = consData->getBytesNoCopy();
            auto consSize = consData->getLength();

            uint32_t consCount;
            if (WIOKit::getOSDataValue(ctrl, "connector-count", consCount)) {
                *sz = consCount;
                DBGLOG("support", "getConnectorsInfo got size override to %u", *sz);
            }

            if (consPtr && consSize > 0 && *sz > 0 && valid(consSize, *sz)) {
                copy(connectors, *sz, static_cast<const Connector *>(consPtr), consSize);
                DBGLOG("support", "getConnectorsInfo installed %u connectors", *sz);
                applyPropertyFixes(ctrl, *sz);
            } else {
                DBGLOG("support", "getConnectorsInfo conoverrides have invalid size %u for %u num", consSize, *sz);
            }
        } else {
            DBGLOG("support", "getConnectorsInfo conoverrides have invalid type");
        }
    } else {
        if (atomutils) {
            DBGLOG("support", "getConnectorsInfo attempting to autofix connectors");
            uint8_t sHeader = 0, displayPathNum = 0, connectorObjectNum = 0;
            auto baseAddr =
                static_cast<uint8_t *>(gettable(atomutils, AtomObjectTableType::Common, &sHeader)) - sizeof(uint32_t);
            auto displayPaths = static_cast<AtomDisplayObjectPath *>(
                gettable(atomutils, AtomObjectTableType::DisplayPath, &displayPathNum));
            auto connectorObjects = static_cast<AtomConnectorObject *>(
                gettable(atomutils, AtomObjectTableType::ConnectorObject, &connectorObjectNum));
            if (displayPathNum == connectorObjectNum)
                autocorrectConnectors(baseAddr, displayPaths, displayPathNum, connectorObjects, connectorObjectNum,
                    connectors, *sz);
            else
                DBGLOG("support", "getConnectorsInfo found different displaypaths %u and connectors %u", displayPathNum,
                    connectorObjectNum);
        }

        applyPropertyFixes(ctrl, *sz);
    }

    DBGLOG("support", "getConnectorsInfo resulting %u connectors follow", *sz);
    print(connectors, *sz);
}

void Support::autocorrectConnectors(uint8_t *baseAddr, AtomDisplayObjectPath *displayPaths, uint8_t displayPathNum,
    AtomConnectorObject *connectorObjects, uint8_t connectorObjectNum, Connector *connectors, uint8_t sz) {
    for (uint8_t i = 0; i < displayPathNum; i++) {
        if (!isEncoder(displayPaths[i].usGraphicObjIds)) {
            DBGLOG("support", "autocorrectConnectors not encoder %X at %u", displayPaths[i].usGraphicObjIds, i);
            continue;
        }

        uint8_t txmit = 0, enc = 0;
        if (!getTxEnc(displayPaths[i].usGraphicObjIds, txmit, enc)) continue;

        uint8_t sense = getSenseID(baseAddr + connectorObjects[i].usRecordOffset);
        if (!sense) {
            DBGLOG("support", "autocorrectConnectors failed to detect sense for %u connector", i);
            continue;
        }

        DBGLOG("support", "autocorrectConnectors found txmit %02X enc %02X sense %02X for %u connector", txmit, enc,
            sense, i);
    }
}

IOReturn Support::wrapGetConnectorsInfo(void *that, Connector *connectors, uint8_t *sz) {
    auto props = callback->currentPropProvider.get();
    callback->updateConnectorsInfo(nullptr, nullptr, *props, connectors, sz);
    void *objtableinterface = getMember<void *>(that, 0x118);
    IOReturn code = FunctionCast(wrapGetConnectorsInfo, callback->orgGetConnectorsInfo)(that, connectors, sz);

    if (code == kIOReturnSuccess && sz && props && *props) {
        callback->updateConnectorsInfo(nullptr, nullptr, *props, connectors, sz);
    } else {
        DBGLOG("support", "getConnectorsInfo failed %X or undefined %d", code, props == nullptr);
        callback->updateConnectorsInfo(nullptr, nullptr, *props, connectors, sz);
        return kIOReturnSuccess;
    }

    return code;
}

IOReturn Support::wrapTranslateAtomConnectorInfo(void *that, AtomConnectorInfo *info, Connector *connector) {
    IOReturn code =
        FunctionCast(wrapTranslateAtomConnectorInfo, callback->orgTranslateAtomConnectorInfo)(that, info, connector);

    if (code == kIOReturnSuccess && info && connector) {
        print(connector, 1);

        uint8_t sense = getSenseID(info->i2cRecord);
        if (sense) {
            DBGLOG("support", "translateAtomConnectorInfo got sense id %02X", sense);
            uint8_t ucNumberOfSrc = info->hpdRecord[0];
            for (uint8_t i = 0; i < ucNumberOfSrc; i++) {
                auto usSrcObjectID =
                    *reinterpret_cast<uint16_t *>(info->hpdRecord + sizeof(uint8_t) + i * sizeof(uint16_t));
                DBGLOG("support", "translateAtomConnectorInfo checking %04X object id", usSrcObjectID);
                if (((usSrcObjectID & OBJECT_TYPE_MASK) >> OBJECT_TYPE_SHIFT) == GRAPH_OBJECT_TYPE_ENCODER) {
                    uint8_t txmit = 0, enc = 0;
                    if (getTxEnc(usSrcObjectID, txmit, enc))
                        callback->autocorrectConnector(getConnectorID(info->usConnObjectId),
                            getSenseID(info->i2cRecord), txmit, enc, connector, 1);
                    break;
                }
            }

        } else {
            DBGLOG("support", "translateAtomConnectorInfo failed to detect sense for translated connector");
        }
    }

    return code;
}

IOReturn Support::wrapGetAtomConnectorInfo(void *that, uint32_t connector, AtomConnectorInfo *coninfo) {
    DBGLOG("support", "getAtomConnectorInfo: connector %x", connector);
    auto ret = FunctionCast(wrapGetAtomConnectorInfo, callback->orgGetAtomConnectorInfo)(that, connector, coninfo);
    DBGLOG("support", "getAtomConnectorInfo: returned %x", ret);
    return ret;
}

IOReturn Support::wrapGetGpioPinInfo(void *that, uint32_t pin, void *pininfo) {
    auto member = getMember<uint32_t>(that, 0x30);
    DBGLOG("support", "getGpioPinInfo: pin %x, member: %x", pin, member);
    (void)member;    // to also get clang-analyze to shut up
    auto ret = FunctionCast(wrapGetGpioPinInfo, callback->orgGetGpioPinInfo)(that, pin, pininfo);
    DBGLOG("support", "getGpioPinInfo: returned %x", ret);
    return ret;
}

uint32_t Support::wrapGetNumberOfConnectors(void *that) {
    auto ret = FunctionCast(wrapGetNumberOfConnectors, callback->orgGetNumberOfConnectors)(that);
    DBGLOG("support", "getNumberOfConnectors returned: %x", ret);
    return ret;
}

void *Support::wrapCreateAtomBiosParser(void *that, void *param1, unsigned char *param2, uint32_t dceVersion) {
    DBGLOG("support", "wrapCreateAtomBiosParser: DCE_Version: %d", dceVersion);
    getMember<uint32_t>(param1, 0x4) = 0xFF;
    auto ret =
        FunctionCast(wrapCreateAtomBiosParser, callback->orgCreateAtomBiosParser)(that, param1, param2, dceVersion);
    return ret;
}

void Support::wrapDoGPUPanic() {
    DBGLOG("support", "doGPUPanic << ()");
    while (true) { IOSleep(3600000); }
}

void *Support::wrapGetImage(void *that, uint32_t offset, uint32_t length) {
    DBGLOG_COND(length == 0x12, "support", "Object Info Table is v1.3");
    if ((length == 0x12 || length == 0x10) && (callback->objectInfoFound == false)) {
        DBGLOG("support", "Current Object Info Table Offset = 0x%x", offset);
        callback->currentObjectInfoOffset = offset;
        callback->objectInfoFound = true;
    }
    DBGLOG("support", "getImage: offset: %x, length %x", offset, length);
    auto ret = FunctionCast(wrapGetImage, callback->orgGetImage)(that, offset, length);
    DBGLOG("support", "getImage: returned %x", ret);
    return ret;
}

static constexpr int CONNECTOR_Unknown = 0;
static constexpr int CONNECTOR_VGA = 1;
static constexpr int CONNECTOR_DVII = 2;
static constexpr int CONNECTOR_DVID = 3;
static constexpr int CONNECTOR_DVIA = 4;
static constexpr int CONNECTOR_Composite = 5;
static constexpr int CONNECTOR_SVIDEO = 6;
static constexpr int CONNECTOR_LVDS = 7;
static constexpr int CONNECTOR_Component = 8;
static constexpr int CONNECTOR_9PinDIN = 9;
static constexpr int CONNECTOR_DisplayPort = 10;
static constexpr int CONNECTOR_HDMIA = 11;
static constexpr int CONNECTOR_HDMIB = 12;
static constexpr int CONNECTOR_TV = 13;
static constexpr int CONNECTOR_eDP = 14;
static constexpr int CONNECTOR_VIRTUAL = 15;
static constexpr int CONNECTOR_DSI = 16;
static constexpr int CONNECTOR_DPI = 17;
static constexpr int CONNECTOR_WRITEBACK = 18;
static constexpr int CONNECTOR_SPI = 19;
static constexpr int CONNECTOR_USB = 20;

static const int object_connector_convert[] = {CONNECTOR_Unknown, CONNECTOR_DVII, CONNECTOR_DVII, CONNECTOR_DVID,
    CONNECTOR_DVID, CONNECTOR_VGA, CONNECTOR_Composite, CONNECTOR_SVIDEO, CONNECTOR_Unknown, CONNECTOR_Unknown,
    CONNECTOR_9PinDIN, CONNECTOR_Unknown, CONNECTOR_HDMIA, CONNECTOR_HDMIB, CONNECTOR_LVDS, CONNECTOR_9PinDIN,
    CONNECTOR_Unknown, CONNECTOR_Unknown, CONNECTOR_Unknown, CONNECTOR_DisplayPort, CONNECTOR_eDP, CONNECTOR_Unknown};

bool Support::wrapObjectInfoTableInit(void *that, void *initdata) {
    auto ret = FunctionCast(wrapObjectInfoTableInit, callback->orgObjectInfoTableInit)(that, initdata);
    struct ATOMObjHeader *objHdr = getMember<ATOMObjHeader *>(that, 0x28);    // ?
    DBGLOG("support", "objectInfoTable values: conObjTblOff: %x, encObjTblOff: %x, dispPathTblOff: %x",
        objHdr->connectorObjectTableOffset, objHdr->encoderObjectTableOffset, objHdr->displayPathTableOffset);
    struct ATOMObjTable *conInfoTbl = getMember<ATOMObjTable *>(that, 0x38);
    void *vbiosparser = getMember<void *>(that, 0x10);
    ATOMDispObjPathTable *dispPathTable = static_cast<ATOMDispObjPathTable *>(FunctionCast(wrapGetImage,
        callback->orgGetImage)(vbiosparser, callback->currentObjectInfoOffset + objHdr->displayPathTableOffset, 0xE));
    DBGLOG("support", "dispObjPathTable: numDispPaths = 0x%x, version: 0x%x", dispPathTable->numOfDispPath,
        dispPathTable->version);
    auto n = dispPathTable->numOfDispPath;
    DBGLOG("lred", "Fixing VBIOS connectors");
    for (size_t i = 0, j = 0; i < n; i++) {
        // Skip invalid device tags
        if (dispPathTable->dispPath[i].deviceTag) {
            uint8_t conObjType = (conInfoTbl->objects[i].objectID & OBJECT_TYPE_MASK) >> OBJECT_TYPE_SHIFT;
            DBGLOG("support", "connectorInfoTable: connector: %x, objects: %x, objectId: %x, objectTypeFromId: %x", i,
                conInfoTbl->numberOfObjects, conInfoTbl->objects[i].objectID,
                (conInfoTbl->objects[i].objectID & OBJECT_TYPE_MASK) >> OBJECT_TYPE_SHIFT);
            if (conObjType != GRAPH_OBJECT_TYPE_CONNECTOR) {
                SYSLOG("lred", "Connector %x is not GRAPH_OBJECT_TYPE_CONNECTOR!, objectType: %x", i, conObjType);
                conInfoTbl->numberOfObjects--;
                dispPathTable->numOfDispPath--;
            } else {
                conInfoTbl->objects[j++] = conInfoTbl->objects[i];
                dispPathTable->dispPath[j] = dispPathTable->dispPath[i];
            }
        } else {
            dispPathTable->numOfDispPath--;
            conInfoTbl->numberOfObjects--;
        }
    }
    DBGLOG("lred", "Results: numOfDispPath: 0x%x, numberOfObjects: 0x%x", dispPathTable->numOfDispPath,
        conInfoTbl->numberOfObjects);
    return ret;
}
