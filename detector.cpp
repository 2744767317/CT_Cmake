#include "detector.h"
#include <string.h>

#define EXTRACT_FUNC(funcPtr, flag) {m_p##funcPtr = (funcPtr)ExtractFunc(m_hModule, #flag); if(NULL==m_p##funcPtr) return Err_LoadDllFailed;}

const int CDetector::OFFSETMASK = Enm_CorrectOp_SW_PreOffset | Enm_CorrectOp_SW_PostOffset | Enm_CorrectOp_HW_PreOffset | Enm_CorrectOp_HW_PostOffset;
const int CDetector::GAINMASK = Enm_CorrectOp_SW_Gain | Enm_CorrectOp_HW_Gain;
const int CDetector::DEFECTMASK = Enm_CorrectOp_SW_Defect | Enm_CorrectOp_HW_Defect;

#if defined(linux)
bool IRayTimer::m_stoptimerFlag;
#endif
timeproc IRayTimer::timercallback=NULL;
std::map<int, CDetector*> CDetector::gDetectorMap;

int SDKAPIHelper::OpenLibrary()
{
    m_hModule = OpenDLib("FpdSys");
    if (NULL == m_hModule)
    {
        return Err_LoadDllFailed;
    }
    EXTRACT_FUNC(FnCreate, Create);
    EXTRACT_FUNC(FnDestroy, Destroy);
    EXTRACT_FUNC(FnGetAttr, GetAttr);
    EXTRACT_FUNC(FnSetAttr, SetAttr);
    EXTRACT_FUNC(FnInvoke, Invoke);
    EXTRACT_FUNC(FnAbort, Abort);
    EXTRACT_FUNC(FnSetUserCode, SetUserCode);
    EXTRACT_FUNC(FnGetErrInfo, GetErrInfo);
    EXTRACT_FUNC(FnReleaseLibraryResource, ReleaseLibraryResource);
    EXTRACT_FUNC(FnGetSDKVersion, GetSDKVersion);
    EXTRACT_FUNC(FnScanOnce, ScanOnce);
    EXTRACT_FUNC(FnScanOnceEx, ScanOnceEx);
    EXTRACT_FUNC(FnGetAuthority, GetAuthority);
    EXTRACT_FUNC(FnRegisterScanNotify, RegisterScanNotify);
    EXTRACT_FUNC(FnRegisterScanNotifyEx, RegisterScanNotifyEx)
    EXTRACT_FUNC(FnUseImageBuf, UseImageBuf);
    EXTRACT_FUNC(FnClearImageBuf, ClearImageBuf);
    EXTRACT_FUNC(FnQueryImageBuf, QueryImageBuf);
    EXTRACT_FUNC(FnGetImageFromBuf, GetImageFromBuf);
    EXTRACT_FUNC(FnOpenDefectTemplateFile, OpenDefectTemplateFile);
    EXTRACT_FUNC(FnSaveDefectTemplateFile, SaveDefectTemplateFile);
    EXTRACT_FUNC(FnCloseDefectTemplateFile, CloseDefectTemplateFile);

    return Err_OK;
}

void SDKAPIHelper::CloseLibrary()
{
	if (m_hModule)
	{
        m_pFnReleaseLibraryResource();
		CloseDLib(m_hModule);
		m_hModule = NULL;
	}
}

CDetector::CDetector(const SDKAPIHelper* pApiHelper)
: m_nDetectorID(0)
, m_pSDKApiHelper(pApiHelper)
, m_pUser(NULL)
, m_UserCallback(NULL)
{
	m_WaitAckEvent = CreateEvent(NULL, false, false, NULL);
}

CDetector::~CDetector()
{
	Destroy();
	if (m_WaitAckEvent)
	{
		CloseHandle(m_WaitAckEvent);
		m_WaitAckEvent = NULL;
	}
}

int CDetector::GetAuthority()
{
    int nAuthority = 0;
    return m_pSDKApiHelper->m_pFnGetAuthority(&nAuthority);
}

int CDetector::ScanOnce(const char* ip)
{
    CHECK_SDKLIB_STATUS;

    return m_pSDKApiHelper->m_pFnScanOnce((char*)ip);
}
int CDetector::ScanOnceEx(Enm_CommChannel eScanType, void* pInParam /*= nullptr*/)
{
    CHECK_SDKLIB_STATUS;
    return m_pSDKApiHelper->m_pFnScanOnceEx(eScanType, pInParam);
}
int CDetector::RegisterScanCB(FnNotifyScanResult scanRetCB)
{
    CHECK_SDKLIB_STATUS;
    return m_pSDKApiHelper->m_pFnRegisterScanNotify(scanRetCB);
}
int CDetector::RegisterScanExCB(FnNotifyScanResultEx scanRetCB)
{
    CHECK_SDKLIB_STATUS;
    return m_pSDKApiHelper->m_pFnRegisterScanNotifyEx(scanRetCB);
}

void CDetector::GlobalSDKCallback(int nDetectorID, int nEventID, int nEventLevel,
    const char* pszMsg, int nParam1, int nParam2, int nPtrParamLen, void* pParam)
{
    if (gDetectorMap.find(nDetectorID) == gDetectorMap.end())
        return;

    gDetectorMap[nDetectorID]->SDKCallback(nDetectorID, nEventID, nEventLevel, pszMsg, nParam1, nParam2, nPtrParamLen, pParam);
}

FPDRESULT CDetector::Create(const char* pszWorkDir, iSDKCallback* pUser)
{
    FPDRESULT nRet = m_pSDKApiHelper->m_pFnCreate(pszWorkDir, GlobalSDKCallback, &m_nDetectorID);
    if (Err_OK != nRet)
    {
        return nRet;
    }
    
    gDetectorMap[m_nDetectorID] = this;
    m_pUser = pUser;
    return nRet;
}

FPDRESULT CDetector::Create(const char* pszWorkDir, UserCallback pUserCallback)
{
    FPDRESULT nRet = m_pSDKApiHelper->m_pFnCreate(pszWorkDir, GlobalSDKCallback, &m_nDetectorID);
    if (Err_OK != nRet)
    {
        return nRet;
    }

    gDetectorMap[m_nDetectorID] = this;
    m_UserCallback = pUserCallback;
    return nRet;
}

FPDRESULT CDetector::Destroy()
{
	if (m_nDetectorID > 0)
	{
		m_pSDKApiHelper->m_pFnDestroy(m_nDetectorID);
		m_nDetectorID = 0;
	}
	return Err_OK;
}

FPDRESULT CDetector::Abort()
{
	return m_pSDKApiHelper->m_pFnAbort(m_nDetectorID);
}
FPDRESULT CDetector::SetUserCode(char* pszUserCode)
{
    return m_pSDKApiHelper->m_pFnSetUserCode(pszUserCode);
}

FPDRESULT CDetector::UseImageBuf(unsigned long long ullBufSizeInBytes)
{
    return m_pSDKApiHelper->m_pFnUseImageBuf(m_nDetectorID, ullBufSizeInBytes);
}

FPDRESULT CDetector::ClearImageBuf()
{
    return m_pSDKApiHelper->m_pFnClearImageBuf(m_nDetectorID);
}

FPDRESULT CDetector::QueryImageBuf(int& nFrameNum, int& nImageSize, int& nPropListMemSize)
{
    int nImageHeight, nImageWidth, nBytesPerPixel;
    int result = m_pSDKApiHelper->m_pFnQueryImageBuf(m_nDetectorID, &nFrameNum, &nImageHeight, &nImageWidth, &nBytesPerPixel, &nPropListMemSize);
    nImageSize = nImageHeight * nImageWidth * nBytesPerPixel;
    return result;
}

FPDRESULT CDetector::GetImageFromBuf(void* pImage, int nImageDataSize, int nPropListMemSize, int& nFrameNum)
{
    IRayVariantMap pProperties;
    pProperties.nItemCount = nPropListMemSize / sizeof(IRayVariantMapItem);
    pProperties.pItems = new IRayVariantMapItem[pProperties.nItemCount];
    int result = Err_Unknown;
    do{
        result = m_pSDKApiHelper->m_pFnGetImageFromBuf(m_nDetectorID, pImage, nImageDataSize, pProperties.pItems, nPropListMemSize);
        if (Err_OK != result)
            break;
        nFrameNum = GetImagePropertyInt(&pProperties, Enm_ImageTag_FrameNo);
    } while (false);

    delete[]pProperties.pItems;
    return result;
}

FPDRESULT CDetector::SetAttr(int nAttrID, int nValue)
{
    CHECK_SDKLIB_STATUS;

	IRayVariant var;
	var.vt = IVT_INT;
	var.val.nVal = nValue;
	FPDRESULT result = m_pSDKApiHelper->m_pFnSetAttr(m_nDetectorID, nAttrID, &var);
	if (Err_OK != result)
	{
	}
	return result;
}

FPDRESULT CDetector::SetAttr(int nAttrID, float fValue)
{
    CHECK_SDKLIB_STATUS;

	IRayVariant var;
	var.vt = IVT_FLT;
	var.val.fVal = fValue;
	FPDRESULT result = m_pSDKApiHelper->m_pFnSetAttr(m_nDetectorID, nAttrID, &var);
	if (Err_OK != result)
	{
	}
	return result;
}
FPDRESULT CDetector::SetAttr(int nAttrID, const char* strValue)
{
    CHECK_SDKLIB_STATUS;

	if (!strValue)
		return Err_InvalidParamValue;

	IRayVariant var;
	var.vt = IVT_STR;
	strncpy(var.val.strVal, strValue, IRAY_MAX_STR_LEN-1);
	FPDRESULT result = m_pSDKApiHelper->m_pFnSetAttr(m_nDetectorID, nAttrID, &var);
	if (Err_OK != result)
	{
	}
	return result;
}


int CDetector::GetAttr(int nAttrID, AttrResult& result)
{
	IRayVariant var;
	FPDRESULT ret = m_pSDKApiHelper->m_pFnGetAttr(m_nDetectorID, nAttrID, &var);
	if (Err_OK != ret)
	{
		memset(&result, 0, sizeof(result));
		return ret;
	}

	if (IVT_INT == var.vt)
	{
		result.nVal = var.val.nVal;
	}
	else if (IVT_FLT == var.vt)
	{
		result.fVal = var.val.fVal;
	}
	else if (IVT_STR == var.vt)
	{
		memcpy(result.strVal, var.val.strVal, IRAY_MAX_STR_LEN);
	}
    return Err_OK;
}

int CDetector::GetAttr(int nAttrID, int& retV)
{
    IRayVariant var;
    FPDRESULT ret = m_pSDKApiHelper->m_pFnGetAttr(m_nDetectorID, nAttrID, &var);
    if (Err_OK != ret)
    {
        return ret;
    }
    if (var.vt != IRAY_VAR_TYPE::IVT_INT)
    {
        return Err_InvalidParamType;
    }

    retV = var.val.nVal;
    return Err_OK;
}
int CDetector::GetAttr(int nAttrID, float& retV)
{
    IRayVariant var;
    FPDRESULT ret = m_pSDKApiHelper->m_pFnGetAttr(m_nDetectorID, nAttrID, &var);
    if (Err_OK != ret)
    {
        return ret;
    }
    if (var.vt != IRAY_VAR_TYPE::IVT_FLT)
    {
        return Err_InvalidParamType;
    }
    retV = var.val.fVal;
    return Err_OK;
}
int CDetector::GetAttr(int nAttrID, std::string& retV)
{
    IRayVariant var;
    FPDRESULT ret = m_pSDKApiHelper->m_pFnGetAttr(m_nDetectorID, nAttrID, &var);
    if (Err_OK != ret)
    {
        return ret;
    }
    if (var.vt != IRAY_VAR_TYPE::IVT_STR)
    {
        return Err_InvalidParamType;
    }
    retV = var.val.strVal;
    return Err_OK;
}


string CDetector::GetErrorInfo(int nErrorCode)
{
	ErrorInfo info;
	m_pSDKApiHelper->m_pFnGetErrInfo(nErrorCode, &info);
	return info.szDescription;
}

string CDetector::GetSDKVersion()
{
    char version[32] = { 0 };
    m_pSDKApiHelper->m_pFnGetSDKVersion(version);
    return string(version);
}


//编程指南有说明，total 512 bytes
FPDRESULT CDetector::WriteCustomerROM(int nCmdId, void* pBlockData, unsigned size)
{
    CHECK_SDKLIB_STATUS;

	if (size > 512)
		return Err_InvalidParamCount;
	ResetEvent(m_WaitAckEvent);
	m_CurCmdId = nCmdId;
	char mask[512] = { 0 };
	memset(mask, 1, size);
	IRayCmdParam param[2];
	param[0].pt = IPT_BLOCK;
	param[0].blc.pData = pBlockData;
	param[0].blc.uBytes = 512;
	param[1].pt = IPT_BLOCK;
	param[1].blc.pData = mask;
	param[1].blc.uBytes = 512;
	FPDRESULT result = m_pSDKApiHelper->m_pFnInvoke(m_nDetectorID, nCmdId, param, 2);
	if (Err_OK != result && Err_TaskPending != result)
	{
		//print("Invoke  failed! Err = %s", GetErrorInfo(result).c_str());
	}
	return result;
}

int CDetector::WaitEvent(int timeout)
{
	int wait = WaitForSingleObject(m_WaitAckEvent, timeout);
	if (WAIT_TIMEOUT == wait)
		return Err_TaskTimeOut;
	else
		return m_nLastError;
}

void CDetector::SDKCallback(int nDetectorID, int nEventID, int nEventLevel,
	const char* pszMsg, int nParam1, int nParam2, int nPtrParamLen, void* pParam)
{
    if ((Evt_TaskResult_Succeed == nEventID) || (Evt_TaskResult_Failed == nEventID) || (Evt_TaskResult_Canceled == nEventID))
	{

        if (Evt_TaskResult_Canceled == nEventID)
            m_nLastError = Err_Unknown;
        else
		    m_nLastError = nParam2;
		if (m_CurCmdId == nParam1)
		{
			SetEvent(m_WaitAckEvent);
		}
	}
    if (m_pUser)
        m_pUser->UserCallbackHandler(nDetectorID,nEventID, nEventLevel, pszMsg, nParam1, nParam2, nPtrParamLen, pParam);
    else if (m_UserCallback)
        m_UserCallback(nDetectorID, nEventID, nEventLevel, pszMsg, nParam1, nParam2, nPtrParamLen, pParam);
}

int CDetector::GetImagePropertyInt(IRayVariantMap* pProperties, int nTagId)
{
    if (!pProperties) 
        return -1;

    for (int nItemIndex=0; nItemIndex < pProperties->nItemCount; nItemIndex++)
    {
        if (nTagId == pProperties->pItems[nItemIndex].nMapKey)
        {
            return pProperties->pItems[nItemIndex].varMapVal.vt == IVT_INT ? pProperties->pItems[nItemIndex].varMapVal.val.nVal : -1;
        }
    }
    return -1;
}

int CDetector::OpenDefectTemplateFile(
	const char* pszFilePath,
	void** ppHandler,
	unsigned short* pWidth,
	unsigned short* pHeight,
	char** ppPoints)
{
    CHECK_SDKLIB_STATUS;
    char* ppRows = nullptr;
    char* ppCols = nullptr;
    char* ppDualReadCols2 = nullptr;
	return m_pSDKApiHelper->m_pFnOpenDefectTemplateFile(pszFilePath,ppHandler,pWidth, pHeight,ppPoints, &ppRows, &ppCols, &ppDualReadCols2);
}

int CDetector::SaveDefectTemplateFile(void* pHandler)
{
    CHECK_SDKLIB_STATUS;
    return m_pSDKApiHelper->m_pFnSaveDefectTemplateFile(pHandler);
}
int CDetector::CloseDefectTemplateFile(void* pHandler)
{
    CHECK_SDKLIB_STATUS;
    return m_pSDKApiHelper->m_pFnCloseDefectTemplateFile(pHandler);
}

std::string GetWorkDirPath()
{
	char buff[128] = {0};
	FILE* pFile = NULL;
	std::string workdir("");
	pFile = fopen("..\\common\\workdir_path.txt", "r");
	if (pFile)
	{
		fgets(buff, 128, pFile);
		fclose(pFile);
		workdir = std::string(buff);
	}
	else
	{
		pFile = fopen("workdir_path.txt", "r");
		if (pFile)
		{
			fgets(buff, 128, pFile);
			fclose(pFile);
			workdir = std::string(buff);
		}
	}
	return workdir.erase(workdir.find_last_not_of("\n") + 1);
}
