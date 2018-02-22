/*
 * ADSpinnaker.cpp
 *
 * This is a driver for FLIR (PointGrey) GigE, 10GigE, and USB3 cameras using their Spinnaker SDK
 *
 * Author: Mark Rivers
 *         University of Chicago
 *
 * Created: February 3, 2018
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <set>

#include <epicsEvent.h>
#include <epicsTime.h>
#include <epicsThread.h>
#include <epicsMessageQueue.h>
#include <iocsh.h>
#include <cantProceed.h>
#include <epicsString.h>
#include <epicsExit.h>

#include "Spinnaker.h"
#include "SpinGenApi/SpinnakerGenApi.h"

using namespace Spinnaker;
using namespace Spinnaker::GenApi;
using namespace Spinnaker::GenICam;
using namespace std;

#include "ADDriver.h"

#include <epicsExport.h>

#define DRIVER_VERSION      1
#define DRIVER_REVISION     0
#define DRIVER_MODIFICATION 0

static const char *driverName = "ADSpinnaker";

/* Spinnaker driver specific parameters */
#define SPPropImplementedString   "SP_PROP_IMPLEMENTED"
#define SPPropIntValueString      "SP_PROP_INT_VALUE"
#define SPPropFloatValueString    "SP_PROP_FLOAT_VALUE"
#define SPPropStringValueString   "SP_PROP_STRING_VALUE"
#define SPPropMinValueString      "SP_PROP_MIN_VALUE"
#define SPPropMaxValueString      "SP_PROP_MAX_VALUE"

typedef enum {
    SPVideoMode,            // 0
    SPFrameRate,            // 1 
    SPFrameRateAuto,        // 2
    SPFrameRateEnable,      // 3
    SPExposure,             // 4
    SPExposureAuto,         // 5
    SPGain,                 // 6
    SPGainAuto,             // 7
    SPBlackLevel,           // 8
    SPBlackLevelAuto,       // 9
    SPBlackLevelBalanceAuto,// 10
    SPSaturation,           // 11
    SPSaturationEnable,     // 12
    SPGamma,                // 13
    SPGammaEnable,          // 14
    SPSharpening,           // 15
    SPSharpeningAuto,       // 16
    SPSharpeningEnable,     // 17
    SPPixelFormat,          // 18
    SPConvertPixelFormat,   // 19
    SPTriggerSource,        // 20
    SPTriggerActivation,    // 21
    SPTriggerDelay,         // 22
    SPSoftwareTrigger,      // 23
    SPBalanceRatio,         // 24
    SPBalanceRatioSelector, // 25
    SPBalanceWhiteAuto,     // 26
    SP_NUM_PROPERTIES
} SPPropertyNum_t; 

/*
#define PGPacketSizeString            "PG_PACKET_SIZE"
#define PGPacketSizeActualString      "PG_PACKET_SIZE_ACTUAL"
#define PGMaxPacketSizeString         "PG_MAX_PACKET_SIZE"
#define PGPacketDelayString           "PG_PACKET_DELAY"
#define PGPacketDelayActualString     "PG_PACKET_DELAY_ACTUAL"
#define PGBandwidthString             "PG_BANDWIDTH"
#define PGTimeStampModeString         "PG_TIME_STAMP_MODE"
#define PGCorruptFramesString         "PG_CORRUPT_FRAMES"
#define PGDriverDroppedString         "PG_DRIVER_DROPPED"
#define PGTransmitFailedString        "PG_TRANSMIT_FAILED"
#define PGDroppedFramesString         "PG_DROPPED_FRAMES"
*/

// Default packet delay in microseconds
#define DEFAULT_PACKET_DELAY 400

// Size of message queue for callback function
#define CALLBACK_MESSAGE_QUEUE_SIZE 10

typedef enum {
    SPPropertyTypeInt,
    SPPropertyTypeBoolean,
    SPPropertyTypeEnum,
    SPPropertyTypeDouble,
    SPPropertyTypeString,
    SPPropertyTypeCmd,
    SPPropertyTypeUnknown
} SPPropertyType_t;

typedef enum {
    SPPixelConvertNone,
    SPPixelConvertMono8,
    SPPixelConvertMono16,
    SPPixelConvertRaw16,
    SPPixelConvertRGB8,
    SPPixelConvertRGB16
} SPPixelConvert_t;


/*
static const char *gigEPropertyTypeStrings[NUM_GIGE_PROPERTIES] = {
    "Heartbeat",
    "HeartbeatTimeout",
    "PacketSize",
    "PacketDelay"
};
*/

typedef enum {
   TimeStampCamera,
   TimeStampEPICS,
   TimeStampHybrid
} PGTimeStamp_t;


class ImageEventHandler : public ImageEvent
{
public:

	ImageEventHandler(epicsMessageQueue *pMsgQ) 
	 : pMsgQ_(pMsgQ)
	{
	}
	~ImageEventHandler() {}

	void OnImageEvent(ImagePtr image)
	{
      static ImagePtr imageCopy;
      imageCopy = image;

      if (pMsgQ_->send(&imageCopy, sizeof(imageCopy)) != 0) {
          printf("OnImageEvent error calling pMsgQ_->send()\n");
      }
	}
	
private:
  epicsMessageQueue *pMsgQ_;

};

/** Main driver class inherited from areaDetectors ADDriver class.
 * One instance of this class will control one camera.
 */
class ADSpinnaker : public ADDriver
{
public:
    ADSpinnaker(const char *portName, int cameraId, int traceMask, int memoryChannel,
                 int maxBuffers, size_t maxMemory,
                 int priority, int stackSize);

    // virtual methods to override from ADDriver
    virtual asynStatus writeInt32( asynUser *pasynUser, epicsInt32 value);
    virtual asynStatus writeFloat64( asynUser *pasynUser, epicsFloat64 value);
    virtual asynStatus readEnum(asynUser *pasynUser, char *strings[], int values[], int severities[], 
                                size_t nElements, size_t *nIn);
    void report(FILE *fp, int details);
    /**< These should be private but are called from C callback functions, must be public. */
    void imageGrabTask();
    void shutdown();

protected:
    int SPPropImplemented;    /** Property implemented                            (int32 read) */
    #define FIRST_SP_PARAM SPPropImplemented
    int SPPropFloatValue;    /** Float value                                      (float64 write/read) */
    int SPPropIntValue;      /** Integer value                                    (int32 write/read) */
    int SPPropStringValue;   /** String                                           (string write/read) */
    int SPPropMinValue;      /** Minimum value                                    (float64 read) */
    int SPPropMaxValue;      /** Maximum value                                    (float64 read) */

//    int PGPacketSize;             /** Size of data packets from camera                (int32 write/read) */
//    int PGPacketSizeActual;       /** Size of data packets from camera                (int32 write/read) */
//    int PGMaxPacketSize;          /** Maximum size of data packets from camera        (int32 write/read) */
//    int PGPacketDelay;            /** Packet delay in usec from camera, GigE only     (int32 write/read) */
//    int PGPacketDelayActual;      /** Packet delay in usec from camera, GigE only     (int32 read) */
//    int PGBandwidth;              /** Bandwidth in MB/s                               (float64 read) */
//    int PGTimeStampMode;          /** Time stamp mode (PGTimeStamp_t)                 (int32 write/read) */
//    int PGCorruptFrames;          /** Number of corrupt frames                        (int32 read) */
//    int PGDriverDropped;          /** Number of driver dropped frames                 (int32 read) */
//    int PGTransmitFailed;         /** Number of transmit failures                     (int32 read) */
//    int PGDroppedFrames;          /** Number of dropped frames                        (int32 read) */

private:
    class SPProperty {
        public:
            SPProperty(ADSpinnaker *pDrvIn, int asynParamIn, int asynAddrIn, const char *nodeNameIn);
            ADSpinnaker *pDrv;
            SPPropertyType_t propertyType;
            int asynParam;
            int asynAddr;
            const char *nodeName;
            CNodePtr pBase;
            bool isImplemented;
            asynStatus setValue(void *value=0, void *readbackValue=0, bool setParam=true);
            asynStatus getValue(void *value=0, bool setParam=true);
            asynStatus update();
   };

    /* Local methods to this class */
    asynStatus grabImage();
    asynStatus startCapture();
    asynStatus stopCapture();

    asynStatus connectCamera();
    asynStatus disconnectCamera();
    asynStatus readStatus();

    SPProperty* findProperty(int asynParam, int asynAddr);

    /* camera property control functions */
    asynStatus setSPProperty(int asynParam, int asynAddr=0, void *value=0, void *readbackValue=0, bool setParam=true);
    asynStatus getSPProperty(int asynParam, int asynAddr=0, void *value=0, bool setParam=true);
    asynStatus updateSPProperties();

    asynStatus setImageParams();
    
    void imageEventCallback(ImagePtr pImage);
    void reportNode(FILE *fp, INodeMap *pNodeMap, const char *nodeName, int level);

    /* Data */
    int cameraId_;
    int memoryChannel_;
    
    INodeMap *pNodeMap_;    
    SystemPtr system_;
    CameraList camList_;
    CameraPtr pCamera_;
    ImagePtr pImage_;
    ImageEventHandler *pImageEventHandler_;
    std::vector<SPProperty*> propertyList_;

    int exiting_;
    epicsEventId startEventId_;
    epicsMessageQueue *pCallbackMsgQ_;
    NDArray *pRaw_;
};

ADSpinnaker::SPProperty::SPProperty(ADSpinnaker *pDrvIn, int asynParamIn, int asynAddrIn, const char *nodeNameIn)

    : pDrv(pDrvIn),
      propertyType(SPPropertyTypeUnknown),
      asynParam(asynParamIn),
      asynAddr(asynAddrIn),
      nodeName(nodeNameIn),
      isImplemented(false)
{
    try {
        if (nodeName) {
            CNodePtr pNode = (CNodePtr)pDrv->pNodeMap_->GetNode(nodeName);
            if (IsImplemented(pNode)) {
                isImplemented = true;
                pBase = pNode;
                switch (pNode->GetPrincipalInterfaceType()) {
                    case intfIString:
                    		propertyType = SPPropertyTypeString;
                     	  break;
                    case  intfIInteger:
                    		propertyType = SPPropertyTypeInt;
                    	  break; 
                    case intfIFloat:
                    		propertyType = SPPropertyTypeDouble;
                    	  break;
                    case intfIBoolean:
                    		propertyType = SPPropertyTypeBoolean;
                    	  break;
                    case intfICommand:
                    		propertyType = SPPropertyTypeCmd;
                    	  break;
                    case intfIEnumeration:
                    		propertyType = SPPropertyTypeEnum;
                    	  break;
                    default:
                       break; 
                }
            }
        }
    }
    catch (Spinnaker::Exception &e)
    {
    	printf("SPProperty::SPProperty exception %s\n", e.what());
    }
}


asynStatus ADSpinnaker::SPProperty::getValue(void *pValue, bool setParam)
{
    static const char *functionName = "SPProperty::getValue";

    if (!isImplemented) return asynError;
    try {
        if (!IsAvailable(pBase)) {
             asynPrint(pDrv->pasynUserSelf, ASYN_TRACE_ERROR, 
                "%s::%s Error: node %s is not available\n",
                driverName, functionName, nodeName);
             return asynError;
        }
        if (!IsReadable(pBase)) {
             asynPrint(pDrv->pasynUserSelf, ASYN_TRACE_WARNING, 
                "%s::%s Warning: node %s is not readable\n",
                driverName, functionName, nodeName);
             return asynError;
        }
        switch (propertyType) {
            case SPPropertyTypeInt: {
                CIntegerPtr pNode = (CIntegerPtr)pBase;
                epicsInt32 value = (epicsInt32)pNode->GetValue();
                if (pValue) *(epicsInt32*)pValue = value;
                if (setParam) pDrv->setIntegerParam(asynAddr, asynParam, value);
                break;
            }
            case SPPropertyTypeBoolean: {
                CBooleanPtr pNode = (CBooleanPtr)pBase;
                epicsInt32 value = (epicsInt32)pNode->GetValue();
                if (pValue) *(epicsInt32*)pValue = value;
                if (setParam) pDrv->setIntegerParam(asynAddr, asynParam, value);
                break;
            }
            case SPPropertyTypeDouble: {
                CFloatPtr pNode = (CFloatPtr)pBase;
                epicsFloat64 value = (epicsFloat64)pNode->GetValue();
                if (pValue) *(epicsFloat64*)pValue = value;
                if (setParam) pDrv->setDoubleParam(asynAddr, asynParam, value);
                break;
            }
            case SPPropertyTypeEnum: {
                CEnumerationPtr pNode = (CEnumerationPtr)pBase;
                epicsInt32 value = (epicsInt32)pNode->GetIntValue();
                if (pValue) *(epicsInt32*)pValue = value;
                if (setParam) pDrv->setIntegerParam(asynAddr, asynParam, value);
                break;
            }
            case SPPropertyTypeString: {
                CStringPtr pNode = (CStringPtr)pBase;
                std::string value = epicsStrDup((pNode->GetValue()).c_str());
                if (pValue) *(std::string *)pValue = value;
                if (setParam) pDrv->setStringParam(asynAddr, asynParam, value);
                break;
            }
            case SPPropertyTypeCmd: {
                break;
            }
            default:
                break;
        }
    }
    catch (Spinnaker::Exception &e) {
        asynPrint(pDrv->pasynUserSelf, ASYN_TRACE_ERROR, 
            "%s::%s node %s exception %s\n",
            driverName, functionName, nodeName, e.what());
        return asynError;
    }
    return asynSuccess;
}


asynStatus ADSpinnaker::SPProperty::update()
{
    static const char *functionName = "SPProperty::update";

    if (!isImplemented) return asynError;
    try {
        if ((propertyType == SPPropertyTypeEnum) &&
            (!IsAvailable(pBase) || !IsWritable(pBase))) {
            char *enumStrings[1];
            int enumValues[1];
            int enumSeverities[1];
            enumStrings[0] = epicsStrDup("N.A.");
            enumValues[0] = 0;
            enumSeverities[0] = 0;
            pDrv->doCallbacksEnum(enumStrings, enumValues, enumSeverities, 
                                  1, asynParam, asynAddr);
            return asynSuccess;
        }
        if (!IsAvailable(pBase)) {
             asynPrint(pDrv->pasynUserSelf, ASYN_TRACE_ERROR, 
                "%s::%s Error: node %s is not available\n",
                driverName, functionName, nodeName);
             return asynError;
        }
        if (!IsReadable(pBase)) {
             asynPrint(pDrv->pasynUserSelf, ASYN_TRACE_WARNING, 
                "%s::%s Warning: node %s is not readable\n",
                driverName, functionName, nodeName);
             return asynError;
        }
        switch (propertyType) {
            case SPPropertyTypeInt: {
                CIntegerPtr pNode = (CIntegerPtr)pBase;
                epicsInt32 value = (epicsInt32)pNode->GetValue();
                pDrv->setIntegerParam(asynAddr, asynParam, value);
                pDrv->setDoubleParam(asynAddr, pDrv->SPPropMinValue, (epicsInt32)pNode->GetMin());
                pDrv->setDoubleParam(asynAddr, pDrv->SPPropMaxValue, (epicsInt32)pNode->GetMax());
                break;
            }
            case SPPropertyTypeBoolean: {
                CBooleanPtr pNode = (CBooleanPtr)pBase;
                epicsInt32 value = (epicsInt32)pNode->GetValue();
                pDrv->setIntegerParam(asynAddr, asynParam, value);
                break;
            }
            case SPPropertyTypeDouble: {
                CFloatPtr pNode = (CFloatPtr)pBase;
                epicsFloat64 value = (epicsFloat64)pNode->GetValue();
                pDrv->setDoubleParam(asynAddr, asynParam, value);
                if (asynParam == pDrv->SPPropFloatValue) {
                    pDrv->setDoubleParam(asynAddr, pDrv->SPPropMinValue, pNode->GetMin());
                    pDrv->setDoubleParam(asynAddr, pDrv->SPPropMaxValue, pNode->GetMax());
printf("node %s asynAddr=%d, min=%f, max=%f\n", nodeName, asynAddr, pNode->GetMin(), pNode->GetMax());
                }
                break;
            }
            case SPPropertyTypeEnum: {
                CEnumerationPtr pNode = (CEnumerationPtr)pBase;
                epicsInt32 value = (epicsInt32)pNode->GetIntValue();
                pDrv->setIntegerParam(asynAddr, asynParam, value);
                NodeList_t entries;
                pNode->GetEntries(entries);
                int numEnums = (int)entries.size();
                char **enumStrings = new char*[numEnums];
                int *enumValues = new int[numEnums];
                int *enumSeverities = new int[numEnums];
                int j=0;
                for (int i=0; i<numEnums; i++) {
                    IEnumEntry *pEntry= dynamic_cast<IEnumEntry *>(entries[i]);
                    const char *pString = epicsStrDup((pEntry->GetSymbolic()).c_str());
                    if (IsAvailable(pEntry) && IsReadable(pEntry)) {
                        enumStrings[j] = epicsStrDup(pString);
                        long long entryValue = pEntry->GetValue();
                        enumValues[j] = (int)entryValue;
                        enumSeverities[j] = 0;
                        j++;
                    }
                }
                pDrv->doCallbacksEnum(enumStrings, enumValues, enumSeverities, 
                                      j, asynParam, asynAddr);
                delete [] enumStrings; delete [] enumValues; delete [] enumSeverities;
                break;
            }
            case SPPropertyTypeString: {
                CStringPtr pNode = (CStringPtr)pBase;
                std::string value = epicsStrDup((pNode->GetValue()).c_str());
                pDrv->setStringParam(asynAddr, asynParam, value);
                break;
            }
            case SPPropertyTypeCmd: {
                break;
            }
            default:
                break;
        }
    }
    catch (Spinnaker::Exception &e) {
        asynPrint(pDrv->pasynUserSelf, ASYN_TRACE_ERROR, 
            "%s::%s node %s exception %s\n",
            driverName, functionName, nodeName, e.what());
        return asynError;
    }
    return asynSuccess;
}


asynStatus ADSpinnaker::SPProperty::setValue(void *pValue, void *pReadbackValue, bool setParam)
{
    static const char *functionName = "SPProperty::setValue";

    if (!isImplemented) return asynError;
    try {
        if (!IsAvailable(pBase)) {
             asynPrint(pDrv->pasynUserSelf, ASYN_TRACE_ERROR, 
                "%s::%s Error: node %s is not available\n",
                driverName, functionName, nodeName);
             return asynError;
        }
        if (!IsWritable(pBase)) {
             asynPrint(pDrv->pasynUserSelf, ASYN_TRACE_WARNING, 
                "%s::%s Warning: node %s is not writable\n",
                driverName, functionName, nodeName);
             return asynError;
        }
        switch (propertyType) {
            case SPPropertyTypeInt: {
                CIntegerPtr pNode = (CIntegerPtr)pBase;
                epicsInt32 value;
                if (pValue)
                    value = *(epicsInt32*)pValue;
                else 
                    pDrv->getIntegerParam(asynAddr, asynParam, &value);
                // Check against the min and max
                int max = (int)pNode->GetMax();
                int min = (int)pNode->GetMin();
                int inc = (int)pNode->GetInc();
                if (inc != 1) {
                    value = (value/inc) * inc;
                }
                if (value < min) {
                   asynPrint(pDrv->pasynUserSelf, ASYN_TRACE_WARNING, 
                        "%s::%s Warning: node %s value %d is less than minimum %d, setting to minimum\n",
                        driverName, functionName, nodeName, value, min);
                    value = min;
                }
                if (value > max) {
                   asynPrint(pDrv->pasynUserSelf, ASYN_TRACE_WARNING, 
                        "%s::%s Warning: node %s value %d is greater than maximum %d, setting to maximum\n",
                        driverName, functionName, nodeName, value, max);
                    value = max;
                }
                pNode->SetValue(value);
                asynPrint(pDrv->pasynUserSelf, ASYN_TRACEIO_DRIVER, 
                    "%s::%s set property %s to %d\n",
                    driverName, functionName, nodeName, value);
                if (IsReadable(pNode)) {
                    epicsInt32 readback = (epicsInt32)pNode->GetValue();
                    if (pReadbackValue) *(epicsInt32*)pReadbackValue = readback;
                    asynPrint(pDrv->pasynUserSelf, ASYN_TRACEIO_DRIVER, 
                        "%s::%s readback property %s is %d\n",
                        driverName, functionName, nodeName, readback);
                    if (setParam) pDrv->setIntegerParam(asynAddr, asynParam, readback);
                }
                break;
            }
            case SPPropertyTypeBoolean: {
                CBooleanPtr pNode = (CBooleanPtr)pBase;
                epicsInt32 value;
                if (pValue) 
                    value = *(epicsInt32*)pValue;
                else
                    pDrv->getIntegerParam(asynAddr, asynParam, &value);
                *pNode = value ? true : false;
                asynPrint(pDrv->pasynUserSelf, ASYN_TRACEIO_DRIVER, 
                    "%s::%s set property %s to %d\n",
                    driverName, functionName, nodeName, value);
                if (IsReadable(pNode)) {
                    epicsInt32 readback = (epicsInt32)pNode->GetValue();
                    if (pReadbackValue) *(epicsInt32*)pReadbackValue = readback;
                    asynPrint(pDrv->pasynUserSelf, ASYN_TRACEIO_DRIVER, 
                        "%s::%s readback property %s is %d\n",
                        driverName, functionName, nodeName, readback);
                    if (setParam) pDrv->setIntegerParam(asynAddr, asynParam, readback);
                }
                break;
            }
            case SPPropertyTypeDouble: {
                CFloatPtr pNode = (CFloatPtr)pBase;
                epicsFloat64 value;
                if (pValue) 
                    value = *(epicsFloat64*)pValue;
                else
                    pDrv->getDoubleParam(asynAddr, asynParam, &value);
                // Check against the min and max
                double max = pNode->GetMax();
                double min = pNode->GetMin();
                if (value < min) {
                   asynPrint(pDrv->pasynUserSelf, ASYN_TRACE_WARNING, 
                        "%s::%s Warning: node %s value %f is less than minimum %f, setting to minimum\n",
                        driverName, functionName, nodeName, value, min);
                    value = min;
                }
                if (value > max) {
                   asynPrint(pDrv->pasynUserSelf, ASYN_TRACE_WARNING, 
                        "%s::%s Warning: node %s value %f is greater than maximum %f, setting to maximum\n",
                        driverName, functionName, nodeName, value, max);
                    value = max;
                }
                pNode->SetValue(value);
                asynPrint(pDrv->pasynUserSelf, ASYN_TRACEIO_DRIVER, 
                    "%s::%s set property %s to %f\n",
                    driverName, functionName, nodeName, value);
                if (IsReadable(pNode)) {
                    double readback = pNode->GetValue();
                    if (pReadbackValue) *(epicsFloat64*)pReadbackValue = readback;
                    asynPrint(pDrv->pasynUserSelf, ASYN_TRACEIO_DRIVER, 
                        "%s::%s readback property %s is %f\n",
                        driverName, functionName, nodeName, readback);
                    if (setParam) pDrv->setDoubleParam(asynAddr, asynParam, readback);
                }
                break;
            }
            case SPPropertyTypeEnum: {
                CEnumerationPtr pNode = (CEnumerationPtr)pBase;
                epicsInt32 value;
                if (pValue) 
                    value = *(epicsInt32*)pValue;
                else
                    pDrv->getIntegerParam(asynAddr, asynParam, &value);
                pNode->SetIntValue(value);
                asynPrint(pDrv->pasynUserSelf, ASYN_TRACEIO_DRIVER, 
                    "%s::%s set property %s to %d\n",
                    driverName, functionName, nodeName, value);
                if (IsReadable(pNode)) {
                    epicsInt32 readback = (epicsInt32)pNode->GetIntValue();
                    if (pReadbackValue) *(epicsInt32*)pReadbackValue = readback;
                    asynPrint(pDrv->pasynUserSelf, ASYN_TRACEIO_DRIVER, 
                        "%s::%s readback property %s is %d\n",
                        driverName, functionName, nodeName, readback);
                    if (setParam) pDrv->setIntegerParam(asynAddr, asynParam, readback);
                }
                break;
            }
            case SPPropertyTypeString: {
                CStringPtr pNode = (CStringPtr)pBase;
                const char *value;
                if (pValue) 
                    value = (const char*)pValue;
                else {
                    std::string temp;
                    pDrv->getStringParam(asynAddr, asynParam, temp);
                    value = temp.c_str();
                }
                pNode->SetValue(value);
                asynPrint(pDrv->pasynUserSelf, ASYN_TRACEIO_DRIVER, 
                    "%s::%s set property %s to %s\n",
                    driverName, functionName, nodeName, value);
                if (IsReadable(pNode)) {
                    std::string readback = epicsStrDup((pNode->GetValue()).c_str());
                    if (pReadbackValue) *(std::string*)pReadbackValue = readback;
                    asynPrint(pDrv->pasynUserSelf, ASYN_TRACEIO_DRIVER, 
                        "%s::%s readback property %s is %s\n",
                        driverName, functionName, nodeName, readback.c_str());
                    if (setParam) pDrv->setStringParam(asynAddr, asynParam, readback);
                }
                break;
            }
            case SPPropertyTypeCmd: {
                CCommandPtr pNode = (CCommandPtr)pBase;
                pNode->Execute();
                asynPrint(pDrv->pasynUserSelf, ASYN_TRACEIO_DRIVER, 
                    "%s::%s executed command %s\n",
                    driverName, functionName, nodeName);
                break;
            }
            default:
                break;
        }
    }
    catch (Spinnaker::Exception &e) {
        asynPrint(pDrv->pasynUserSelf, ASYN_TRACE_ERROR, 
            "%s::%s node %s exception %s\n",
            driverName, functionName, nodeName, e.what());
        return asynError;
    }
    return asynSuccess;
}

/** Configuration function to configure one camera.
 *
 * This function need to be called once for each camera to be used by the IOC. A call to this
 * function instanciates one object from the ADSpinnaker class.
 * \param[in] portName asyn port name to assign to the camera.
 * \param[in] cameraId The camera index or serial number; <1000 is assumed to be index, >=1000 is assumed to be serial number.
 * \param[in] traceMask The initial value of the asynTraceMask.  
 *            If set to 0 or 1 then asynTraceMask will be set to ASYN_TRACE_ERROR.
 *            If set to 0x21 (ASYN_TRACE_WARNING | ASYN_TRACE_ERROR) then each call to the
 *            FlyCap2 library will be traced including during initialization.
 * \param[in] memoryChannel  The camera memory channel (non-volatile memory containing camera parameters) 
 *            to load during initialization.  If 0 no memory channel is loaded.
 *            If >=1 thenRestoreFromMemoryChannel(memoryChannel-1) is called.  
 *            Set memoryChannel to 1 to work around a bug in the Linux GigE driver in R2.0.
 * \param[in] maxBuffers Maxiumum number of NDArray objects (image buffers) this driver is allowed to allocate.
 *            This driver requires 2 buffers, and each queue element in a plugin can require one buffer
 *            which will all need to be added up in this parameter. 0=unlimited.
 * \param[in] maxMemory Maximum memory (in bytes) that this driver is allowed to allocate. So if max. size = 1024x768 (8bpp)
 *            and maxBuffers is, say 14. maxMemory = 1024x768x14 = 11010048 bytes (~11MB). 0=unlimited.
 * \param[in] priority The EPICS thread priority for this driver.  0=use asyn default.
 * \param[in] stackSize The size of the stack for the EPICS port thread. 0=use asyn default.
 */
extern "C" int ADSpinnakerConfig(const char *portName, int cameraId, int traceMask, int memoryChannel, 
                               int maxBuffers, size_t maxMemory, int priority, int stackSize)
{
    new ADSpinnaker( portName, cameraId, traceMask, memoryChannel, maxBuffers, maxMemory, priority, stackSize);
    return asynSuccess;
}


static void c_shutdown(void *arg)
{
  ADSpinnaker *p = (ADSpinnaker *)arg;
  p->shutdown();
}


static void imageGrabTaskC(void *drvPvt)
{
    ADSpinnaker *pPvt = (ADSpinnaker *)drvPvt;

    pPvt->imageGrabTask();
}

/** Constructor for the ADSpinnaker class
 * \param[in] portName asyn port name to assign to the camera.
 * \param[in] cameraId The camera index or serial number; <1000 is assumed to be index, >=1000 is assumed to be serial number.
 * \param[in] traceMask The initial value of the asynTraceMask.  
 *            If set to 0 or 1 then asynTraceMask will be set to ASYN_TRACE_ERROR.
 *            If set to 0x21 (ASYN_TRACE_WARNING | ASYN_TRACE_ERROR) then each call to the
 *            FlyCap2 library will be traced including during initialization.
 * \param[in] memoryChannel  The camera memory channel (non-volatile memory containing camera parameters) 
 *            to load during initialization.  If 0 no memory channel is loaded.
 *            If >=1 thenRestoreFromMemoryChannel(memoryChannel-1) is called.  
 *            Set memoryChannel to 1 to work around a bug in the Linux GigE driver in R2.0.
 * \param[in] maxBuffers Maxiumum number of NDArray objects (image buffers) this driver is allowed to allocate.
 *            This driver requires 2 buffers, and each queue element in a plugin can require one buffer
 *            which will all need to be added up in this parameter. 0=unlimited.
 * \param[in] maxMemory Maximum memory (in bytes) that this driver is allowed to allocate. So if max. size = 1024x768 (8bpp)
 *            and maxBuffers is, say 14. maxMemory = 1024x768x14 = 11010048 bytes (~11MB). 0=unlimited.
 * \param[in] priority The EPICS thread priority for this driver.  0=use asyn default.
 * \param[in] stackSize The size of the stack for the EPICS port thread. 0=use asyn default.
 */
ADSpinnaker::ADSpinnaker(const char *portName, int cameraId, int traceMask, int memoryChannel,
                    int maxBuffers, size_t maxMemory, int priority, int stackSize )
    : ADDriver(portName, SP_NUM_PROPERTIES, 0, maxBuffers, maxMemory,
            asynEnumMask, asynEnumMask,
            ASYN_CANBLOCK | ASYN_MULTIDEVICE, 1, priority, stackSize),
    cameraId_(cameraId), memoryChannel_(memoryChannel), exiting_(0), pRaw_(NULL)
{
    static const char *functionName = "ADSpinnaker";
//    int i;
    //PropertyType propType;
    asynStatus status;
    
    if (traceMask == 0) traceMask = ASYN_TRACE_ERROR;
    pasynTrace->setTraceMask(pasynUserSelf, traceMask);

    createParam(SPPropImplementedString,        asynParamInt32,   &SPPropImplemented);
    createParam(SPPropFloatValueString,       asynParamFloat64,   &SPPropFloatValue);
    createParam(SPPropIntValueString,           asynParamInt32,   &SPPropIntValue);
    createParam(SPPropStringValueString,        asynParamOctet,   &SPPropStringValue);
    createParam(SPPropMinValueString,         asynParamFloat64,   &SPPropMinValue);
    createParam(SPPropMaxValueString,         asynParamFloat64,   &SPPropMaxValue);

/*
    createParam(PGPacketSizeString,             asynParamInt32,   &PGPacketSize);
    createParam(PGPacketSizeActualString,       asynParamInt32,   &PGPacketSizeActual);
    createParam(PGMaxPacketSizeString,          asynParamInt32,   &PGMaxPacketSize);
    createParam(PGPacketDelayString,            asynParamInt32,   &PGPacketDelay);
    createParam(PGPacketDelayActualString,      asynParamInt32,   &PGPacketDelayActual);
    createParam(PGBandwidthString,              asynParamFloat64, &PGBandwidth);
    createParam(PGTimeStampModeString,          asynParamInt32,   &PGTimeStampMode);
    createParam(PGCorruptFramesString,          asynParamInt32,   &PGCorruptFrames);
    createParam(PGDriverDroppedString,          asynParamInt32,   &PGDriverDropped);
    createParam(PGTransmitFailedString,         asynParamInt32,   &PGTransmitFailed);
    createParam(PGDroppedFramesString,          asynParamInt32,   &PGDroppedFrames);
*/
    /* Set initial values of some parameters */
    setIntegerParam(NDDataType, NDUInt8);
    setIntegerParam(NDColorMode, NDColorModeMono);
    setIntegerParam(NDArraySizeZ, 0);
    setIntegerParam(ADMinX, 0);
    setIntegerParam(ADMinY, 0);
    setStringParam(ADStringToServer, "<not used by driver>");
    setStringParam(ADStringFromServer, "<not used by driver>");
    setIntegerParam(SPTriggerSource, 0);

    // Retrieve singleton reference to system object
    system_ = System::GetInstance();

    status = connectCamera();
    if (status) {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
            "%s:%s:  camera connection failed (%d)\n",
            driverName, functionName, status);
        // Call report() to get a list of available cameras
        report(stdout, 1);
        return;
    }

    // Construct property list.
    // String properties
    propertyList_.push_back(new SPProperty(this, ADImageMode,                        0, "AcquisitionMode"));

    propertyList_.push_back(new SPProperty(this, ADSerialNumber,                     0, "DeviceSerialNumber"));
    propertyList_.push_back(new SPProperty(this, ADFirmwareVersion,                  0, "DeviceFirmwareVersion"));
    propertyList_.push_back(new SPProperty(this, ADManufacturer,                     0, "DeviceVendorName"));
    propertyList_.push_back(new SPProperty(this, ADModel,                            0, "DeviceModelName"));

    // Integer properties
    propertyList_.push_back(new SPProperty(this, ADMaxSizeX,                         0, "WidthMax"));
    propertyList_.push_back(new SPProperty(this, ADMaxSizeY,                         0, "HeightMax"));
    propertyList_.push_back(new SPProperty(this, ADSizeX,                            0, "Width"));
    propertyList_.push_back(new SPProperty(this, ADSizeY,                            0, "Height"));
    propertyList_.push_back(new SPProperty(this, ADMinX,                             0, "OffsetX"));
    propertyList_.push_back(new SPProperty(this, ADMinY,                             0, "OffsetY"));
    propertyList_.push_back(new SPProperty(this, ADBinX,                             0, "BinningHorizontal"));
    propertyList_.push_back(new SPProperty(this, ADBinY,                             0, "BinningVertical"));
    propertyList_.push_back(new SPProperty(this, ADNumImages,                        0, "AcquisitionFrameCount"));

    // Command properties
    propertyList_.push_back(new SPProperty(this, SPPropIntValue,     SPSoftwareTrigger, "TriggerSoftware"));

    // Float properties
    propertyList_.push_back(new SPProperty(this, ADAcquireTime,                      0, "ExposureTime"));
    propertyList_.push_back(new SPProperty(this, SPPropFloatValue,          SPExposure, "ExposureTime")); 
    propertyList_.push_back(new SPProperty(this, SPPropIntValue,        SPExposureAuto, "ExposureAuto"));
    propertyList_.push_back(new SPProperty(this, ADGain,                             0, "Gain"));
    propertyList_.push_back(new SPProperty(this, SPPropFloatValue,              SPGain, "Gain"));
    propertyList_.push_back(new SPProperty(this, SPPropIntValue,            SPGainAuto, "GainAuto"));
    propertyList_.push_back(new SPProperty(this, SPPropFloatValue,         SPFrameRate, "AcquisitionFrameRate")); 
    propertyList_.push_back(new SPProperty(this, SPPropIntValue,       SPFrameRateAuto, "AcquisitionFrameRateAuto"));
    gcstring tempString = "AcquisitionFrameRateEnable";
    CNodePtr pBase = (CNodePtr)pNodeMap_->GetNode(tempString);
    if (!IsAvailable(pBase)) {
        tempString = "AcquisitionFrameRateEnabled";
        pBase = (CNodePtr)pNodeMap_->GetNode(tempString);
        if (!IsAvailable(pBase)) {
            printf("Error: neither AcquisitionFrameRateEnable nor AcquisitionFrameRateEnabled exist\n");
        }
    }
    propertyList_.push_back(new SPProperty(this, SPPropIntValue,       SPFrameRateEnable, tempString));
    propertyList_.push_back(new SPProperty(this, SPPropFloatValue,        SPTriggerDelay, "TriggerDelay"));
    propertyList_.push_back(new SPProperty(this, ADTemperatureActual,                  0, "DeviceTemperature"));
    propertyList_.push_back(new SPProperty(this, SPPropFloatValue,          SPBlackLevel, "BlackLevel"));
    propertyList_.push_back(new SPProperty(this, SPPropIntValue,        SPBlackLevelAuto, "BlackLevelAuto"));
    propertyList_.push_back(new SPProperty(this, SPPropIntValue, SPBlackLevelBalanceAuto, "BlackLevelBalanceAuto"));
    propertyList_.push_back(new SPProperty(this, SPPropFloatValue,        SPBalanceRatio, "BalanceRatio"));
    propertyList_.push_back(new SPProperty(this, SPPropIntValue,  SPBalanceRatioSelector, "BalanceRatioSelector"));
    propertyList_.push_back(new SPProperty(this, SPPropIntValue,      SPBalanceWhiteAuto, "BalanceWhiteAuto"));
    propertyList_.push_back(new SPProperty(this, SPPropFloatValue,          SPSaturation, "Saturation"));
    propertyList_.push_back(new SPProperty(this, SPPropIntValue,      SPSaturationEnable, "SaturationEnable"));
    propertyList_.push_back(new SPProperty(this, SPPropFloatValue,               SPGamma, "Gamma"));
    propertyList_.push_back(new SPProperty(this, SPPropIntValue,           SPGammaEnable, "GammaEnable"));
    propertyList_.push_back(new SPProperty(this, SPPropFloatValue,          SPSharpening, "Sharpening"));
    propertyList_.push_back(new SPProperty(this, SPPropIntValue,        SPSharpeningAuto, "SharpeningAuto"));
    propertyList_.push_back(new SPProperty(this, SPPropIntValue,      SPSharpeningEnable, "SharpeningEnable"));

    // Enum properties
    propertyList_.push_back(new SPProperty(this, SPPropIntValue,         SPPixelFormat, "PixelFormat"));
    propertyList_.push_back(new SPProperty(this, SPPropIntValue,  SPConvertPixelFormat, "ConvertPixelFormat"));
    propertyList_.push_back(new SPProperty(this, SPPropIntValue,           SPVideoMode, "VideoMode"));
    propertyList_.push_back(new SPProperty(this, ADTriggerMode,                      0, "TriggerMode"));
    propertyList_.push_back(new SPProperty(this, SPPropIntValue,       SPTriggerSource, "TriggerSource"));
    propertyList_.push_back(new SPProperty(this, SPPropIntValue,   SPTriggerActivation, "TriggerActivation"));

    updateSPProperties();
    report(stdout, 1);

		epicsInt32 iValue;
		
		getSPProperty(ADSerialNumber);
 		getSPProperty(ADFirmwareVersion);
 		getSPProperty(ADManufacturer);
 		getSPProperty(ADModel);
 		getSPProperty(ADMaxSizeX, 0, &iValue);
		setIntegerParam(ADSizeX, iValue);
 		getSPProperty(ADMaxSizeY, 0, &iValue);
		setIntegerParam(ADSizeY, iValue);

    // Create the message queue to pass images from the callback class
    pCallbackMsgQ_ = new epicsMessageQueue(CALLBACK_MESSAGE_QUEUE_SIZE, sizeof(ImagePtr));
    if (!pCallbackMsgQ_) {
        cantProceed("ADSpinnaker::ADSpinnaker epicsMessageQueueCreate failure\n");
    }

		pImageEventHandler_ = new ImageEventHandler(pCallbackMsgQ_);
		pCamera_->RegisterEvent(*pImageEventHandler_);

    startEventId_ = epicsEventCreate(epicsEventEmpty);

    // launch image read task
    epicsThreadCreate("PointGreyImageTask", 
                      epicsThreadPriorityMedium,
                      epicsThreadGetStackSize(epicsThreadStackMedium),
                      imageGrabTaskC, this);

    // shutdown on exit
    epicsAtExit(c_shutdown, this);

    return;
}


void ADSpinnaker::shutdown(void)
{
    static const char *functionName = "shutdown";
    exiting_ = 1;
    
    lock();
    printf("Shutting down, calling camList.Clear() and system.ReleaseInstance()\n");
    try {
    		pCamera_->UnregisterEvent(*pImageEventHandler_);
    		delete pImageEventHandler_;
    		pCamera_->DeInit();
        camList_.Clear();
        system_->ReleaseInstance();
    }
    catch (Spinnaker::Exception &e)
    {
    	asynPrint(pasynUserSelf, ASYN_TRACE_ERROR, 
    	    "%s::%s exception %s\n",
    	    driverName, functionName, e.what());
    }
    unlock();
}

asynStatus ADSpinnaker::connectCamera(void)
{
    unsigned int numCameras;
    char tempString[100];
    static const char *functionName = "connectCamera";

    try {
        // Retrieve list of cameras from the system
        camList_ = system_->GetCameras();
    
        numCameras = camList_.GetSize();
    
        asynPrint(pasynUserSelf, ASYN_TRACE_WARNING,
            "%s::%s called camList_.GetSize, camList_=%p, numCameras=%d\n",
            driverName, functionName, &camList_, numCameras);
        
        if (numCameras <= 0) {
            asynPrint(pasynUserSelf, ASYN_TRACE_ERROR, 
                "%s:%s: no cameras found\n",
                driverName, functionName);
     
            // Clear camera list before releasing system
            camList_.Clear();
        
            return asynError;
        }
    
        if (cameraId_ < 1000) {
            asynPrint(pasynUserSelf, ASYN_TRACE_WARNING,
                "%s::%s calling camList_.GetByIndex, camList_=%p\n",
                driverName, functionName, &camList_);
            pCamera_ = camList_.GetByIndex(cameraId_);
        } else { 
            asynPrint(pasynUserSelf, ASYN_TRACE_WARNING,
                "%s::%s calling camList_.GetBySerial, camList_=%p, cameraId_=%d\n",
                driverName, functionName, &camList_, cameraId_);
            char tempString[100];
            sprintf(tempString, "%d", cameraId_);
            std::string tempStdString(tempString);
            pCamera_ = camList_.GetBySerial(tempStdString);
        }
    
//    		report(stdout, 1);
    
    		// Initialize camera
    		pCamera_->Init();
    		
    		// Retrieve GenICam nodemap
    		pNodeMap_ = &pCamera_->GetNodeMap();
    }

    catch (Spinnaker::Exception &e)
    {
    	asynPrint(pasynUserSelf, ASYN_TRACE_ERROR, 
    	    "%s::%s exception %s\n",
    	    driverName, functionName, e.what());
    	return asynError;
    }

    epicsSnprintf(tempString, sizeof(tempString), "%d.%d.%d", 
                  DRIVER_VERSION, DRIVER_REVISION, DRIVER_MODIFICATION);
    setStringParam(NDDriverVersion,tempString);
 
/*   
    Utilities::GetLibraryVersion(&version);
    epicsSnprintf(tempString, sizeof(tempString), "%d.%d.%d", version.major, version.minor, version.type);
    asynPrint(pasynUserSelf, ASYN_TRACE_WARNING,
        "%s::%s called Utilities::GetLibraryVersion, version=%s\n",
        driverName, functionName, tempString);
    setStringParam(ADSDKVersion, tempString);


    // Get and set the embedded image info
    asynPrint(pasynUserSelf, ASYN_TRACE_WARNING,
        "%s::%s calling CameraBase::GetEmbeddedImageInfo, &embeddedInfo=%p\n",
        driverName, functionName, &embeddedInfo);
    error = pCameraBase_->GetEmbeddedImageInfo(&embeddedInfo);
    if (checkError(error, functionName, "GetEmbeddedImageInfo")) return asynError;
    // Force the timestamp and frame counter information to be on
    embeddedInfo.timestamp.onOff = true;
    embeddedInfo.frameCounter.onOff = true;
    asynPrint(pasynUserSelf, ASYN_TRACE_WARNING,
        "%s::%s calling CameraBase::SetEmbeddedImageInfo, &embeddedInfo=%p\n",
        driverName, functionName, &embeddedInfo);
    error = pCameraBase_->SetEmbeddedImageInfo(&embeddedInfo);
    if (checkError(error, functionName, "SetEmbeddedImageInfo")) return asynError;
*/    
    return asynSuccess;
}


/** Task to grab images off the camera and send them up to areaDetector
 *
 */

void ADSpinnaker::imageGrabTask()
{
    asynStatus status = asynSuccess;
    int imageCounter;
    int numImages, numImagesCounter;
    int imageMode;
    int arrayCallbacks;
    epicsTimeStamp startTime;
    int acquire;
    static const char *functionName = "imageGrabTask";

    lock();

    while (1) {
        // Is acquisition active? 
        getIntegerParam(ADAcquire, &acquire);
        // If we are not acquiring then wait for a semaphore that is given when acquisition is started 
        if (!acquire) {
            setIntegerParam(ADStatus, ADStatusIdle);
            callParamCallbacks();

            // Wait for a signal that tells this thread that the transmission
            // has started and we can start asking for image buffers...
            asynPrint(pasynUserSelf, ASYN_TRACE_FLOW,
                "%s::%s waiting for acquire to start\n", 
                driverName, functionName);
            // Release the lock while we wait for an event that says acquire has started, then lock again
            unlock();
            epicsEventWait(startEventId_);
            lock();
            asynPrint(pasynUserSelf, ASYN_TRACE_FLOW,
                "%s::%s started!\n", 
                driverName, functionName);
            setIntegerParam(ADNumImagesCounter, 0);
            setIntegerParam(ADAcquire, 1);
        }

        // Get the current time 
        epicsTimeGetCurrent(&startTime);
        // We are now waiting for an image
        setIntegerParam(ADStatus, ADStatusWaiting);
        // Call the callbacks to update any changes
        callParamCallbacks();

        status = grabImage();
        if (status == asynError) {
            // remember to release the NDArray back to the pool now
            // that we are not using it (we didn't get an image...)
            if (pRaw_) pRaw_->release();
            pRaw_ = NULL;
            continue;
        }

        getIntegerParam(NDArrayCounter, &imageCounter);
        getIntegerParam(ADNumImages, &numImages);
        getIntegerParam(ADNumImagesCounter, &numImagesCounter);
        getIntegerParam(ADImageMode, &imageMode);
        getIntegerParam(NDArrayCallbacks, &arrayCallbacks);
        imageCounter++;
        numImagesCounter++;
        setIntegerParam(NDArrayCounter, imageCounter);
        setIntegerParam(ADNumImagesCounter, numImagesCounter);

        if (arrayCallbacks) {
            // Call the NDArray callback
            doCallbacksGenericPointer(pRaw_, NDArrayData, 0);
        }
        // Release the NDArray buffer now that we are done with it.
        // After the callback just above we don't need it anymore
        pRaw_->release();
        pRaw_ = NULL;

        // See if acquisition is done if we are in single or multiple mode
        if ((imageMode == ADImageSingle) || ((imageMode == ADImageMultiple) && (numImagesCounter >= numImages))) {
            setIntegerParam(ADStatus, ADStatusIdle);
            status = stopCapture();
        }
        callParamCallbacks();
    }
}

asynStatus ADSpinnaker::grabImage()
{
    asynStatus status = asynSuccess;
    size_t nRows, nCols;
//    size_t stride;
    NDDataType_t dataType;
    NDColorMode_t colorMode;
    int convertPixelFormat;
    int numColors;
    size_t dims[3];
    ImageStatus imageStatus;
    PixelFormatEnums pixelFormat;
    int pixelSize;
    size_t dataSize, dataSizePG;
//    double bandwidth;
//    double frameRate;
    void *pData;
    int nDims;
//    long long timeStamp;
//    int timeStampMode;
    static const char *functionName = "grabImage";

  try {
    unlock();
    if (pCallbackMsgQ_->receive(&pImage_, sizeof(pImage_)) != sizeof(pImage_)) {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s::%s error receiving from message queue\n",
                driverName, functionName);
        return asynError;
    }
    lock();
    imageStatus = pImage_->GetImageStatus();
    if (imageStatus != IMAGE_NO_ERROR) {
        asynPrint(pasynUserSelf, ASYN_TRACE_WARNING,
            "%s::%s error GetImageStatus returned %d\n",
            driverName, functionName, imageStatus);
        pImage_->Release();
        return asynError;
    } 
    nCols = pImage_->GetWidth();
    nRows = pImage_->GetHeight();
    //stride = pImage_->GetStride();
    pixelFormat = pImage_->GetPixelFormat();
    gcstring pixelFormatName = pImage_->GetPixelFormatName();
 
//    timeStamp = pImage_->GetTimeStamp();    
//    pPGImage = pPGRawImage_;
    // Calculate bandwidth
//    dataSizePG = pPGRawImage_->GetReceivedDataSize();
//    getDoubleParam(FRAME_RATE, PGPropertyValueAbs, &frameRate);
//    bandwidth = frameRate * dataSizePG / (1024 * 1024);
//    setDoubleParam(PGBandwidth, bandwidth);

    // Convert the pixel format if requested
    getIntegerParam(SPConvertPixelFormat, SPPropIntValue, &convertPixelFormat);
    if (convertPixelFormat != SPPixelConvertNone) {
        PixelFormatEnums convertedFormat;
        switch (convertPixelFormat) {
            case SPPixelConvertMono8:
                convertedFormat = PixelFormat_Mono8;
                break;
            case SPPixelConvertMono16:
                convertedFormat = PixelFormat_Mono16;
                break;
            case SPPixelConvertRaw16:
                convertedFormat = PixelFormat_Raw16;
                break;
            case SPPixelConvertRGB8:
                convertedFormat = PixelFormat_RGB8;
                break;
            case SPPixelConvertRGB16:
                convertedFormat = PixelFormat_RGB16;
                break;
            default:
                asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                    "%s::%s Error: Unknown pixel conversion format %d\n",
                    driverName, functionName, convertPixelFormat);
                convertedFormat = PixelFormat_Mono8;
                break;
        }

printf("Converting image to format 0x%x\n", convertedFormat);
        try {
            ImagePtr pConvertedImage = pImage_->Convert(convertedFormat);
            pImage_->Release();
            pImage_ = pConvertedImage;
        }
        catch (Spinnaker::Exception &e) {
    	       asynPrint(pasynUserSelf, ASYN_TRACE_ERROR, 
    	           "%s::%s exception %s\n",
    	       driverName, functionName, e.what());
        }
    }

    pixelFormat = pImage_->GetPixelFormat();
printf("After conversion image format=0x%x\n", pixelFormat);
    pixelFormatName = pImage_->GetPixelFormatName();
    switch (pixelFormat) {
        case PixelFormat_Mono8:
        case PixelFormat_Raw8:
            dataType = NDUInt8;
            colorMode = NDColorModeMono;
            numColors = 1;
            pixelSize = 1;
            break;

        case PixelFormat_RGB8:
            dataType = NDUInt8;
            colorMode = NDColorModeRGB1;
            numColors = 3;
            pixelSize = 1;
            break;

        case PixelFormat_Mono16:
        case PixelFormat_Raw16:
            dataType = NDUInt16;
            colorMode = NDColorModeMono;
            numColors = 1;
            pixelSize = 2;
            break;

        case PixelFormat_RGB16:
            dataType = NDUInt16;
            colorMode = NDColorModeRGB1;
            numColors = 3;
            pixelSize = 2;
            break;

        default:
            asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s:%s: unsupported pixel format=0x%x\n",
                driverName, functionName, pixelFormat);
            return asynError;
    }

    if (numColors == 1) {
        nDims = 2;
        dims[0] = nCols;
        dims[1] = nRows;
    } else {
        nDims = 3;
        dims[0] = 3;
        dims[1] = nCols;
        dims[2] = nRows;
    }
    dataSize = dims[0] * dims[1] * pixelSize;
    if (nDims == 3) dataSize *= dims[2];
    dataSizePG = pImage_->GetBufferSize();
    // Note, we should be testing for equality here.  However, there appears to be a bug in the
    // SDK when images are converted.  When converting from raw8 to mono8, for example, the
    // size returned by GetDataSize is the size of an RGB8 image, not a mono8 image.
    if (dataSize > dataSizePG) {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
            "%s:%s: data size mismatch: calculated=%lu, reported=%lu\n",
            driverName, functionName, (long)dataSize, (long)dataSizePG);
        //return asynError;
    }
    setIntegerParam(NDArraySizeX, (int)nCols);
    setIntegerParam(NDArraySizeY, (int)nRows);
    setIntegerParam(NDArraySize, (int)dataSize);
    setIntegerParam(NDDataType,dataType);
    if (nDims == 3) {
        colorMode = NDColorModeRGB1;
    } else {
        // If the color mode is currently set to Bayer leave it alone
        getIntegerParam(NDColorMode, (int *)&colorMode);
        if (colorMode != NDColorModeBayer) colorMode = NDColorModeMono;
    }
    setIntegerParam(NDColorMode, colorMode);

    pRaw_ = pNDArrayPool->alloc(nDims, dims, dataType, 0, NULL);
    if (!pRaw_) {
        // If we didn't get a valid buffer from the NDArrayPool we must abort
        // the acquisition as we have nowhere to dump the data...
        setIntegerParam(ADStatus, ADStatusAborting);
        callParamCallbacks();
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR, 
            "%s::%s [%s] ERROR: Serious problem: not enough buffers left! Aborting acquisition!\n",
            driverName, functionName, portName);
        setIntegerParam(ADAcquire, 0);
        return(asynError);
    }
    pData = pImage_->GetData();
    if (pData) {
        memcpy(pRaw_->pData, pData, dataSize);
    } else {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR, 
            "%s::%s [%s] ERROR: pData is NULL!\n",
            driverName, functionName, portName);
        return asynError;
    }
    pImage_->Release();

    // Put the frame number into the buffer
    pRaw_->uniqueId = (int)pImage_->GetFrameID();
//    getIntegerParam(PGTimeStampMode, &timeStampMode);
    updateTimeStamp(&pRaw_->epicsTS);
    // Set the timestamps in the buffer
//    switch (timeStampMode) {
//        case TimeStampCamera:
//             pRaw_->timeStamp = timeStamp / 1e9;
//             break;
//        case TimeStampEPICS:
            pRaw_->timeStamp = pRaw_->epicsTS.secPastEpoch + pRaw_->epicsTS.nsec/1e9;
//            break;
//        case TimeStampHybrid:
//            // For now we just use EPICS time
//            pRaw_->timeStamp = pRaw_->epicsTS.secPastEpoch + pRaw_->epicsTS.nsec/1e9;
//            break;
//   }
    
    // Get any attributes that have been defined for this driver        
    getAttributes(pRaw_->pAttributeList);
    
    // Change the status to be readout...
    setIntegerParam(ADStatus, ADStatusReadout);
    callParamCallbacks();

    pRaw_->pAttributeList->add("ColorMode", "Color mode", NDAttrInt32, &colorMode);
    return status;
  }
    catch (Spinnaker::Exception &e)
    {
    	asynPrint(pasynUserSelf, ASYN_TRACE_ERROR, 
    	    "%s::%s exception %s\n",
    	    driverName, functionName, e.what());
    	return asynError;
    }
}

ADSpinnaker::SPProperty* ADSpinnaker::findProperty(int asynParam, int asynAddr)
{
    static const char *functionName = "findProperty";
    SPProperty *pProperty;
    std::vector<SPProperty*>::iterator it;
    for (it=propertyList_.begin(); it<propertyList_.end(); it++) {
        pProperty = *it;
        if ((asynParam == pProperty->asynParam) && (asynAddr == pProperty->asynAddr)) {
            return pProperty;
        }
    }
    asynPrint(pasynUserSelf, ASYN_TRACE_ERROR, 
        "%s::%s cannot find property with param=%d, addr=%d\n",
        driverName, functionName, asynParam, asynAddr);
    return 0;
}

asynStatus ADSpinnaker::getSPProperty(int asynParam, int asynAddr, void *pValue, bool setParam)
{
    SPProperty* pElement = findProperty(asynParam, asynAddr);
    return pElement->getValue(pValue, setParam);
}

asynStatus ADSpinnaker::setSPProperty(int asynParam, int asynAddr, void *pValue, void *pReadbackValue, bool setParam)
{
    SPProperty* pElement = findProperty(asynParam, asynAddr);
    return pElement->setValue(pValue, pReadbackValue, setParam);
    return pElement->setValue(pValue, pReadbackValue, setParam);
}

asynStatus ADSpinnaker::updateSPProperties()
{
    //static const char *functionName = "updateSPProperties";
    SPProperty *pProperty;
    std::vector<SPProperty*>::iterator it;
    for (it=propertyList_.begin(); it<propertyList_.end(); it++) {
        pProperty = *it;
        pProperty->update();
    }
    return asynSuccess;
}

/** Sets an int32 parameter.
  * \param[in] pasynUser asynUser structure that contains the function code in pasynUser->reason. 
  * \param[in] value The value for this parameter 
  *
  * Takes action if the function code requires it.  ADAcquire, ADSizeX, and many other
  * function codes make calls to the Firewire library from this function. */

asynStatus ADSpinnaker::writeInt32( asynUser *pasynUser, epicsInt32 value)
{
    asynStatus status = asynSuccess;
    int function = pasynUser->reason;
    int addr;
    static const char *functionName = "writeInt32";

    pasynManager->getAddr(pasynUser, &addr);
    if (addr < 0) addr=0;

    // Set the value in the parameter library.  This may change later but that's OK
    status = setIntegerParam(addr, function, value);

    if (function == ADAcquire) {
        if (value) {
            // start acquisition
            status = startCapture();
        } else {
            status = stopCapture();
        }

    } else if ( (function == ADSizeX)       ||
                (function == ADSizeY)       ||
                (function == ADMinX)        ||
                (function == ADMinY)        ||
                (function == ADBinX)        ||
                (function == ADBinY)        ||
                (function == ADImageMode)   ||
                (function == ADNumImages)   ||
                (function == NDDataType)) {    
        status = setImageParams();
    } else if (function == ADTriggerMode) {
        status = setSPProperty(function, addr, &value);
    } else if (function == SPPropIntValue) {
        status = setSPProperty(function, addr, &value);
    } else if (function == ADReadStatus) {
        status = readStatus();
    } else {
        // If this parameter belongs to a base class call its method
        if (function < FIRST_SP_PARAM) status = ADDriver::writeInt32(pasynUser, value);
    }
    
    // When some parameters are changed they can cause others to change limits, enum choices, etc.
    if ((function==SPPropIntValue && addr==SPFrameRateAuto) ||
        (function==SPPropIntValue && addr==SPGainAuto) ||
        (function==SPPropIntValue && addr==SPVideoMode) ||
        (function == ADTriggerMode) ||
        (function==SPPropIntValue && addr==SPTriggerSource)) {
        updateSPProperties();
    }

    asynPrint(pasynUserSelf, ASYN_TRACEIO_DRIVER, 
        "%s::%s function=%d, addr=%d, value=%d, status=%d\n",
        driverName, functionName, function, addr, value, status);
            
    callParamCallbacks(addr);
    return status;
}

/** Sets an float64 parameter.
  * \param[in] pasynUser asynUser structure that contains the function code in pasynUser->reason. 
  * \param[in] value The value for this parameter 
  *
  * Takes action if the function code requires it.  The PGPropertyValueAbs
  * function code makes calls to the Firewire library from this function. */

asynStatus ADSpinnaker::writeFloat64( asynUser *pasynUser, epicsFloat64 value)
{
    asynStatus status = asynSuccess;
    int function = pasynUser->reason;
    epicsFloat64 readbackValue;
    int addr;
    static const char *functionName = "writeFloat64";
    
    pasynManager->getAddr(pasynUser, &addr);
    if (addr < 0) addr=0;

    // Set the value in the parameter library.  This may change later but that's OK
    status = setDoubleParam(addr, function, value);

    if (function == ADAcquireTime) {
        // Camera units are microseconds
        double tempValue = value * 1.e6;
        status = setSPProperty(ADAcquireTime, 0, &tempValue, &readbackValue, false);
        setDoubleParam(ADAcquireTime, readbackValue/1.e6);
    
    } else if (function == ADAcquirePeriod) {
        double tempValue = 1./value;
        status = setSPProperty(SPPropFloatValue, SPFrameRate, &tempValue, &readbackValue);
        setDoubleParam(ADAcquirePeriod, 1./readbackValue);

    } else if (function == ADGain) {
        status = setSPProperty(function, addr, &value);

    } else if (function == SPPropFloatValue) {
        switch (addr) {
            case SPTriggerDelay: {
                // Camera units are microseconds
                double tempValue = value * 1.e6;
                status = setSPProperty(function, SPTriggerDelay, &tempValue, &readbackValue, false);
                setDoubleParam(SPTriggerDelay, SPPropFloatValue, readbackValue/1.e6);
                break; }
            case SPFrameRate:    
                status = setSPProperty(function, SPFrameRate, &value, &readbackValue);
                setDoubleParam(ADAcquirePeriod, 1./readbackValue);
                break;
            default:
                status = setSPProperty(function, addr, &value);
                break;
        }
    } else {
        // If this parameter belongs to a base class call its method
        if (function < FIRST_SP_PARAM) status = ADDriver::writeFloat64(pasynUser, value);
    }

    asynPrint(pasynUser, ASYN_TRACEIO_DRIVER, 
        "%s::%s function=%d, addr=%d, value=%f, status=%d\n",
        driverName, functionName, function, addr, value, status);
    callParamCallbacks(addr);
    return status;
}

asynStatus ADSpinnaker::readEnum(asynUser *pasynUser, char *strings[], int values[], int severities[], 
                               size_t nElements, size_t *nIn)
{
    int function = pasynUser->reason;
    int numEnums;
    int i;
    long long entryValue;
    int addr;
    static const char *functionName = "readEnum";

    pasynManager->getAddr(pasynUser, &addr);
    if (addr < 0) addr=0;
    SPProperty *pElement = findProperty(function, addr);

    if (pElement == 0) {
        return asynError;
    }
    if ((pElement->propertyType != SPPropertyTypeEnum) && 
        (pElement->propertyType != SPPropertyTypeUnknown)) {
        return asynError;
    }
    // There are a few enums we don't want to autogenerate the values
    if ((function == ADImageMode) ||
        (addr == SPConvertPixelFormat)) {
        return asynError;
    }
    const char *nodeName = pElement->nodeName;

    *nIn = 0;
    
    try {
        CEnumerationPtr pNode = pNodeMap_->GetNode(nodeName);
        if (!IsAvailable(pNode) || !IsWritable(pNode)) {
            if (strings[0]) free(strings[0]);
            strings[0] = epicsStrDup("N.A.");
            values[0] = 0;
            *nIn = 1;
            return asynSuccess;
        }
        NodeList_t entries;
        pNode->GetEntries(entries);
        numEnums = (int)entries.size();

        for (i=0; ((i<numEnums) && (i<(int)nElements)); i++) {
            IEnumEntry *pEntry= dynamic_cast<IEnumEntry *>(entries[i]);
            const char *pString = epicsStrDup((pEntry->GetSymbolic()).c_str());
            if (IsAvailable(pEntry) && IsReadable(pEntry)) {
                if (strings[*nIn]) free(strings[*nIn]);
                strings[*nIn] = epicsStrDup(pString);
                entryValue = pEntry->GetValue();
                values[*nIn] = (int)entryValue;
                severities[*nIn] = 0;
                (*nIn)++;
            } else {
            }
        }
    }

    catch (Spinnaker::Exception &e)
    {
    	asynPrint(pasynUserSelf, ASYN_TRACE_ERROR, 
    	    "%s::%s node %s exception %s\n",
    	    driverName, functionName, nodeName, e.what());
    	return asynError;
    }
    return asynSuccess;   
}


asynStatus ADSpinnaker::setImageParams()
{
    //static const char *functionName = "setImageParams";
    
    //bool resumeAcquire;
    int imageMode;
    AcquisitionModeEnums acquisitionMode;

    if (!pCamera_) return asynError;

    // We translate between the areaDetector ImageMode and the Spinnaker AcquisitionMode
    getIntegerParam(ADImageMode, &imageMode);
    switch (imageMode) {
        case ADImageSingle:
            acquisitionMode = AcquisitionMode_SingleFrame;
            break;
        case ADImageMultiple:
            acquisitionMode = AcquisitionMode_MultiFrame;
            break;
        case ADImageContinuous:
            acquisitionMode = AcquisitionMode_Continuous;
            break;
          
    }
    
    setSPProperty(SPPropIntValue, SPVideoMode);
    setSPProperty(ADImageMode, 0, &acquisitionMode);
    setSPProperty(ADNumImages);
    setSPProperty(ADSizeX);
    setSPProperty(ADSizeY);
    setSPProperty(ADMinX);
    setSPProperty(ADMinY);
    setSPProperty(ADBinX);
    setSPProperty(ADBinY);

    // We read these back after setting all of them in case one setting affects another
    getSPProperty(SPPropIntValue, SPVideoMode);
    getSPProperty(ADImageMode, 0, &acquisitionMode);
    getSPProperty(ADNumImages);
    getSPProperty(ADSizeX);
    getSPProperty(ADSizeY);
    getSPProperty(ADMinX);
    getSPProperty(ADMinY);
    getSPProperty(ADBinX);
    getSPProperty(ADBinY);

    switch (acquisitionMode) {
        case AcquisitionMode_SingleFrame:
            imageMode = ADImageSingle;
            break;
        case AcquisitionMode_MultiFrame:
            imageMode = ADImageMultiple;
            break;
        case AcquisitionMode_Continuous:
            imageMode = ADImageContinuous;
            break;
        default:
            break;         
    }
    setIntegerParam(ADImageMode, imageMode);

    return asynSuccess;
}


asynStatus ADSpinnaker::startCapture()
{
    static const char *functionName = "startCapture";

    // Start the camera transmission...
    setIntegerParam(ADNumImagesCounter, 0);
    setShutter(1);
printf("%s::%s calling BeginAcquisition()\n", driverName, functionName);
    try {
        pCamera_->BeginAcquisition();
        epicsEventSignal(startEventId_);
    }
    catch (Spinnaker::Exception &e)
    {
    	asynPrint(pasynUserSelf, ASYN_TRACE_ERROR, 
    	    "%s::%s exception %s\n",
    	    driverName, functionName, e.what());
    	return asynError;
    }
    return asynSuccess;
}


asynStatus ADSpinnaker::stopCapture()
{
    int status;
    static const char *functionName = "stopCapture";

    setIntegerParam(ADAcquire, 0);
    setShutter(0);
    // Need to wait for the task to set the status to idle
    while (1) {
        getIntegerParam(ADStatus, &status);
        if (status == ADStatusIdle) break;
printf("Waiting for ADStatusIdle\n");
        unlock();
        epicsThreadSleep(.1);
        lock();
    }
    printf("%s::%s calling EndAcquisition()\n", driverName, functionName);
    try {
        pCamera_->EndAcquisition();
    }
    catch (Spinnaker::Exception &e)
    {
    	asynPrint(pasynUserSelf, ASYN_TRACE_ERROR, 
    	    "%s::%s exception %s\n",
    	    driverName, functionName, e.what());
    	return asynError;
    }
    return asynSuccess;
}


asynStatus ADSpinnaker::readStatus()
{
    //static const char *functionName = "readStatus";

    getSPProperty(ADTemperatureActual);
 
 /*
    error = pCameraBase_->GetStats(pCameraStats_);
    if (checkError(error, functionName, "GetStats")) 
        return asynError;
    asynPrint(pasynUserSelf, ASYN_TRACE_WARNING,
        "%s::%s calling CameraBase::GetStats, pCameraBase_=%p, pCameraStats_=%p, pCameraStats_->temperature=%d\n",
        driverName, functionName, pCameraBase_, pCameraStats_, pCameraStats_->temperature);
    setIntegerParam(PGCorruptFrames,    pCameraStats_->imageCorrupt);
    setIntegerParam(PGDriverDropped,    pCameraStats_->imageDriverDropped);
    if (pCameraStats_->imageXmitFailed == 0x80000000) pCameraStats_->imageXmitFailed = 0;
    setIntegerParam(PGTransmitFailed,   pCameraStats_->imageXmitFailed);
    setIntegerParam(PGDroppedFrames,    pCameraStats_->imageDropped);
*/    
    callParamCallbacks();
    return asynSuccess;
}

// This helper function deals with output indentation, of which there is a lot.
void indent(FILE *fp, unsigned int level)
{
	for (unsigned int i=0; i<level; i++) {
		fprintf(fp, "   ");
	}
}

void ADSpinnaker::reportNode(FILE *fp, INodeMap *pNodeMap, const char *nodeName, int level)
{
    static const char *functionName = "reportStringNode";
    
    try {
        CNodePtr pBase = (CNodePtr)pNodeMap->GetNode(nodeName);
        if (IsAvailable(pBase) && IsReadable(pBase)) {
            gcstring value;
        		gcstring displayName = pBase->GetDisplayName();
    				switch (pBase->GetPrincipalInterfaceType()) {
    				case intfIString: {
            		CStringPtr pNode = static_cast<CStringPtr>(pBase);
            		value = pNode->GetValue();
     					  break;
                }
    				case  intfIInteger: {
            		CIntegerPtr pNode = static_cast<CIntegerPtr>(pBase);
            		value = pNode->ToString();
    					  break; 
    					  }
    
    				case intfIFloat: {
            		CFloatPtr pNode = static_cast<CFloatPtr>(pBase);
            		value = pNode->ToString();
    					  break;
    					  }
    				case intfIBoolean: {
            		CBooleanPtr pNode = static_cast<CBooleanPtr>(pBase);
            		value = pNode->ToString();
    					  break;
                }
    				case intfICommand: {
            		CCommandPtr pNode = static_cast<CCommandPtr>(pBase);
		            value = pNode->GetToolTip();
    					  break;
                }
    				case intfIEnumeration: {
            		CEnumerationPtr pNode = static_cast<CEnumerationPtr>(pBase);
    		        CEnumEntryPtr pEntry = pNode->GetCurrentEntry();
            		value = pEntry->GetSymbolic();
    					  break;
    					 }
    				default:
    				   break; 
    				}
         		indent(fp, level);
        		fprintf(fp, "%s (%s):%s\n", displayName.c_str(), nodeName, value.c_str());
        }
    } 
    catch (Spinnaker::Exception &e) {
    	asynPrint(pasynUserSelf, ASYN_TRACE_ERROR, 
    	    "%s::%s node %s exception %s\n",
    	    driverName, functionName, nodeName, e.what());
    }
}


/** Print out a report; calls ADDriver::report to get base class report as well.
  * \param[in] fp File pointer to write output to
  * \param[in] details Level of detail desired.  If >1 prints information about 
               supported video formats and modes, etc.
 */

void ADSpinnaker::report(FILE *fp, int details)
{
    int numCameras;
//    int mode, rate;
//    int source;
    int i;
//    asynStatus status;
//    bool supported;
//    int property;
//    int pixelFormatIndex;
    static const char *functionName = "report";

    try {    
        numCameras = camList_.GetSize();
        fprintf(fp, "\nNumber of cameras detected: %d\n", numCameras);
        if (details <1) return;
        for (i=0; i<numCameras; i++) {
            CameraPtr pCamera;
            pCamera = camList_.GetByIndex(i);
    		    INodeMap *pNodeMap = &pCamera->GetTLDeviceNodeMap();
    
            fprintf(fp, "Camera %d\n", i);
            reportNode(fp, pNodeMap, "DeviceVendorName", 1);
            reportNode(fp, pNodeMap, "DeviceModelName", 1);
            reportNode(fp, pNodeMap, "DeviceSerialNumber", 1);
            reportNode(fp, pNodeMap, "DeviceVersion", 1);
            reportNode(fp, pNodeMap, "DeviceType", 1);
        }
        // Print out propertyList_
        SPProperty *pProperty;
        std::vector<SPProperty*>::iterator p;
        fprintf(fp, "Property list\n");
        for (p=propertyList_.begin(); p<propertyList_.end(); p++) {
            pProperty = *p;
            fprintf(fp, "\n  Node name: %s\n", pProperty->nodeName);
            fprintf(fp, "  asynParam, asynAddr: %d, %d\n", pProperty->asynParam, pProperty->asynAddr);
            fprintf(fp, "  isImplemented: %d\n", pProperty->isImplemented);
        }
    }
    catch (Spinnaker::Exception &e)
    {
    	asynPrint(pasynUserSelf, ASYN_TRACE_ERROR, 
    	    "%s::%s exception %s\n",
    	    driverName, functionName, e.what());
    }
    
/*    
    if (pCamera_) {

    if (details < 1) goto done;
    
    if (pCamera_) {
        VideoMode videoMode;
        FrameRate frameRate;

*/          
    ADDriver::report(fp, details);
    return;
}

static const iocshArg configArg0 = {"Port name", iocshArgString};
static const iocshArg configArg1 = {"cameraId", iocshArgInt};
static const iocshArg configArg2 = {"traceMask", iocshArgInt};
static const iocshArg configArg3 = {"memoryChannel", iocshArgInt};
static const iocshArg configArg4 = {"maxBuffers", iocshArgInt};
static const iocshArg configArg5 = {"maxMemory", iocshArgInt};
static const iocshArg configArg6 = {"priority", iocshArgInt};
static const iocshArg configArg7 = {"stackSize", iocshArgInt};
static const iocshArg * const configArgs[] = {&configArg0,
                                              &configArg1,
                                              &configArg2,
                                              &configArg3,
                                              &configArg4,
                                              &configArg5,
                                              &configArg6,
                                              &configArg7};
static const iocshFuncDef configADSpinnaker = {"ADSpinnakerConfig", 8, configArgs};
static void configCallFunc(const iocshArgBuf *args)
{
    ADSpinnakerConfig(args[0].sval, args[1].ival, args[2].ival, 
                    args[3].ival, args[4].ival, args[5].ival,
                    args[6].ival, args[7].ival);
}


static void ADSpinnakerRegister(void)
{
    iocshRegister(&configADSpinnaker, configCallFunc);
}

extern "C" {
epicsExportRegistrar(ADSpinnakerRegister);
}
