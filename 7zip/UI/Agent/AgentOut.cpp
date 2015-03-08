// AgentOutcpp

#include "StdAfx.h"

#include "Common/StringConvert.h"
#include "Common/IntToString.h"

#include "Windows/Defs.h"
#include "Windows/PropVariant.h"
#include "Windows/PropVariantConversions.h"
#include "Windows/FileDir.h"

#include "../../Compress/Copy/CopyCoder.h"
#include "../../Common/FileStreams.h"

#include "../Common/UpdatePair.h"
#include "../Common/EnumDirItems.h"
#include "../Common/HandlerLoader.h"
#include "../Common/UpdateCallback.h"
#include "../Common/OpenArchive.h"

#include "Agent.h"
#include "UpdateCallbackAgent.h"

using namespace NWindows;
using namespace NCOM;

static HRESULT CopyBlock(ISequentialInStream *inStream, ISequentialOutStream *outStream)
{
  CMyComPtr<ICompressCoder> copyCoder = new NCompress::CCopyCoder;
  return copyCoder->Code(inStream, outStream, NULL, NULL, NULL);
}

STDMETHODIMP CAgent::SetFolder(IFolderFolder *folder)
{
  _archiveNamePrefix.Empty();
  if (folder == NULL)
  {
    _archiveFolderItem = NULL;
    return S_OK;
    // folder = m_RootFolder;
  }
  else
  {
    CMyComPtr<IFolderFolder> archiveFolder = folder;
    CMyComPtr<IArchiveFolderInternal> archiveFolderInternal;
    RINOK(archiveFolder.QueryInterface(
        IID_IArchiveFolderInternal, &archiveFolderInternal));
    CAgentFolder *agentFolder;
    RINOK(archiveFolderInternal->GetAgentFolder(&agentFolder));
    _archiveFolderItem = agentFolder->_proxyFolderItem;
  }

  UStringVector pathParts;
  pathParts.Clear();
  CMyComPtr<IFolderFolder> folderItem = folder;
  if (_archiveFolderItem != NULL)
    while (true)
    {
      CMyComPtr<IFolderFolder> newFolder;
      folderItem->BindToParentFolder(&newFolder);  
      if (newFolder == NULL)
        break;
      CMyComBSTR name;
      folderItem->GetName(&name);
      pathParts.Insert(0, (const wchar_t *)name);
      folderItem = newFolder;
    }

  for(int i = 0; i < pathParts.Size(); i++)
  {
    _archiveNamePrefix += pathParts[i];
    _archiveNamePrefix += L'\\';
  }
  return S_OK;
}

STDMETHODIMP CAgent::SetFiles(const wchar_t *folderPrefix, 
    const wchar_t **names, UINT32 numNames)
{
  _folderPrefix = folderPrefix;
  _names.Clear();
  _names.Reserve(numNames);
  for (int i = 0; i < numNames; i++)
    _names.Add(names[i]);
  return S_OK;
}


static HRESULT GetFileTime(CAgent *agent, UINT32 itemIndex, FILETIME &fileTime)
{
  CPropVariant property;
  RINOK(agent->GetArchive()->GetProperty(itemIndex, kpidLastWriteTime, &property));
  if (property.vt == VT_FILETIME)
    fileTime = property.filetime;
  else if (property.vt == VT_EMPTY)
    fileTime = agent->DefaultTime;
  else
    throw 4190407;
  return S_OK;
}

static HRESULT EnumerateArchiveItems(CAgent *agent,
    const CProxyFolder &item, 
    const UString &prefix,
    CObjectVector<CArchiveItem> &archiveItems)
{
  int i;
  for(i = 0; i < item.Files.Size(); i++)
  {
    const CProxyFile &fileItem = item.Files[i];
    CArchiveItem archiveItem;

    RINOK(::GetFileTime(agent, fileItem.Index, archiveItem.LastWriteTime));

    CPropVariant property;
    agent->GetArchive()->GetProperty(fileItem.Index, kpidSize, &property);
    if (archiveItem.SizeIsDefined = (property.vt != VT_EMPTY))
      archiveItem.Size = ConvertPropVariantToUInt64(property);
    archiveItem.IsDirectory = false;
    archiveItem.Name = prefix + fileItem.Name;
    archiveItem.Censored = true; // test it
    archiveItem.IndexInServer = fileItem.Index;
    archiveItems.Add(archiveItem);
  }
  for(i = 0; i < item.Folders.Size(); i++)
  {
    const CProxyFolder &dirItem = item.Folders[i];
    UString fullName = prefix + dirItem.Name;
    if(dirItem.IsLeaf)
    {
      CArchiveItem archiveItem;
      RINOK(::GetFileTime(agent, dirItem.Index, archiveItem.LastWriteTime));
      archiveItem.IsDirectory = true;
      archiveItem.SizeIsDefined = false;
      archiveItem.Name = fullName;
      archiveItem.Censored = true; // test it
      archiveItem.IndexInServer = dirItem.Index;
      archiveItems.Add(archiveItem);
    }
    RINOK(EnumerateArchiveItems(agent, dirItem, fullName + UString(L'\\'), archiveItems));
  }
  return S_OK;
}

STDMETHODIMP CAgent::DoOperation(
    const wchar_t *filePath, 
    const CLSID *clsID, 
    const wchar_t *newArchiveName, 
    const Byte *stateActions, 
    const wchar_t *sfxModule,
    IFolderArchiveUpdateCallback *updateCallback100)
{
  if (!CanUpdate())
    return E_NOTIMPL;
  NUpdateArchive::CActionSet actionSet;
  for (int i = 0; i < NUpdateArchive::NPairState::kNumValues; i++)
    actionSet.StateActions[i] = (NUpdateArchive::NPairAction::EEnum)stateActions[i];

  CObjectVector<CDirItem> dirItems;

  UString folderPrefix = _folderPrefix;
  NFile::NName::NormalizeDirPathPrefix(folderPrefix);
  UString errorPath;
  RINOK(::EnumerateDirItems(folderPrefix, _names, _archiveNamePrefix, dirItems, errorPath));

  NWindows::NDLL::CLibrary library;

  CMyComPtr<IOutArchive> outArchive;
  if (GetArchive())
  {
    RINOK(GetArchive()->QueryInterface(IID_IOutArchive, (void **)&outArchive));
  }
  else
  {
    CHandlerLoader loader;
    RINOK(loader.CreateHandler(filePath, *clsID, (void **)&outArchive, true));
    library.Attach(loader.Detach());
  }

  NFileTimeType::EEnum fileTimeType;
  UINT32 value;
  RINOK(outArchive->GetFileTimeType(&value));

  switch(value)
  {
    case NFileTimeType::kWindows:
    case NFileTimeType::kDOS:
    case NFileTimeType::kUnix:
      fileTimeType = NFileTimeType::EEnum(value);
      break;
    default:
      return E_FAIL;
  }

  CObjectVector<CUpdatePair> updatePairs;

  CObjectVector<CArchiveItem> archiveItems;
  if (GetArchive())
  {
    RINOK(ReadItems());
    EnumerateArchiveItems(this, _proxyArchive->RootFolder,  L"", archiveItems);
  }

  GetUpdatePairInfoList(dirItems, archiveItems, fileTimeType, updatePairs);
  
  CObjectVector<CUpdatePair2> updatePairs2;
  UpdateProduce(dirItems, archiveItems, updatePairs, actionSet,
      updatePairs2);
  
  CUpdateCallbackAgent updateCallbackAgent;
  updateCallbackAgent.Callback = updateCallback100;
  CArchiveUpdateCallback *updateCallbackSpec = new CArchiveUpdateCallback;
  CMyComPtr<IArchiveUpdateCallback> updateCallback(updateCallbackSpec );

  updateCallbackSpec->DirPrefix = folderPrefix;
  updateCallbackSpec->DirItems = &dirItems;
  updateCallbackSpec->ArchiveItems = &archiveItems;
  updateCallbackSpec->UpdatePairs = &updatePairs2;
  updateCallbackSpec->Archive = GetArchive();
  updateCallbackSpec->Callback = &updateCallbackAgent;

  COutFileStream *outStreamSpec = new COutFileStream;
  CMyComPtr<IOutStream> outStream(outStreamSpec);
  UString archiveName = newArchiveName;
  {
    UString resultPath;
    int pos;
    if(!NFile::NDirectory::MyGetFullPathName(archiveName, resultPath, pos))
      throw 141716;
    NFile::NDirectory::CreateComplexDirectory(resultPath.Left(pos));
  }
  if (!outStreamSpec->Create(archiveName, true))
  {
    // ShowLastErrorMessage();
    return E_FAIL;
  }
  
  CMyComPtr<ISetProperties> setProperties;
  if (outArchive->QueryInterface(&setProperties) == S_OK)
  {
    if (m_PropNames.Size() == 0)
    {
      RINOK(setProperties->SetProperties(0, 0, 0));
    }
    else
    {
      CRecordVector<const wchar_t *> names;
      for(i = 0; i < m_PropNames.Size(); i++)
        names.Add((const wchar_t *)m_PropNames[i]);

      RINOK(setProperties->SetProperties(&names.Front(), 
          &m_PropValues.front(), names.Size()));
    }
  }
  m_PropNames.Clear();
  m_PropValues.clear();

  if (sfxModule != NULL)
  {
    CInFileStream *sfxStreamSpec = new CInFileStream;
    CMyComPtr<IInStream> sfxStream(sfxStreamSpec);
    if (!sfxStreamSpec->Open(sfxModule))
      throw "Can't open sfx module";
    RINOK(CopyBlock(sfxStream, outStream));
  }

  return outArchive->UpdateItems(outStream, updatePairs2.Size(),
      updateCallback);
}


HRESULT CAgent::CommonUpdate(
    const wchar_t *newArchiveName,
    int numUpdateItems,
    IArchiveUpdateCallback *updateCallback)
{
  if (!CanUpdate())
    return E_NOTIMPL;
  CMyComPtr<IOutArchive> outArchive;
  RINOK(GetArchive()->QueryInterface(IID_IOutArchive, (void **)&outArchive));

  COutFileStream *outStreamSpec = new COutFileStream;
  CMyComPtr<IOutStream> outStream(outStreamSpec);

  UString archiveName = newArchiveName;
  {
    UString resultPath;
    int pos;
    if(!NFile::NDirectory::MyGetFullPathName(archiveName, resultPath, pos))
      throw 141716;
    NFile::NDirectory::CreateComplexDirectory(resultPath.Left(pos));
  }

  /*
  bool isOK = false;
  for (int i = 0; i < (1 << 16); i++)
  {
    resultName = newArchiveName;
    if (i > 0)
    {
      wchar_t s[32];
      ConvertUInt64ToString(i, s);
      resultName += s;
    }
    if (outStreamSpec->Open(realPath))
    {
      isOK = true;
      break;
    }
    if (::GetLastError() != ERROR_FILE_EXISTS)
      return ::GetLastError();
  }
  if (!isOK)
    return ::GetLastError();
  */
  if (!outStreamSpec->Create(archiveName, true))
  {
    // ShowLastErrorMessage();
    return E_FAIL;
  }
  
  return outArchive->UpdateItems(outStream, numUpdateItems, updateCallback);
}


STDMETHODIMP CAgent::DeleteItems(
    const wchar_t *newArchiveName, 
    const UINT32 *indices, UINT32 numItems, 
    IFolderArchiveUpdateCallback *updateCallback100)
{
  if (!CanUpdate())
    return E_NOTIMPL;
  CUpdateCallbackAgent updateCallbackAgent;
  updateCallbackAgent.Callback = updateCallback100;
  CArchiveUpdateCallback *updateCallbackSpec = new CArchiveUpdateCallback;
  CMyComPtr<IArchiveUpdateCallback> updateCallback(updateCallbackSpec);
  
  CUIntVector realIndices;
  _archiveFolderItem->GetRealIndices(indices, numItems, realIndices);
  CObjectVector<CUpdatePair2> updatePairs;
  int curIndex = 0;
  UINT32 numItemsInArchive;
  RINOK(GetArchive()->GetNumberOfItems(&numItemsInArchive));
  for (int i = 0; i < numItemsInArchive; i++)
  {
    if (curIndex < realIndices.Size())
      if (realIndices[curIndex] == i)
      {
        curIndex++;
        continue;
      }
    CUpdatePair2 updatePair;
    updatePair.NewData = updatePair.NewProperties = false;
    updatePair.ExistInArchive = true;
    updatePair.ExistOnDisk = false;
    updatePair.IsAnti = false; // check it. Maybe it can be undefined
    updatePair.ArchiveItemIndex = i;
    updatePairs.Add(updatePair);
  }
  updateCallbackSpec->UpdatePairs = &updatePairs;
  updateCallbackSpec->Archive = GetArchive();
  updateCallbackSpec->Callback = &updateCallbackAgent;
  return CommonUpdate(newArchiveName, updatePairs.Size(), updateCallback);
}

HRESULT CAgent::CreateFolder(
    const wchar_t *newArchiveName, 
    const wchar_t *folderName, 
    IFolderArchiveUpdateCallback *updateCallback100)
{
  if (!CanUpdate())
    return E_NOTIMPL;
  CUpdateCallbackAgent updateCallbackAgent;
  updateCallbackAgent.Callback = updateCallback100;
  CArchiveUpdateCallback *updateCallbackSpec = new CArchiveUpdateCallback;
  CMyComPtr<IArchiveUpdateCallback> updateCallback(updateCallbackSpec);

  CObjectVector<CUpdatePair2> updatePairs;
  UINT32 numItemsInArchive;
  RINOK(GetArchive()->GetNumberOfItems(&numItemsInArchive));
  for (int i = 0; i < numItemsInArchive; i++)
  {
    CUpdatePair2 updatePair;
    updatePair.NewData = updatePair.NewProperties = false;
    updatePair.ExistInArchive = true;
    updatePair.ExistOnDisk = false;
    updatePair.IsAnti = false;  // check it. 
    updatePair.ArchiveItemIndex = i;
    updatePairs.Add(updatePair);
  }
  CUpdatePair2 updatePair;
  updatePair.NewData = updatePair.NewProperties = true;
  updatePair.ExistInArchive = false;
  updatePair.ExistOnDisk = true;
  updatePair.IsAnti = false;
  updatePair.ArchiveItemIndex = -1;
  updatePair.DirItemIndex = 0;

  updatePairs.Add(updatePair);

  CObjectVector<CDirItem> dirItems;
  CDirItem dirItem;

  dirItem.Attributes = FILE_ATTRIBUTE_DIRECTORY;
  dirItem.Size = 0;
  dirItem.Name = _archiveFolderItem->GetFullPathPrefix() + folderName;

  SYSTEMTIME systemTime;
  FILETIME fileTime;
  ::GetSystemTime(&systemTime);
  ::SystemTimeToFileTime(&systemTime, &fileTime);
  dirItem.LastAccessTime = dirItem.LastWriteTime = 
      dirItem.CreationTime = fileTime;

  dirItems.Add(dirItem);

  updateCallbackSpec->Callback = &updateCallbackAgent;
  updateCallbackSpec->DirItems = &dirItems;
  updateCallbackSpec->UpdatePairs = &updatePairs;
  updateCallbackSpec->Archive = GetArchive();
  return CommonUpdate(newArchiveName, updatePairs.Size(), updateCallback);
}


HRESULT CAgent::RenameItem(
    const wchar_t *newArchiveName, 
    const UINT32 *indices, UINT32 numItems, 
    const wchar_t *newItemName, 
    IFolderArchiveUpdateCallback *updateCallback100)
{
  if (!CanUpdate())
    return E_NOTIMPL;
  if (numItems != 1)
    return E_INVALIDARG;
  CUpdateCallbackAgent updateCallbackAgent;
  updateCallbackAgent.Callback = updateCallback100;
  CArchiveUpdateCallback *updateCallbackSpec = new CArchiveUpdateCallback;
  CMyComPtr<IArchiveUpdateCallback> updateCallback(updateCallbackSpec);
  
  CUIntVector realIndices;
  _archiveFolderItem->GetRealIndices(indices, numItems, realIndices);

  UString fullPrefix = _archiveFolderItem->GetFullPathPrefix();
  UString oldItemPath = fullPrefix + 
      _archiveFolderItem->GetItemName(indices[0]);
  UString newItemPath = fullPrefix + newItemName;

  CObjectVector<CUpdatePair2> updatePairs;
  int curIndex = 0;
  UINT32 numItemsInArchive;
  RINOK(GetArchive()->GetNumberOfItems(&numItemsInArchive));
  for (int i = 0; i < numItemsInArchive; i++)
  {
    if (curIndex < realIndices.Size())
      if (realIndices[curIndex] == i)
      {
        CUpdatePair2 updatePair;
        updatePair.NewData = false;
        updatePair.NewProperties = true;
        updatePair.ExistInArchive = true;
        updatePair.ExistOnDisk = false;
        RINOK(IsArchiveItemAnti(GetArchive(), i, updatePair.IsAnti));
        updatePair.ArchiveItemIndex = i;
        updatePair.NewNameIsDefined = true;

        updatePair.NewName = newItemName;

        UString oldFullPath;
        RINOK(GetArchiveItemPath(GetArchive(), i, DefaultName, oldFullPath));

        if (oldItemPath.CollateNoCase(oldFullPath.Left(oldItemPath.Length())) != 0)
          return E_INVALIDARG;

        updatePair.NewName = newItemPath + oldFullPath.Mid(oldItemPath.Length());
        updatePairs.Add(updatePair);
        curIndex++;
        continue;
      }
    CUpdatePair2 updatePair;
    updatePair.NewData = updatePair.NewProperties = false;
    updatePair.ExistInArchive = true;
    updatePair.ExistOnDisk = false;
    updatePair.IsAnti = false;
    updatePair.ArchiveItemIndex = i;
    updatePairs.Add(updatePair);
  }
  updateCallbackSpec->Callback = &updateCallbackAgent;
  updateCallbackSpec->UpdatePairs = &updatePairs;
  updateCallbackSpec->Archive = GetArchive();
  return CommonUpdate(newArchiveName, updatePairs.Size(), updateCallback);
}

STDMETHODIMP CAgent::SetProperties(const wchar_t **names, 
    const PROPVARIANT *values, INT32 numProperties)
{
  m_PropNames.Clear();
  m_PropValues.clear();
  for (int i = 0; i < numProperties; i++)
  {
    m_PropNames.Add(names[i]);
    m_PropValues.push_back(values[i]);
  }
  return S_OK;
}


