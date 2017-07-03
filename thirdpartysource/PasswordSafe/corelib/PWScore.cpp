/*
* Copyright (c) 2003-2009 Rony Shapiro <ronys@users.sourceforge.net>.
* All rights reserved. Use of the code is allowed under the
* Artistic License 2.0 terms, as specified in the LICENSE file
* distributed with this code, or available from
* http://www.opensource.org/licenses/artistic-license-2.0.php
*/
// file PWScore.cpp
//-----------------------------------------------------------------------------

#include "PWScore.h"
#include "corelib.h"
#include "BlowFish.h"
#include "PWSprefs.h"
#include "PWSrand.h"
#include "Util.h"
#include "UUIDGen.h"
#include "SysInfo.h"
#include "UTF8Conv.h"
#include "Report.h"
#include "VerifyFormat.h"
#include "PWSfileV3.h" // XXX cleanup with dynamic_cast
#include "StringXStream.h"

#include "os/typedefs.h"
#include "os/dir.h"
#include "os/debug.h"
#include "os/file.h"
#include "os/mem.h"

#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <algorithm>
#include <set>

using namespace std;

typedef ItemMMap::iterator ItemMMapIter;
typedef ItemMMap::const_iterator ItemMMapConstIter;
typedef std::pair <CUUIDGen, CUUIDGen> ItemMMap_Pair;

typedef ItemMap::iterator ItemMapIter;
typedef ItemMap::const_iterator ItemMapConstIter;
typedef std::pair <CUUIDGen, CUUIDGen> ItemMap_Pair;

unsigned char PWScore::m_session_key[20];
unsigned char PWScore::m_session_salt[20];
unsigned char PWScore::m_session_initialized = false;
Asker *PWScore::m_pAsker = NULL;
Reporter *PWScore::m_pReporter = NULL;

PWScore::PWScore() : m_currfile(_T("")),
                     m_passkey(NULL), m_passkey_len(0),
                     m_lockFileHandle(INVALID_HANDLE_VALUE),
                     m_lockFileHandle2(INVALID_HANDLE_VALUE),
                     m_LockCount(0), m_LockCount2(0),
                     m_usedefuser(false), m_defusername(_T("")),
                     m_ReadFileVersion(PWSfile::UNKNOWN_VERSION),
                     m_bDBChanged(false), m_bDBPrefsChanged(false),
                     m_IsReadOnly(false), m_nRecordsWithUnknownFields(0),
                     m_pfcnNotifyListModified(NULL), m_NotifyListInstance(NULL),
                     m_bNotifyList(false), m_bNotifyDB(false),
                     m_pfcnNotifyDBModified(NULL), m_NotifyDBInstance(NULL)
{
  // following should ideally be wrapped in a mutex
  if (!PWScore::m_session_initialized) {
    PWScore::m_session_initialized = true;
    CItemData::SetSessionKey(); // per-session initialization
    pws_os::mlock(m_session_key, sizeof(m_session_key));
    PWSrand::GetInstance()->GetRandomData(m_session_key, sizeof(m_session_key));
    PWSrand::GetInstance()->GetRandomData(m_session_salt,
                                          sizeof(m_session_salt));
  }
}

PWScore::~PWScore()
{
  // do NOT trash m_session_*, as there may be other cores around
  // relying on it. Trashing the ciphertext encrypted with it is enough
  if (m_passkey_len > 0) {
    trashMemory(m_passkey, ((m_passkey_len + 7)/8)*8);
    delete[] m_passkey;
  }
  if (!m_UHFL.empty()) {
    m_UHFL.clear();
  }
}

void PWScore::SetApplicationNameAndVersion(const stringT &appName,
                                           DWORD dwMajorMinor)
{
  int nMajor = HIWORD(dwMajorMinor);
  int nMinor = LOWORD(dwMajorMinor);
  Format(m_AppNameAndVersion, _T("%s V%d.%02d"), appName.c_str(),
         nMajor, nMinor);
}

void PWScore::AddEntry(const uuid_array_t &uuid, const CItemData &item)
{
  ASSERT(m_pwlist.find(uuid) == m_pwlist.end());
  m_pwlist[uuid] = item;

  SetDBChanged(true);
}

void PWScore::ClearData(void)
{
  if (m_passkey_len > 0) {
    trashMemory(m_passkey, ((m_passkey_len + 7)/8)*8);
    delete[] m_passkey;
    m_passkey_len = 0;
  }
  //Composed of ciphertext, so doesn't need to be overwritten
  m_pwlist.clear();

  // Clear out out dependents mappings
  m_base2aliases_mmap.clear();
  m_alias2base_map.clear();
  m_base2shortcuts_mmap.clear();
  m_shortcut2base_map.clear();

  // Clear out unknown fields
  m_UHFL.clear();

  // Clear out database filters
  m_MapFilters.clear();
}

void PWScore::ReInit(bool bNewFile)
{
  // Now reset all values as if created from new
  m_usedefuser = false;
  m_defusername = _T("");
  if (bNewFile)
    m_ReadFileVersion = PWSfile::NEWFILE;
  else
    m_ReadFileVersion = PWSfile::UNKNOWN_VERSION;
  if (m_passkey_len > 0) {
    trashMemory(m_passkey, ((m_passkey_len + 7)/8)*8);
    delete[] m_passkey;
    m_passkey = NULL;
    m_passkey_len = 0;
  }
  m_nRecordsWithUnknownFields = 0;
  m_UHFL.clear();

  SetChanged(false, false);
}

void PWScore::NewFile(const StringX &passkey)
{
  ClearData();
  SetPassKey(passkey);

  SetChanged(false, false);

  // default username is a per-database preference - wipe clean
  // for new database:
  m_usedefuser = false;
  m_defusername = _T("");
}

// functor object type for for_each:
struct RecordWriter {
  RecordWriter(PWSfile *out, PWScore *core) : m_out(out), m_core(core) {}
  void operator()(pair<CUUIDGen const, CItemData> &p)
  {
    StringX savePassword, uuid_str;
    if (p.second.GetStatus() == CItemData::ES_DELETED)
      return;

    savePassword = p.second.GetPassword();
    if (p.second.IsAlias()) {
      uuid_array_t item_uuid, base_uuid;
      p.second.GetUUID(item_uuid);
      m_core->GetAliasBaseUUID(item_uuid, base_uuid);

      const CUUIDGen buuid(base_uuid);
      StringX uuid_str(_T("[["));
      uuid_str += buuid.GetHexStr();
      uuid_str += _T("]]");
      p.second.SetPassword(uuid_str);
    } else if (p.second.IsShortcut()) {
      uuid_array_t item_uuid, base_uuid;
      p.second.GetUUID(item_uuid);
      m_core->GetShortcutBaseUUID(item_uuid, base_uuid);

      const CUUIDGen buuid(base_uuid);
      StringX uuid_str(_T("[~"));
      uuid_str += buuid.GetHexStr();
      uuid_str += _T("~]");
      p.second.SetPassword(uuid_str);
    }
 
    m_out->WriteRecord(p.second);
    p.second.SetPassword(savePassword);
    p.second.ClearStatus();
  }

private:
  PWSfile *m_out;
  PWScore *m_core;
};

int PWScore::WriteFile(const StringX &filename, PWSfile::VERSION version)
{
  int status;
  PWSfile *out = PWSfile::MakePWSfile(filename, version,
                                      PWSfile::Write, status);

  if (status != PWSfile::SUCCESS) {
    delete out;
    return status;
  }

  m_hdr.m_prefString = PWSprefs::GetInstance()->Store();
  m_hdr.m_whatlastsaved = m_AppNameAndVersion.c_str();

  out->SetHeader(m_hdr);

  // Give PWSfileV3 the unknown headers to write out
  // XXX cleanup gross dynamic_cast
  PWSfileV3 *out3 = dynamic_cast<PWSfileV3 *>(out);
  if (out3 != NULL) {
    out3->SetUnknownHeaderFields(m_UHFL);
    out3->SetFilters(m_MapFilters); // Give it the filters to write out
  }
  status = out->Open(GetPassKey());

  if (status != PWSfile::SUCCESS) {
    delete out;
    return CANT_OPEN_FILE;
  }

  RecordWriter write_record(out, this);
  for_each(m_pwlist.begin(), m_pwlist.end(), write_record);

  m_hdr = out->GetHeader(); // update time saved, etc.

  out->Close();
  delete out;

  /*
  // Now really delete the deleted items
  PurgeDeletedEntries();
  */

  SetChanged(false, false);

  m_ReadFileVersion = version; // needed when saving a V17 as V20 1st time [871893]

  return SUCCESS;
}

/*
void PWScore::PurgeDeletedEntries()
{
  std::vector<CUUIDGen>::iterator iter_del;
  for (iter_del = m_vdeleted.begin(); iter_del != m_vdeleted.end(); iter_del++) {
    ItemListIter iter = m_pwlist.find(*iter_del);
    if (iter == m_pwlist.end())
      continue;

    delete iter->second.GetDisplayInfo();
    m_pwlist.erase(iter);
  }

  m_vdeleted.clear();
}
*/


int PWScore::CheckPassword(const StringX &filename, const StringX &passkey)
{
  int status;

  if (!filename.empty())
    status = PWSfile::CheckPassword(filename, passkey, m_ReadFileVersion);
  else { // can happen if tries to export b4 save
    unsigned int t_passkey_len = passkey.length();
    if (t_passkey_len != m_passkey_len) // trivial test
      return WRONG_PASSWORD;
    int BlockLength = ((m_passkey_len + 7)/8)*8;
    unsigned char *t_passkey = new unsigned char[BlockLength];
    LPCTSTR plaintext = LPCTSTR(passkey.c_str());
    EncryptPassword((const unsigned char *)plaintext, t_passkey_len, t_passkey);
    if (memcmp(t_passkey, m_passkey, BlockLength) == 0)
      status = PWSfile::SUCCESS;
    else
      status = PWSfile::WRONG_PASSWORD;
    delete[] t_passkey;
  }

  switch (status) {
    case PWSfile::SUCCESS:
      return SUCCESS;
    case PWSfile::CANT_OPEN_FILE:
      return CANT_OPEN_FILE;
    case PWSfile::WRONG_PASSWORD:
      return WRONG_PASSWORD;
    default:
      ASSERT(0);
      return status; // should never happen
  }
}
#define MRE_FS _T("\xbb")

int PWScore::ReadFile(const StringX &a_filename,
                      const StringX &a_passkey)
{
  int status;
  PWSfile *in = PWSfile::MakePWSfile(a_filename, m_ReadFileVersion,
                                     PWSfile::Read, status, m_pAsker, m_pReporter);

  if (status != PWSfile::SUCCESS) {
    delete in;
    return status;
  }

  status = in->Open(a_passkey);

  // in the old times we could open even 1.x files
  // for compatibility reasons, we open them again, to see if this is really a "1.x" file
  if ((m_ReadFileVersion == PWSfile::V20) && (status == PWSfile::WRONG_VERSION)) {
    PWSfile::VERSION tmp_version;  // only for getting compatible to "1.x" files
    tmp_version = m_ReadFileVersion;
    m_ReadFileVersion = PWSfile::V17;
    in->Close();//Closing previously opened file
    in->SetCurVersion(PWSfile::V17);
    status = in->Open(a_passkey);
    if (status != PWSfile::SUCCESS) {
      m_ReadFileVersion = tmp_version;
    }
  }

  if (status != PWSfile::SUCCESS) {
    delete in;
    return CANT_OPEN_FILE;
  }
  if (m_ReadFileVersion == PWSfile::UNKNOWN_VERSION) {
    delete in;
    return UNKNOWN_VERSION;
  }

  m_hdr = in->GetHeader();
  m_OrigDisplayStatus = m_hdr.m_displaystatus; // for WasDisplayStatusChanged

  // Get pref string and tree display status & who saved when
  // all possibly empty!
  PWSprefs::GetInstance()->Load(m_hdr.m_prefString);

  // prepare handling of pre-2.0 DEFUSERCHR conversion
  if (m_ReadFileVersion == PWSfile::V17) {
    in->SetDefUsername(m_defusername);
    m_hdr.m_nCurrentMajorVersion = PWSfile::V17;
    m_hdr.m_nCurrentMinorVersion = 0;
  } else {
    // for 2.0 & later...
    in->SetDefUsername(PWSprefs::GetInstance()->
      GetPref(PWSprefs::DefaultUsername));
  }

  ClearData(); //Before overwriting old data, but after opening the file...
  SetChanged(false, false);

  SetPassKey(a_passkey); // so user won't be prompted for saves

  CItemData temp;
  uuid_array_t base_uuid, temp_uuid;
  StringX csMyPassword, cs_possibleUUID;
  bool go = true;
#ifdef DEMO
  bool limited = false;
#endif

  PWSfileV3 *in3 = dynamic_cast<PWSfileV3 *>(in); // XXX cleanup
  if (in3 != NULL  && !in3->GetFilters().empty())
    m_MapFilters = in3->GetFilters();

  UUIDList possible_aliases, possible_shortcuts;
  do {
    temp.Clear(); // Rather than creating a new one each time.
    status = in->ReadRecord(temp);
    switch (status) {
      case PWSfile::FAILURE:
      {
        // Show a useful (?) error message - better than
        // silently losing data (but not by much)
        // Best if title intact. What to do if not?

        stringT cs_msg;
        stringT cs_caption;
        LoadAString(cs_caption, IDSC_READ_ERROR);
        Format(cs_msg, IDSC_ENCODING_PROBLEM, temp.GetTitle().c_str());
        cs_msg = cs_caption + _S(": ") + cs_caption;
        if (m_pReporter != NULL)
          (*m_pReporter)(cs_msg);
      }
      // deliberate fall-through
      case PWSfile::SUCCESS:
        uuid_array_t uuid;
        temp.GetUUID(uuid);
        /*
         * If, for some reason, we're reading in a uuid that we already have
         * we will change the uuid, rather than overwrite an entry.
         * This is to protect the user from possible bugs that break
         * the uniqueness requirement of uuids.
         */
         if (m_pwlist.find(uuid) != m_pwlist.end()) {
#if defined( _DEBUG ) || defined( DEBUG )
           pws_os::Trace0(_T("Non-Unique uuid detected:\n"));
           CItemData::FieldBits bf;
           bf.flip();
           StringX dump = temp.GetPlaintext(TCHAR(':'), bf, TCHAR('-'), NULL);
           pws_os::Trace(_T("%s\n"), dump);
#endif
           temp.CreateUUID(); // replace duplicated uuid
           temp.GetUUID(uuid); // refresh uuid_array
         }
         // following is duplicated in Validate() - need to refactor
         csMyPassword = temp.GetPassword();
         if (csMyPassword.length() == 36) { // look for "[[uuid]]" or "[~uuid~]"
           cs_possibleUUID = csMyPassword.substr(2, 32);  // try to extract uuid
           ToLower(cs_possibleUUID);
           if (((csMyPassword.substr(0, 2) == _T("[[") &&
                 csMyPassword.substr(csMyPassword.length() - 2) == _T("]]")) ||
                (csMyPassword.substr(0, 2) == _T("[~") &&
                 csMyPassword.substr(csMyPassword.length() - 2) == _T("~]"))) &&
               cs_possibleUUID.find_first_not_of(_T("0123456789abcdef")) == 
               StringX::npos) {
             CUUIDGen uuid(cs_possibleUUID.c_str());
             uuid.GetUUID(base_uuid);
             temp.GetUUID(temp_uuid);
             if (csMyPassword.substr(1, 1) == _T("[")) {
               m_alias2base_map[temp_uuid] = base_uuid;
               possible_aliases.push_back(temp_uuid);
             } else {
               m_shortcut2base_map[temp_uuid] = base_uuid;
               possible_shortcuts.push_back(temp_uuid);
             }
           }
         } // uuid matching
#ifdef DEMO
         if (m_pwlist.size() < MAXDEMO) {
           m_pwlist[uuid] = temp;
         } else {
           limited = true;
         }
#else
         m_pwlist[uuid] = temp;
#endif
         break;
      case PWSfile::END_OF_FILE:
        go = false;
        break;
    } // switch
  } while (go);

  m_nRecordsWithUnknownFields = in->GetNumRecordsWithUnknownFields();
  in->GetUnknownHeaderFields(m_UHFL);
  int closeStatus = in->Close(); // in V3 this checks integrity
#ifdef DEMO
  if (closeStatus == PWSfile::SUCCESS && limited)
    closeStatus = PWScore::LIMIT_REACHED; // if integrity OK but LIMIT_REACHED, return latter
#endif
  delete in;

  AddDependentEntries(possible_aliases, NULL, CItemData::ET_ALIAS, CItemData::UUID);
  AddDependentEntries(possible_shortcuts, NULL, CItemData::ET_SHORTCUT, CItemData::UUID);
  possible_aliases.clear();
  possible_shortcuts.clear();
  SetDBChanged(false);

  return closeStatus;
}

static void ManageIncBackupFiles(const stringT &cs_filenamebase,
                                 size_t maxnumincbackups, stringT &cs_newname)
{
  // make sure we've no more than maxnumincbackups backup files,
  // and return the base name of the next backup file
  // (sans the suffix, which will be added by caller)

  stringT cs_filenamemask(cs_filenamebase);
  vector<stringT> files;
  vector<int> file_nums;

  cs_filenamemask += _T("_???.ibak");

  pws_os::FindFiles(cs_filenamemask, files);

  for (vector<stringT>::iterator iter = files.begin();
       iter != files.end(); iter++) {
    stringT ibak_number_str = iter->substr(iter->length() - 8, 3);
    if (ibak_number_str.find_first_not_of(_T("0123456789")) != stringT::npos)
      continue;
    istringstreamT is(ibak_number_str);
    int n;
    is >> n;
    file_nums.push_back(n);
  }


  if (file_nums.empty()) {
    cs_newname = cs_filenamebase + _T("_001");
    return;
  }

  sort(file_nums.begin(), file_nums.end());

  int nnn = file_nums.back();
  nnn++;
  if (nnn > 999) nnn = 1;

  Format(cs_newname, _T("%s_%03d"), cs_filenamebase.c_str(), nnn);

  int i = 0;
  size_t num_found = file_nums.size();
  stringT excess_file;
  while (num_found >= maxnumincbackups) {
    nnn = file_nums[i];
    Format(excess_file, _T("%s_%03d.ibak"), cs_filenamebase.c_str(), nnn);
    i++;
    num_found--;
    if (!pws_os::DeleteAFile(excess_file)) {
      pws_os::Trace(_T("DeleteFile(%s) failed"), excess_file.c_str());
      continue;
    }
  }
}

bool PWScore::BackupCurFile(int maxNumIncBackups, int backupSuffix,
                            const stringT &userBackupPrefix,
                            const stringT &userBackupDir)
{
  stringT cs_temp, cs_newfile;
  const stringT path(m_currfile.c_str());
  stringT drv, dir, name, ext;

  pws_os::splitpath(path, drv, dir, name, ext);
  // Get location for intermediate backup
  if (userBackupDir.empty()) { // directory same as database's
    // Get directory containing database
    cs_temp = drv + dir;
    // (in Windows, need to verify for non-Windows)
    // splitpath directory ends with a '/', therefore do not need:
    // cs_temp += pws_os::PathSeparator;
  } else {
    cs_temp = userBackupDir;
  }

  // generate prefix of intermediate backup file name
  if (userBackupPrefix.empty()) {
    cs_temp += name;
  } else {
    cs_temp += userBackupPrefix;
  }

  // Add on suffix
  switch (backupSuffix) { // case values from order in listbox.
    case 1: // YYYYMMDD_HHMMSS suffix
      {
        time_t now;
        time(&now);
        StringX cs_datetime = PWSUtil::ConvertToDateTimeString(now,
                                                               TMC_EXPORT_IMPORT);
        cs_temp += _T("_");
        StringX nf = cs_temp.c_str() + 
                     cs_datetime.substr( 0, 4) +  // YYYY
                     cs_datetime.substr( 5, 2) +  // MM
                     cs_datetime.substr( 8, 2) +  // DD
                     StringX(_T("_")) +
                     cs_datetime.substr(11, 2) +  // HH
                     cs_datetime.substr(14, 2) +  // MM
                     cs_datetime.substr(17, 2);   // SS
        cs_newfile = nf.c_str();
        break;
      }
    case 2: // _nnn suffix
      ManageIncBackupFiles(cs_temp, maxNumIncBackups, cs_newfile);
      break;
    case 0: // no suffix
    default:
      cs_newfile = cs_temp;
      break;
  }

  cs_newfile +=  _T(".ibak");

  // Current file becomes backup
  // Directories along the specified backup path are created as needed
  return pws_os::RenameFile(m_currfile.c_str(), cs_newfile);
}

void PWScore::ChangePassword(const StringX &newPassword)
{
  SetPassKey(newPassword);
  SetDBChanged(true);
}

// functor object type for find_if:
struct FieldsMatch {
  bool operator()(pair<CUUIDGen, CItemData> p) {
    const CItemData &item = p.second;
    return (item.GetStatus() != CItemData::ES_DELETED &&
            m_group == item.GetGroup() &&
            m_title == item.GetTitle() &&
            m_user  == item.GetUser());
  }
  FieldsMatch(const StringX &a_group, const StringX &a_title,
              const StringX &a_user) :
  m_group(a_group), m_title(a_title), m_user(a_user) {}

private:
  const StringX &m_group;
  const StringX &m_title;
  const StringX &m_user;
};

// Finds stuff based on title, group & user fields only
ItemListIter PWScore::Find(const StringX &a_group,const StringX &a_title,
                           const StringX &a_user)
{
  FieldsMatch fields_match(a_group, a_title, a_user);

  ItemListIter retval = find_if(m_pwlist.begin(), m_pwlist.end(),
                                fields_match);
  return retval;
}

struct TitleMatch {
  bool operator()(pair<CUUIDGen, CItemData> p) {
    const CItemData &item = p.second;
    return (m_title == item.GetTitle());
  }
  TitleMatch(const StringX &a_title) :
    m_title(a_title) {}

private:
  const StringX &m_title;
};

ItemListIter PWScore::GetUniqueBase(const StringX &a_title, bool &bMultiple)
{
  ItemListIter retval(m_pwlist.end());
  int num(0);
  TitleMatch TitleMatch(a_title);

  ItemListIter found(m_pwlist.begin());
  do {
    found = find_if(found, m_pwlist.end(), TitleMatch);
    if (found != m_pwlist.end()) {
      num++;
      if (num == 1) {
        // Save first
        retval = found;
      } else {
        // More than 1, set to end()
        retval = m_pwlist.end();
        break;
      }
      found++;
    } else
      break;
  } while (found != m_pwlist.end());

  // It is 1 if only 1, but 0 if none & 2 if more than 1 (we just stopped at the second)
  bMultiple = (num > 1);
  return retval;
}

struct GroupTitle_TitleUserMatch {
  bool operator()(pair<CUUIDGen, CItemData> p) {
    const CItemData &item = p.second;
    return ((m_gt == item.GetGroup() && m_tu == item.GetTitle()) ||
            (m_gt == item.GetTitle() && m_tu == item.GetUser()));
  }
  GroupTitle_TitleUserMatch(const StringX &a_grouptitle,
                            const StringX &a_titleuser) :
                            m_gt(a_grouptitle),  m_tu(a_titleuser) {}

private:
  const StringX &m_gt;
  const StringX &m_tu;
};

ItemListIter PWScore::GetUniqueBase(const StringX &grouptitle, 
                                    const StringX &titleuser, bool &bMultiple)
{
  ItemListIter retval(m_pwlist.end());
  int num(0);
  GroupTitle_TitleUserMatch GroupTitle_TitleUserMatch(grouptitle, titleuser);

  ItemListIter found(m_pwlist.begin());
  do {
    found = find_if(found, m_pwlist.end(), GroupTitle_TitleUserMatch);
    if (found != m_pwlist.end()) {
      num++;
      if (num == 1) {
        // Save first
        retval = found;
      } else {
        // More than 1, set to end()
        retval = m_pwlist.end();
        break;
      }
      found++;
    } else
      break;
  } while (found != m_pwlist.end());

  // It is 1 if only 1, but 0 if none & 2 if more than 1 (we just stopped at the second)
  bMultiple = (num > 1);
  return retval;
}

void PWScore::EncryptPassword(const unsigned char *plaintext, int len,
                              unsigned char *ciphertext) const
{
  // ciphertext is ((len +7)/8)*8 bytes long
  BlowFish *Algorithm = BlowFish::MakeBlowFish(m_session_key,
                                               sizeof(m_session_key),
                                               m_session_salt,
                                               sizeof(m_session_salt));
  int BlockLength = ((len + 7)/8)*8;
  unsigned char curblock[8];

  for (int x = 0; x< BlockLength; x += 8) {
    int i;
    if ((len == 0) ||
      ((len%8 != 0) && (len - x < 8))) {
        //This is for an uneven last block
        memset(curblock, 0, 8);
        for (i = 0; i < len %8; i++)
          curblock[i] = plaintext[x + i];
    } else
      for (i = 0; i < 8; i++)
        curblock[i] = plaintext[x + i];
    Algorithm->Encrypt(curblock, curblock);
    memcpy(ciphertext + x, curblock, 8);
  }
  trashMemory(curblock, 8);
  delete Algorithm;
}

void PWScore::SetPassKey(const StringX &new_passkey)
{
  // if changing, clear old
  if (m_passkey_len > 0) {
    trashMemory(m_passkey, ((m_passkey_len + 7)/8)*8);
    delete[] m_passkey;
  }

  m_passkey_len = new_passkey.length() * sizeof(TCHAR);

  int BlockLength = ((m_passkey_len + 7)/8)*8;
  m_passkey = new unsigned char[BlockLength];
  LPCTSTR plaintext = LPCTSTR(new_passkey.c_str());
  EncryptPassword((const unsigned char *)plaintext, m_passkey_len, m_passkey);
}

StringX PWScore::GetPassKey() const
{
  StringX retval(_T(""));
  if (m_passkey_len > 0) {
    const unsigned int BS = BlowFish::BLOCKSIZE;
    unsigned int BlockLength = ((m_passkey_len + (BS-1))/BS)*BS;
    BlowFish *Algorithm = BlowFish::MakeBlowFish(m_session_key,
                                                 sizeof(m_session_key),
                                                 m_session_salt,
                                                 sizeof(m_session_salt));
    unsigned char curblock[BS];

    for (unsigned int x = 0; x < BlockLength; x += BS) {
      unsigned int i;
      for (i = 0; i < BS; i++)
        curblock[i] = m_passkey[x + i];

      Algorithm->Decrypt(curblock, curblock);
      for (i = 0; i < BS; i += sizeof(TCHAR))
        if (x + i < m_passkey_len)
          retval += *((TCHAR*)(curblock + i));
    }
    trashMemory(curblock, sizeof(curblock));
    delete Algorithm;
  }
  return retval;
}

void PWScore::SetDisplayStatus(const vector<bool> &s)
{ 
  // DON'T set m_bDBChanged!
  // Application should use WasDisplayStatusChanged()
  // to determine if state has changed.
  // This allows app to silently save without nagging user
  m_hdr.m_displaystatus = s;
}

const vector<bool> &PWScore::GetDisplayStatus() const
{
  return m_hdr.m_displaystatus;
}

bool PWScore::WasDisplayStatusChanged() const
{
  // m_OrigDisplayStatus is set while reading file.
  // m_hdr.m_displaystatus may be changed via SetDisplayStatus
  return m_hdr.m_displaystatus != m_OrigDisplayStatus;
}


// GetUniqueGroups - returns an array of all group names, with no duplicates.
void PWScore::GetUniqueGroups(vector<stringT> &aryGroups) const
{
  // use the fact that set eliminates dups for us
  set<stringT> setGroups;

  ItemListConstIter iter;

  for (iter = m_pwlist.begin(); iter != m_pwlist.end(); iter++ ) {
    const CItemData &ci = iter->second;
    setGroups.insert(ci.GetGroup().c_str());
  }

  aryGroups.clear();
  // copy unique results from set to caller's vector
  copy(setGroups.begin(), setGroups.end(), back_inserter(aryGroups));
}

void PWScore::CopyPWList(const ItemList &in)
{
  m_pwlist = in;
  SetDBChanged(true);
}

bool PWScore::Validate(stringT &status)
{
  // Check uuid is valid
  // Check PWH is valid
  // Check alias password has corresponding base entry
  // Check shortcut password has corresponding base entry
  // Note that with m_pwlist implemented as a map keyed
  // on uuids, each entry is guaranteed to have
  // a unique uuid. The uniqueness invariant
  // should be enforced elsewhere (upon read/import).

  uuid_array_t uuid_array, base_uuid, temp_uuid;
  int n = -1;
  unsigned num_PWH_fixed = 0;
  unsigned num_uuid_fixed = 0;
  unsigned num_alias_warnings, num_shortcuts_warnings;

  CReport rpt;
  stringT cs_Error, cs_temp;
  LoadAString(cs_temp, IDSC_RPTVALIDATE);
  rpt.StartReport(cs_temp.c_str(), GetCurFile().c_str());

  pws_os::Trace(_T("%s : Start validation\n"), PWSUtil::GetTimeStamp());

  UUIDList possible_aliases, possible_shortcuts;
  ItemListIter iter;
  for (iter = m_pwlist.begin(); iter != m_pwlist.end(); iter++) {
    CItemData &ci = iter->second;
    ci.GetUUID(uuid_array);
    n++;
    if (uuid_array[0] == 0x00) {
      CItemData fixedItem(ci);
      num_uuid_fixed += fixedItem.ValidateUUID(m_hdr.m_nCurrentMajorVersion,
                                               m_hdr.m_nCurrentMinorVersion,
                                               uuid_array);
      Format(cs_Error, IDSC_VALIDATEUUID,
             ci.GetGroup().c_str(), ci.GetTitle().c_str(), ci.GetUser().c_str());
      rpt.WriteLine(cs_Error);

      m_pwlist.erase(iter); // erasing item in mid-iteration!
      fixedItem.SetStatus(CItemData::ES_MODIFIED);
      AddEntry(fixedItem);
    }
    if (!ci.ValidatePWHistory()) {
      Format(cs_Error, IDSC_VALIDATEPWH,
             ci.GetGroup().c_str(), ci.GetTitle().c_str(), ci.GetUser().c_str());
      rpt.WriteLine(cs_Error);
      num_PWH_fixed++;
    }
    StringX csMyPassword = ci.GetPassword();
    if (csMyPassword.length() == 36) { // look for "[[uuid]]" or "[~uuid~]"
      StringX cs_possibleUUID = csMyPassword.substr(2, 32); // try to extract uuid
      ToLower(cs_possibleUUID);
      if (((csMyPassword.substr(0,2) == _T("[[") &&
            csMyPassword.substr(csMyPassword.length() - 2) == _T("]]")) ||
           (csMyPassword.substr(0, 2) == _T("[~") &&
            csMyPassword.substr(csMyPassword.length() - 2) == _T("~]"))) &&
          cs_possibleUUID.find_first_not_of(_T("0123456789abcdef")) == StringX::npos) {
        CUUIDGen uuid(cs_possibleUUID.c_str());
        uuid.GetUUID(base_uuid);
        ci.GetUUID(temp_uuid);
        if (csMyPassword.substr(0, 2) == _T("[[")) {
          m_alias2base_map[temp_uuid] = base_uuid;
          possible_aliases.push_back(temp_uuid);
        } else {
          m_shortcut2base_map[temp_uuid] = base_uuid;
          possible_shortcuts.push_back(temp_uuid);
        }
      }
    } // uuid matching
  } // iteration over m_pwlist

  num_alias_warnings = AddDependentEntries(possible_aliases, &rpt, CItemData::ET_ALIAS, CItemData::UUID);
  num_shortcuts_warnings = AddDependentEntries(possible_shortcuts, &rpt, CItemData::ET_SHORTCUT, CItemData::UUID);
  possible_aliases.clear();
  possible_shortcuts.clear();

  pws_os::Trace(_T("%s : End validation. %d entries processed\n"), 
        PWSUtil::GetTimeStamp(), n + 1);
  rpt.EndReport();

  if ((num_uuid_fixed + num_PWH_fixed + num_alias_warnings + num_shortcuts_warnings) > 0) {
    Format(status, IDSC_NUMPROCESSED,
           n + 1, num_uuid_fixed, num_PWH_fixed,
           num_alias_warnings, num_shortcuts_warnings);
    SetDBChanged(true);
    return true;
  } else {
    return false;
  }
}

void PWScore::ClearFileUUID()
{
  memset(m_hdr.m_file_uuid_array, 0x00, sizeof(m_hdr.m_file_uuid_array));
}

void PWScore::SetFileUUID(uuid_array_t &file_uuid_array)
{
  memcpy(m_hdr.m_file_uuid_array, file_uuid_array,
         sizeof(m_hdr.m_file_uuid_array));
}

void PWScore::GetFileUUID(uuid_array_t &file_uuid_array) const
{
  memcpy(file_uuid_array, m_hdr.m_file_uuid_array, sizeof(file_uuid_array));
}

StringX PWScore::GetUniqueTitle(const StringX &path, const StringX &title,
                                const StringX &user, const int IDS_MESSAGE)
{
  StringX new_title(title);
  if (Find(path, title, user) != m_pwlist.end()) {
    // Find a unique "Title"
    ItemListConstIter listpos;
    int i = 0;
    StringX s_copy;
    do {
      i++;
      Format(s_copy, IDS_MESSAGE, i);
      new_title = title + s_copy;
      listpos = Find(path, new_title, user);
    } while (listpos != m_pwlist.end());
  }
  return new_title;
}

void PWScore::AddDependentEntry(const uuid_array_t &base_uuid, const uuid_array_t &entry_uuid, 
                                const CItemData::EntryType type)
{
  ItemMMap *pmmap;
  ItemMap *pmap;
  if (type == CItemData::ET_ALIAS) {
    pmap = &m_alias2base_map;
    pmmap = &m_base2aliases_mmap;
  } else if (type == CItemData::ET_SHORTCUT) {
    pmap = &m_shortcut2base_map;
    pmmap = &m_base2shortcuts_mmap;
  } else
    return;

  ItemListIter iter = m_pwlist.find(base_uuid);
  ASSERT(iter != m_pwlist.end());

  if (type == CItemData::ET_ALIAS) {
    // Mark base entry as a base entry - must be a normal entry or already an alias base
    ASSERT(iter->second.IsNormal() || iter->second.IsAliasBase());
    iter->second.SetAliasBase();
  } else if (type == CItemData::ET_SHORTCUT) {
    // Mark base entry as a base entry - must be a normal entry or already a shortcut base
    ASSERT(iter->second.IsNormal() || iter->second.IsShortcutBase());
    iter->second.SetShortcutBase();
  }

  // Add to both the base->type multimap and the type->base map
  pmmap->insert(ItemMMap_Pair(base_uuid, entry_uuid));
  pmap->insert(ItemMap_Pair(entry_uuid, base_uuid));
}

void PWScore::RemoveDependentEntry(const uuid_array_t &base_uuid, const uuid_array_t &entry_uuid,
                                   const CItemData::EntryType type)
{
  ItemMMap *pmmap;
  ItemMap *pmap;
  if (type == CItemData::ET_ALIAS) {
    pmap = &m_alias2base_map;
    pmmap = &m_base2aliases_mmap;
  } else if (type == CItemData::ET_SHORTCUT) {
    pmap = &m_shortcut2base_map;
    pmmap = &m_base2shortcuts_mmap;
  } else
    return;

  // Remove from entry -> base map
  pmap->erase(entry_uuid);

  // Remove from base -> entry multimap
  ItemMMapIter mmiter;
  ItemMMapIter mmlastElement;

  mmiter = pmmap->find(base_uuid);
  if (mmiter == pmmap->end())
    return;

  mmlastElement = pmmap->upper_bound(base_uuid);
  uuid_array_t mmiter_uuid;

  for ( ; mmiter != mmlastElement; mmiter++) {
    mmiter->second.GetUUID(mmiter_uuid);
    if (memcmp(entry_uuid, mmiter_uuid, sizeof(uuid_array_t)) == 0) {
      pmmap->erase(mmiter);
      break;
    }
  }

  // Reset base entry to normal if it has no more aliases
  if (pmmap->find(base_uuid) == pmmap->end()) {
    ItemListIter iter = m_pwlist.find(base_uuid);
    if (iter != m_pwlist.end())
      iter->second.SetNormal();
  }
}

void PWScore::RemoveAllDependentEntries(const uuid_array_t &base_uuid, 
                                        const CItemData::EntryType type)
{
  ItemMMap *pmmap;
  ItemMap *pmap;
  if (type == CItemData::ET_ALIAS) {
    pmap = &m_alias2base_map;
    pmmap = &m_base2aliases_mmap;
  } else if (type == CItemData::ET_SHORTCUT) {
    pmap = &m_shortcut2base_map;
    pmmap = &m_base2shortcuts_mmap;
  } else
    return;

  // Remove from entry -> base map for each entry
  ItemMMapIter itr;
  ItemMMapIter lastElement;

  itr = pmmap->find(base_uuid);
  if (itr == pmmap->end())
    return;

  lastElement = pmmap->upper_bound(base_uuid);
  uuid_array_t itr_uuid;

  for ( ; itr != lastElement; itr++) {
    itr->second.GetUUID(itr_uuid);
    // Remove from entry -> base map
    pmap->erase(itr_uuid);
  }

  // Remove from base -> entry multimap
  pmmap->erase(base_uuid);

  // Reset base entry to normal
  ItemListIter iter = m_pwlist.find(base_uuid);
  if (iter != m_pwlist.end())
    iter->second.SetNormal();
}

void PWScore::MoveDependentEntries(const uuid_array_t &from_baseuuid,
                                   const uuid_array_t &to_baseuuid, 
                                   const CItemData::EntryType type)
{
  ItemMMap *pmmap;
  ItemMap *pmap;
  if (type == CItemData::ET_ALIAS) {
    pmap = &m_alias2base_map;
    pmmap = &m_base2aliases_mmap;
  } else if (type == CItemData::ET_SHORTCUT) {
    pmap = &m_shortcut2base_map;
    pmmap = &m_base2shortcuts_mmap;
  } else
    return;

  ItemMMapIter from_itr;
  ItemMMapIter lastfromElement;

  from_itr = pmmap->find(from_baseuuid);
  if (from_itr == pmmap->end())
    return;

  lastfromElement = pmmap->upper_bound(from_baseuuid);

  for ( ; from_itr != lastfromElement; from_itr++) {
    // Add to new base in base -> entry multimap
    pmmap->insert(ItemMMap_Pair(to_baseuuid, from_itr->second));
    // Remove from entry -> base map
    pmap->erase(from_itr->second);
    // Add to entry -> base map (new base)
    pmap->insert(ItemMap_Pair(from_itr->second, to_baseuuid));    
  }

  // Now delete all old base entries
  pmmap->erase(from_baseuuid);
}

int PWScore::AddDependentEntries(UUIDList &dependentlist, CReport *rpt,
                                 const CItemData::EntryType type, const int &iVia)
{
  // When called during validation of a database  - *rpt is valid
  // When called during the opening of a database or during drag & drop
  //   - *rpt is NULL and no report generated

  // type is either CItemData::ET_ALIAS or CItemData::ET_SHORTCUT

  // If iVia == CItemData::UUID, the password was "[[uuidstr]]" or "[~uuidstr~]" of the
  //   associated base entry
  // If iVia == CItemData::PASSWORD, the password is expected to be in the full format 
  // [g:t:u], where g and/or u may be empty.

  ItemMap *pmap;
  ItemMMap *pmmap;
  if (type == CItemData::ET_ALIAS) {
    pmap = &m_alias2base_map;
    pmmap = &m_base2aliases_mmap;
  } else if (type == CItemData::ET_SHORTCUT) {
    pmap = &m_shortcut2base_map;
    pmmap = &m_base2shortcuts_mmap;
  } else
    return -1;

  int num_warnings(0);

  if (!dependentlist.empty()) {
    UUIDListIter paiter;
    ItemListIter iter;
    StringX csPwdGroup, csPwdTitle, csPwdUser, tmp;
    uuid_array_t base_uuid, entry_uuid;
    bool bwarnings(false);
    stringT strError;

    for (paiter = dependentlist.begin();
         paiter != dependentlist.end(); paiter++) {
      iter = m_pwlist.find(*paiter);
      if (iter == m_pwlist.end())
        return num_warnings;

      CItemData *pci_curitem = &iter->second;
      pci_curitem->GetUUID(entry_uuid);
      GetDependentEntryBaseUUID(entry_uuid, base_uuid, type);

      // Delete it - we will put it back if it is an alias/shortcut
      pmap->erase(entry_uuid);

      if (iVia == CItemData::UUID) {
        iter = m_pwlist.find(base_uuid);
      } else {
        tmp = pci_curitem->GetPassword();
        // Remove leading '[['/'[~' & trailing ']]'/'~]'
        tmp = tmp.substr(2, tmp.length() - 4);
        csPwdGroup = tmp.substr(0, tmp.find_first_of(_T(":")));
        // Skip over 'group:'
        tmp = tmp.substr(csPwdGroup.length() + 1);
        csPwdTitle = tmp.substr(0, tmp.find_first_of(_T(":")));
        // Skip over 'title:'
        csPwdUser = tmp.substr(csPwdTitle.length() + 1);
        iter = Find(csPwdGroup, csPwdTitle, csPwdUser);
      }

      if (iter != m_pwlist.end()) {
        const CItemData::EntryType type2 = iter->second.GetEntryType();
        if (type == CItemData::ET_SHORTCUT) {
          // Adding shortcuts -> Base must be normal or already a shortcut base
          if (type2 != CItemData::ET_NORMAL && type2 != CItemData::ET_SHORTCUTBASE) {
            // Bad news!
            if (rpt != NULL) {
              if (!bwarnings) {
                bwarnings = true;
                LoadAString(strError, IDSC_IMPORTWARNINGHDR);
                rpt->WriteLine(strError);
              }
              stringT cs_type;
              LoadAString(cs_type, IDSC_SHORTCUT);
              Format(strError, IDSC_IMPORTWARNING3, cs_type.c_str(),
                     pci_curitem->GetGroup().c_str(), pci_curitem->GetTitle().c_str(), 
                     pci_curitem->GetUser().c_str(), cs_type.c_str());
              rpt->WriteLine(strError);
            }
            // Invalid - delete!
            RemoveEntryAt(m_pwlist.find(entry_uuid));
            continue;
          } 
        }
        if (type == CItemData::ET_ALIAS) {
          // Adding Aliases -> Base must be normal or already a alias base
          if (type2 != CItemData::ET_NORMAL && type2 != CItemData::ET_ALIASBASE) {
            // Bad news!
            if (rpt != NULL) {
              if (!bwarnings) {
                bwarnings = true;
                LoadAString(strError, IDSC_IMPORTWARNINGHDR);
                rpt->WriteLine(strError);
              }
              stringT cs_type;
              LoadAString(cs_type, IDSC_ALIAS);
              Format(strError, IDSC_IMPORTWARNING3, cs_type.c_str(),
                     pci_curitem->GetGroup().c_str(), pci_curitem->GetTitle().c_str(), 
                     pci_curitem->GetUser().c_str(), cs_type.c_str());
              rpt->WriteLine(strError);
            }
            // Invalid - delete!
            RemoveEntryAt(m_pwlist.find(entry_uuid));
            continue;
          }
          if (type2 == CItemData::ET_ALIAS) {
            // This is an alias too!  Not allowed!  Make new one point to original base
            // Note: this may be random as who knows the order of reading records?
            uuid_array_t temp_uuid;
            iter->second.GetUUID(temp_uuid);
            GetDependentEntryBaseUUID(temp_uuid, base_uuid, type);
            if (rpt != NULL) {
              if (!bwarnings) {
                bwarnings = true;
                LoadAString(strError, IDSC_IMPORTWARNINGHDR);
                rpt->WriteLine(strError);
              }
              Format(strError, IDSC_IMPORTWARNING1, pci_curitem->GetGroup().c_str(),
                     pci_curitem->GetTitle().c_str(), pci_curitem->GetUser().c_str());
              rpt->WriteLine(strError);
              LoadAString(strError, IDSC_IMPORTWARNING1A);
              rpt->WriteLine(strError);
            }
            pci_curitem->SetAlias();
            num_warnings++;
          }
        }
        iter->second.GetUUID(base_uuid);
        if (type == CItemData::ET_ALIAS) {
          iter->second.SetAliasBase();
        } else
        if (type == CItemData::ET_SHORTCUT) {
          iter->second.SetShortcutBase();
        }

        pmmap->insert(ItemMMap_Pair(base_uuid, entry_uuid));
        pmap->insert(ItemMap_Pair(entry_uuid, base_uuid));
        if (type == CItemData::ET_ALIAS) {
          pci_curitem->SetPassword(_T("[Alias]"));
          pci_curitem->SetAlias();
        } else
        if (type == CItemData::ET_SHORTCUT) {
          pci_curitem->SetPassword(_T("[Shortcut]"));
          pci_curitem->SetShortcut();
        }
      } else {
        // Specified base does not exist!
        if (rpt != NULL) {
          if (!bwarnings) {
            bwarnings = true;
            LoadAString(strError, IDSC_IMPORTWARNINGHDR);
            rpt->WriteLine(strError);
          }
          Format(strError, IDSC_IMPORTWARNING2, pci_curitem->GetGroup().c_str(),
                 pci_curitem->GetTitle().c_str(), pci_curitem->GetUser().c_str());
          rpt->WriteLine(strError);
          LoadAString(strError, IDSC_IMPORTWARNING2A);
          rpt->WriteLine(strError);
        }
        if (type == CItemData::ET_SHORTCUT)
          RemoveEntryAt(m_pwlist.find(entry_uuid)); // Can't keep invalid shortcut
        else
          pci_curitem->SetNormal(); // but can make invalid alias a normal entry

        num_warnings++;
      }
    }
  }
  return num_warnings;
}

void PWScore::ResetAllAliasPasswords(const uuid_array_t &base_uuid)
{
  // Alias ONLY - no shortcut version needed
  ItemMMapIter itr;
  ItemMMapIter lastElement;
  ItemListIter base_itr, alias_itr;
  uuid_array_t alias_uuid;
  StringX csBasePassword;

  itr = m_base2aliases_mmap.find(base_uuid);
  if (itr == m_base2aliases_mmap.end())
    return;

  base_itr = m_pwlist.find(base_uuid);
  if (base_itr != m_pwlist.end()) {
    csBasePassword = base_itr->second.GetPassword();
  } else {
    m_base2aliases_mmap.erase(base_uuid);
    return;
  }

  lastElement = m_base2aliases_mmap.upper_bound(base_uuid);

  for ( ; itr != lastElement; itr++) {
    itr->second.GetUUID(alias_uuid);
    alias_itr = m_pwlist.find(alias_uuid);
    if (alias_itr != m_pwlist.end()) {
      alias_itr->second.SetPassword(csBasePassword);
      alias_itr->second.SetNormal();
    }
  }
  m_base2aliases_mmap.erase(base_uuid);
}

void PWScore::GetAllDependentEntries(const uuid_array_t &base_uuid, UUIDList &tlist, const CItemData::EntryType type)
{
  ItemMMapIter itr;
  ItemMMapIter lastElement;
  uuid_array_t uuid;

  ItemMMap *pmmap;
  if (type == CItemData::ET_ALIAS)
    pmmap = &m_base2aliases_mmap;
  else if (type == CItemData::ET_SHORTCUT)
    pmmap = &m_base2shortcuts_mmap;
  else
    return;

  itr = pmmap->find(base_uuid);
  if (itr == pmmap->end())
    return;

  lastElement = pmmap->upper_bound(base_uuid);

  for ( ; itr != lastElement; itr++) {
    itr->second.GetUUID(uuid);
    tlist.push_back(uuid);
  }
}

bool PWScore::GetBaseEntry(const StringX &Password, GetBaseEntryPL &pl)
{
  // pl.ibasedata is:
  //  +n: password contains (n-1) colons and base entry found (n = 1, 2 or 3)
  //   0: password not in alias format
  //  -n: password contains (n-1) colons but either no base entry found or no unique entry found (n = 1, 2 or 3)

  // "bMultipleEntriesFound" is set if no "unique" base entry could be found and is only valid if n = -1 or -2.

  pl.bMultipleEntriesFound = false;
  memset(pl.base_uuid, 0x00, sizeof(uuid_array_t));

  // Take a copy of the Password field to do the counting!
  StringX passwd(Password);

  int num_colonsP1 = Replace(passwd, _T(':'), _T(';')) + 1;
  if ((Password[0] == _T('[')) &&
      (Password[Password.length() - 1] == _T(']')) &&
      num_colonsP1 <= 3) {
    StringX tmp;
    ItemListIter iter;
    switch (num_colonsP1) {
      case 1:
        // [X] - OK if unique entry [g:X:u], [g:X:], [:X:u] or [:X:] exists for any value of g or u
        pl.csPwdTitle = Password.substr(1, Password.length() - 2);  // Skip over '[' & ']'
        iter = GetUniqueBase(pl.csPwdTitle, pl.bMultipleEntriesFound);
        if (iter != m_pwlist.end()) {
          // Fill in the fields found during search
          pl.csPwdGroup = iter->second.GetGroup();
          pl.csPwdUser = iter->second.GetUser();
        }
        break;
      case 2:
        // [X:Y] - OK if unique entry [X:Y:u] or [g:X:Y] exists for any value of g or u
        pl.csPwdUser = _T("");
        tmp = Password.substr(1, Password.length() - 2);  // Skip over '[' & ']'
        pl.csPwdGroup = tmp.substr(0, tmp.find_first_of(_T(":")));
        pl.csPwdTitle = tmp.substr(pl.csPwdGroup.length() + 1);  // Skip over 'group:'
        iter = GetUniqueBase(pl.csPwdGroup, pl.csPwdTitle, pl.bMultipleEntriesFound);
        if (iter != m_pwlist.end()) {
          // Fill in the fields found during search
          pl.csPwdGroup = iter->second.GetGroup();
          pl.csPwdTitle = iter->second.GetTitle();
          pl.csPwdUser = iter->second.GetUser();
        }
        break;
      case 3:
        // [X:Y:Z], [X:Y:], [:Y:Z], [:Y:] (title cannot be empty)
        tmp = Password.substr(1, Password.length() - 2);  // Skip over '[' & ']'
        pl.csPwdGroup = tmp.substr(0, tmp.find_first_of(_T(":")));
        tmp = tmp.substr(pl.csPwdGroup.length() + 1);  // Skip over 'group:'
        pl.csPwdTitle = tmp.substr(0, tmp.find_first_of(_T(":")));    // Skip over 'title:'
        pl.csPwdUser = tmp.substr(pl.csPwdTitle.length() + 1);
        iter = Find(pl.csPwdGroup, pl.csPwdTitle, pl.csPwdUser);
        break;
      default:
        ASSERT(0);
    }
    if (iter != m_pwlist.end()) {
      pl.TargetType = iter->second.GetEntryType();
      if (pl.InputType == CItemData::ET_ALIAS && pl.TargetType == CItemData::ET_ALIAS) {
        // Check if base is already an alias, if so, set this entry -> real base entry
        uuid_array_t temp_uuid;
        iter->second.GetUUID(temp_uuid);
        GetAliasBaseUUID(temp_uuid, pl.base_uuid);
      } else {
        // This may not be a valid combination of source+target entries - sorted out by caller
        iter->second.GetUUID(pl.base_uuid);
      }
      // Valid and found
      pl.ibasedata = num_colonsP1;
      return true;
    }
    // Valid but either exact [g:t:u] not found or
    //  no or multiple entries satisfy [x] or [x:y]
    pl.ibasedata  = -num_colonsP1;
    return true;
  }
  pl.ibasedata = 0; // invalid password format for an alias
  return false;
}

bool PWScore::GetDependentEntryBaseUUID(const uuid_array_t &entry_uuid, 
                                        uuid_array_t &base_uuid, 
                                        const CItemData::EntryType type)
{
  memset(base_uuid, 0x00, sizeof(uuid_array_t));

  ItemMap *pmap;
  if (type == CItemData::ET_ALIAS)
    pmap = &m_alias2base_map;
  else if (type == CItemData::ET_SHORTCUT)
    pmap = &m_shortcut2base_map;
  else
    return false;

  ItemMapIter iter = pmap->find(entry_uuid);
  if (iter != pmap->end()) {
    iter->second.GetUUID(base_uuid);
    return true;
  } else {
    return false;
  }
}

bool PWScore::RegisterOnListModified(void (*pfcn) (LPARAM), LPARAM instance)
{
  if (m_pfcnNotifyListModified != NULL)
    return false;

  if ((pfcn == NULL) || !instance) {
    UnRegisterOnListModified();
    return false;
  }
  
  m_pfcnNotifyListModified = pfcn;
  m_NotifyListInstance = instance;
  m_bNotifyList = true;
  return true;
}

void PWScore::UnRegisterOnListModified()
{
  m_pfcnNotifyListModified = NULL;
  m_NotifyListInstance = NULL;
  m_bNotifyList = false;
}

void PWScore::NotifyListModified()
{
  if (m_bNotifyList)
    m_pfcnNotifyListModified(m_NotifyListInstance);
}

bool PWScore::RegisterOnDBModified(void (*pfcn) (LPARAM, bool), LPARAM instance)
{
  if (m_pfcnNotifyDBModified != NULL)
    return false;

  if (pfcn == NULL || !instance) {
    UnRegisterOnDBModified();
    return false;
  }

  m_pfcnNotifyDBModified = pfcn;
  m_NotifyDBInstance = instance;
  m_bNotifyDB = true;
  return true;
}

void PWScore::UnRegisterOnDBModified()
{
  m_pfcnNotifyDBModified = NULL;
  m_NotifyDBInstance = NULL;
  m_bNotifyDB = false;
}

void PWScore::NotifyDBModified()
{
  if (m_bNotifyDB)
    m_pfcnNotifyDBModified(m_NotifyDBInstance, m_bDBChanged || m_bDBPrefsChanged);
}

bool PWScore::LockFile(const stringT &filename, stringT &locker)
{
  return pws_os::LockFile(filename, locker,
                          m_lockFileHandle, m_LockCount);
}

bool PWScore::IsLockedFile(const stringT &filename) const
{
  return pws_os::IsLockedFile(filename);
}

void PWScore::UnlockFile(const stringT &filename)
{
  return pws_os::UnlockFile(filename, 
                            m_lockFileHandle, m_LockCount);
}

bool PWScore::LockFile2(const stringT &filename, stringT &locker)
{
  return pws_os::LockFile(filename, locker,
                          m_lockFileHandle2, m_LockCount2);
}

void PWScore::UnlockFile2(const stringT &filename)
{
  return pws_os::UnlockFile(filename, 
                            m_lockFileHandle2, m_LockCount2);
}

/*
void PWScore::MarkEntryForRemoval(CItemData *pci)
{
  m_bDBChanged = true;

  pci->SetStatus(CItemData::ES_DELETED);

  uuid_array_t uuid_del;
  pci->GetUUID(uuid_del);
  m_vdeleted.push_back(uuid_del);

  NotifyListModified();
  NotifyDBModified();
}
*/