/*
 * s3fs - FUSE-based file system backed by Amazon S3
 *
 * Copyright 2007-2013 Takeshi Nakatani <ggtakec.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <libgen.h>
#include <sys/stat.h>
#include <pwd.h>
#include <grp.h>
#include <syslog.h>

#include <string>
#include <sstream>
#include <map>
#include <list>

#include "common.h"
#include "s3fs_util.h"
#include "s3fs.h"

using namespace std;

//-------------------------------------------------------------------
// Global valiables
//-------------------------------------------------------------------
std::string mount_prefix   = "";

//-------------------------------------------------------------------
// Utility
//-------------------------------------------------------------------
string get_realpath(const char *path) {
  string realpath = mount_prefix;
  realpath += path;

  return realpath;
}

//-------------------------------------------------------------------
// Class S3ObjList
//-------------------------------------------------------------------
// New class S3ObjList is base on old s3_object struct.
// This class is for S3 compatible clients.
//
// If name is terminated by "/", it is forced dir type.
// If name is terminated by "_$folder$", it is forced dir type.
// If is_dir is true and name is not terminated by "/", the name is added "/".
//
bool S3ObjList::insert(const char* name, const char* etag, bool is_dir)
{
  if(!name || '\0' == name[0]){
    return false;
  }

  s3obj_t::iterator iter;
  string newname;
  string orgname = name;

  // Normalization
  string::size_type pos = orgname.find("_$folder$");
  if(string::npos != pos){
    newname = orgname.substr(0, pos);
    is_dir  = true;
  }else{
    newname = orgname;
  }
  if(is_dir){
    if('/' != newname[newname.length() - 1]){
      newname += "/";
    }
  }else{
    if('/' == newname[newname.length() - 1]){
      is_dir = true;
    }
  }

  // Check derived name object.
  if(is_dir){
    string chkname = newname.substr(0, newname.length() - 1);
    if(objects.end() != (iter = objects.find(chkname))){
      // found "dir" object --> remove it.
      objects.erase(iter);
    }
  }else{
    string chkname = newname + "/";
    if(objects.end() != (iter = objects.find(chkname))){
      // found "dir/" object --> not add new object.
      // and add normalization
      return insert_nomalized(orgname.c_str(), chkname.c_str(), true);
    }
  }

  // Add object
  if(objects.end() != (iter = objects.find(newname))){
    // Found same object --> update information.
    (*iter).second.normalname.erase();
    (*iter).second.orgname = orgname;
    (*iter).second.is_dir  = is_dir;
    if(etag){
      (*iter).second.etag = string(etag);  // over write
    }
  }else{
    // add new object
    s3obj_entry newobject;
    newobject.orgname = orgname;
    newobject.is_dir  = is_dir;
    if(etag){
      newobject.etag = etag;
    }
    objects[newname] = newobject;
  }

  // add normalization
  return insert_nomalized(orgname.c_str(), newname.c_str(), is_dir);
}

bool S3ObjList::insert_nomalized(const char* name, const char* normalized, bool is_dir)
{
  if(!name || '\0' == name[0] || !normalized || '\0' == normalized[0]){
    return false;
  }
  if(0 == strcmp(name, normalized)){
    return true;
  }

  s3obj_t::iterator iter;
  if(objects.end() != (iter = objects.find(name))){
    // found name --> over write
    (*iter).second.orgname.erase();
    (*iter).second.etag.erase();
    (*iter).second.normalname = normalized;
    (*iter).second.is_dir     = is_dir;
  }else{
    // not found --> add new object
    s3obj_entry newobject;
    newobject.normalname = normalized;
    newobject.is_dir     = is_dir;
    objects[name]        = newobject;
  }
  return true;
}

const s3obj_entry* S3ObjList::GetS3Obj(const char* name) const
{
  s3obj_t::const_iterator iter;

  if(!name || '\0' == name[0]){
    return NULL;
  }
  if(objects.end() == (iter = objects.find(name))){
    return NULL;
  }
  return &((*iter).second);
}

string S3ObjList::GetOrgName(const char* name) const
{
  const s3obj_entry* ps3obj;

  if(!name || '\0' == name[0]){
    return string("");
  }
  if(NULL == (ps3obj = GetS3Obj(name))){
    return string("");
  }
  return ps3obj->orgname;
}

string S3ObjList::GetNormalizedName(const char* name) const
{
  const s3obj_entry* ps3obj;

  if(!name || '\0' == name[0]){
    return string("");
  }
  if(NULL == (ps3obj = GetS3Obj(name))){
    return string("");
  }
  if(0 == (ps3obj->normalname).length()){
    return string(name);
  }
  return ps3obj->normalname;
}

string S3ObjList::GetETag(const char* name) const
{
  const s3obj_entry* ps3obj;

  if(!name || '\0' == name[0]){
    return string("");
  }
  if(NULL == (ps3obj = GetS3Obj(name))){
    return string("");
  }
  return ps3obj->etag;
}

bool S3ObjList::IsDir(const char* name) const
{
  const s3obj_entry* ps3obj;

  if(NULL == (ps3obj = GetS3Obj(name))){
    return false;
  }
  return ps3obj->is_dir;
}

bool S3ObjList::GetNameList(s3obj_list_t& list, bool OnlyNormalized, bool CutSlash) const
{
  s3obj_t::const_iterator iter;

  for(iter = objects.begin(); objects.end() != iter; iter++){
    if(OnlyNormalized && 0 != (*iter).second.normalname.length()){
      continue;
    }
    string name = (*iter).first;
    if(CutSlash && 1 < name.length() && '/' == name[name.length() - 1]){
      // only "/" string is skio this.
      name = name.substr(0, name.length() - 1);
    }
    list.push_back(name);
  }
  return true;
}

typedef std::map<std::string, bool> s3obj_h_t;

bool S3ObjList::MakeHierarchizedList(s3obj_list_t& list, bool haveSlash)
{
  s3obj_h_t h_map;
  s3obj_h_t::iterator hiter;
  s3obj_list_t::const_iterator liter;

  for(liter = list.begin(); list.end() != liter; liter++){
    string strtmp = (*liter);
    if(1 < strtmp.length() && '/' == strtmp[strtmp.length() - 1]){
      strtmp = strtmp.substr(0, strtmp.length() - 1);
    }
    h_map[strtmp] = true;

    // check hierarchized directory
    for(string::size_type pos = strtmp.find_last_of("/"); string::npos != pos; pos = strtmp.find_last_of("/")){
      strtmp = strtmp.substr(0, pos);
      if(0 == strtmp.length() || "/" == strtmp){
        break;
      }
      if(h_map.end() == h_map.find(strtmp)){
        // not found
        h_map[strtmp] = false;
      }
    }
  }

  // check map and add lost hierarchized directory.
  for(hiter = h_map.begin(); hiter != h_map.end(); ++hiter){
    if(false == (*hiter).second){
      // add hierarchized directory.
      string strtmp = (*hiter).first;
      if(haveSlash){
        strtmp += "/";
      }
      list.push_back(strtmp);
    }
  }
  return true;
}

//-------------------------------------------------------------------
// Utility functions for moving objects
//-------------------------------------------------------------------
MVNODE *create_mvnode(const char *old_path, const char *new_path, bool is_dir, bool normdir)
{
  MVNODE *p;
  char *p_old_path;
  char *p_new_path;

  p = (MVNODE *) malloc(sizeof(MVNODE));
  if (p == NULL) {
    printf("create_mvnode: could not allocation memory for p\n");
    S3FS_FUSE_EXIT();
    return NULL;
  }

  if(NULL == (p_old_path = strdup(old_path))){
    free(p);
    printf("create_mvnode: could not allocation memory for p_old_path\n");
    S3FS_FUSE_EXIT();
    return NULL;
  }

  if(NULL == (p_new_path = strdup(new_path))){ 
    free(p);
    free(p_old_path);
    printf("create_mvnode: could not allocation memory for p_new_path\n");
    S3FS_FUSE_EXIT();
    return NULL;
  }

  p->old_path   = p_old_path;
  p->new_path   = p_new_path;
  p->is_dir     = is_dir;
  p->is_normdir = normdir;
  p->prev = NULL;
  p->next = NULL;
  return p;
}

//
// Add sorted MVNODE data(Ascending order) 
//
MVNODE *add_mvnode(MVNODE** head, MVNODE** tail, const char *old_path, const char *new_path, bool is_dir, bool normdir)
{
  if(!head || !tail){
    return NULL;
  }

  MVNODE* cur;
  MVNODE* mvnew;
  for(cur = *head; cur; cur = cur->next){
    if(cur->is_dir == is_dir){
      int nResult = strcmp(cur->old_path, old_path);
      if(0 == nResult){
        // Found same old_path.
        return cur;

      }else if(0 > nResult){
        // next check.
        // ex: cur("abc"), mvnew("abcd")
        // ex: cur("abc"), mvnew("abd")
        continue;

      }else{
        // Add into before cur-pos.
        // ex: cur("abc"), mvnew("ab")
        // ex: cur("abc"), mvnew("abb")
        if(NULL == (mvnew = create_mvnode(old_path, new_path, is_dir, normdir))){
          return NULL;
        }
        if(cur->prev){
          (cur->prev)->next = mvnew;
        }else{
          *head = mvnew;
        }
        mvnew->prev = cur->prev;
        mvnew->next = cur;
        cur->prev = mvnew;

        return mvnew;
      }
    }
  }
  // Add into tail.
  if(NULL == (mvnew = create_mvnode(old_path, new_path, is_dir, normdir))){
    return NULL;
  }
  mvnew->prev = (*tail);
  if(*tail){
    (*tail)->next = mvnew;
  }
  (*tail) = mvnew;
  if(!(*head)){
    (*head) = mvnew;
  }
  return mvnew;
}

void free_mvnodes(MVNODE *head)
{
  MVNODE *my_head;
  MVNODE *next;

  for(my_head = head, next = NULL; my_head; my_head = next){
    next = my_head->next;
    free(my_head->old_path);
    free(my_head->new_path);
    free(my_head);
  }
  return;
}

//-------------------------------------------------------------------
// Utility for UID/GID
//-------------------------------------------------------------------
// get user name from uid
string get_username(uid_t uid)
{
  struct passwd* ppw;
  if(NULL == (ppw = getpwuid(uid)) || NULL == ppw->pw_name){
    FGPRINT("    could not get username(errno=%d).\n", (int)errno);
    SYSLOGDBG("could not get username(errno=%d).\n", (int)errno);
    return string("");
  }
  return string(ppw->pw_name);
}

// check uid in group(gid)
int is_uid_inculde_group(uid_t uid, gid_t gid)
{
  static size_t maxlen = 0;	// set onece
  int result;
  char* pbuf;
  struct group ginfo;
  struct group* pginfo = NULL;

  // make buffer
  if(0 == maxlen){
    if(0 > (maxlen = (size_t)sysconf(_SC_GETGR_R_SIZE_MAX))){
      FGPRINT("    could not get max name length.\n");
      SYSLOGDBG("could not get max name length.\n");
      maxlen = 0;
      return -ERANGE;
    }
  }
  if(NULL == (pbuf = (char*)malloc(sizeof(char) * maxlen))){
    FGPRINT("    failed to allocate memory.\n");
    SYSLOGERR("failed to allocate memory.\n");
    return -ENOMEM;
  }
  // get group infomation
  if(0 != (result = getgrgid_r(gid, &ginfo, pbuf, maxlen, &pginfo))){
    FGPRINT("    could not get group infomation.\n");
    SYSLOGDBG("could not get group infomation.\n");
    free(pbuf);
    return -result;
  }

  // check group
  if(NULL == pginfo){
    // there is not gid in group.
    free(pbuf);
    return -EINVAL;
  }

  string username = get_username(uid);

  char** ppgr_mem;
  for(ppgr_mem = pginfo->gr_mem; ppgr_mem && *ppgr_mem; ppgr_mem++){
    if(username == *ppgr_mem){
      // Found username in group.
      free(pbuf);
      return 1;
    }
  }
  free(pbuf);
  return 0;
}

//-------------------------------------------------------------------
// Utility for file and directory
//-------------------------------------------------------------------
// safe variant of dirname
// dirname clobbers path so let it operate on a tmp copy
string mydirname(string path)
{
  return string(dirname(&path[0]));
}

// safe variant of basename
// basename clobbers path so let it operate on a tmp copy
string mybasename(string path)
{
  return string(basename(&path[0]));
}

// mkdir --parents
int mkdirp(const string& path, mode_t mode)
{
  string base;
  string component;
  stringstream ss(path);
  while (getline(ss, component, '/')) {
    base += "/" + component;
    mkdir(base.c_str(), mode);
  }
  return 0;
}

//-------------------------------------------------------------------
// Utility functions for convert
//-------------------------------------------------------------------
time_t get_mtime(const char *s)
{
  return (time_t) strtoul(s, (char **) NULL, 10);
}

time_t get_mtime(headers_t& meta, bool overcheck)
{
  headers_t::const_iterator iter;
  if(meta.end() == (iter = meta.find("x-amz-meta-mtime"))){
    if(overcheck){
      return get_lastmodified(meta);
    }
    return 0;
  }
  return get_mtime((*iter).second.c_str());
}

off_t get_size(const char *s)
{
  return (off_t) strtoul(s, (char **) NULL, 10);
}

off_t get_size(headers_t& meta)
{
  headers_t::const_iterator iter;
  if(meta.end() == (iter = meta.find("Content-Length"))){
    return 0;
  }
  return get_size((*iter).second.c_str());
}

mode_t get_mode(const char *s)
{
  return (mode_t) strtoul(s, (char **) NULL, 10);
}

mode_t get_mode(headers_t& meta, const char* path, bool checkdir, bool forcedir)
{
  mode_t mode = 0;
  bool isS3sync = false;
  headers_t::const_iterator iter;

  if(meta.end() != (iter = meta.find("x-amz-meta-mode"))){
    mode = get_mode((*iter).second.c_str());
  }else{
    if(meta.end() != (iter = meta.find("x-amz-meta-permissions"))){ // for s3sync
      mode = get_mode((*iter).second.c_str());
      isS3sync = true;
    }
  }
  if(!isS3sync){
    if(checkdir){
      if(forcedir){
        mode |= S_IFDIR;
      }else{
        if(meta.end() != (iter = meta.find("Content-Type"))){
          string strConType = (*iter).second;
          if(strConType == "application/x-directory"){
            mode |= S_IFDIR;
          }else if(path && 0 < strlen(path) && '/' == path[strlen(path) - 1]){
            if(strConType == "binary/octet-stream" || strConType == "application/octet-stream"){
              mode |= S_IFDIR;
            }else{
              mode |= S_IFREG;
            }
          }else{
            mode |= S_IFREG;
          }
        }else{
          mode |= S_IFREG;
        }
      }
    }
  }else{
    if(!checkdir){
      // cut dir/reg flag.
      mode &= ~S_IFDIR;
      mode &= ~S_IFREG;
    }
  }
  return mode;
}

uid_t get_uid(const char *s)
{
  return (uid_t) strtoul(s, (char **) NULL, 10);
}

uid_t get_uid(headers_t& meta)
{
  headers_t::const_iterator iter;
  if(meta.end() == (iter = meta.find("x-amz-meta-uid"))){
    if(meta.end() == (iter = meta.find("x-amz-meta-owner"))){ // for s3sync
      return 0;
    }
  }
  return get_uid((*iter).second.c_str());
}

gid_t get_gid(const char *s)
{
  return (gid_t) strtoul(s, (char **) NULL, 10);
}

gid_t get_gid(headers_t& meta)
{
  headers_t::const_iterator iter;
  if(meta.end() == (iter = meta.find("x-amz-meta-gid"))){
    if(meta.end() == (iter = meta.find("x-amz-meta-group"))){ // for s3sync
      return 0;
    }
  }
  return get_gid((*iter).second.c_str());
}

blkcnt_t get_blocks(off_t size)
{
  return size / 512 + 1;
}

time_t get_lastmodified(const char* s)
{
  struct tm tm;
  if(!s){
    return 0L;
  }
  strptime(s, "%a, %d %b %Y %H:%M:%S %Z", &tm);
  return mktime(&tm);      // GMT
}

time_t get_lastmodified(headers_t& meta)
{
  headers_t::const_iterator iter;
  if(meta.end() == (iter = meta.find("Last-Modified"))){
    return 0;
  }
  return get_lastmodified((*iter).second.c_str());
}

//-------------------------------------------------------------------
// Help
//-------------------------------------------------------------------
void show_usage (void)
{
  printf("Usage: %s BUCKET:[PATH] MOUNTPOINT [OPTION]...\n",
    program_name.c_str());
}

void show_help (void)
{
  show_usage();
  printf( 
    "\n"
    "Mount an Amazon S3 bucket as a file system.\n"
    "\n"
    "   General forms for s3fs and FUSE/mount options:\n"
    "      -o opt[,opt...]\n"
    "      -o opt [-o opt] ...\n"
    "\n"
    "s3fs Options:\n"
    "\n"
    "   Most s3fs options are given in the form where \"opt\" is:\n"
    "\n"
    "             <option_name>=<option_value>\n"
    "\n"
    "   default_acl (default=\"private\")\n"
    "     - the default canned acl to apply to all written s3 objects\n"
    "          see http://aws.amazon.com/documentation/s3/ for the \n"
    "          full list of canned acls\n"
    "\n"
    "   retries (default=\"2\")\n"
    "      - number of times to retry a failed s3 transaction\n"
    "\n"
    "   use_cache (default=\"\" which means disabled)\n"
    "      - local folder to use for local file cache\n"
    "\n"
    "   use_rrs (default=\"\" which means diabled)\n"
    "      - use Amazon's Reduced Redundancy Storage when set to 1\n"
    "\n"
    "   use_sse (default=\"\" which means diabled)\n"
    "      - use Amazon's Server Site Encryption when set to 1\n"
    "\n"
    "   public_bucket (default=\"\" which means disabled)\n"
    "      - anonymously mount a public bucket when set to 1\n"
    "\n"
    "   passwd_file (default=\"\")\n"
    "      - specify which s3fs password file to use\n"
    "\n"
    "   connect_timeout (default=\"10\" seconds)\n"
    "      - time to wait for connection before giving up\n"
    "\n"
    "   readwrite_timeout (default=\"30\" seconds)\n"
    "      - time to wait between read/write activity before giving up\n"
    "\n"
    "   max_stat_cache_size (default=\"10000\" entries (about 4MB))\n"
    "      - maximum number of entries in the stat cache\n"
    "\n"
    "   stat_cache_expire (default is no expire)\n"
    "      - specify expire time(seconds) for entries in the stat cache.\n"
    "\n"
    "   enable_noobj_cache (default is disable)\n"
    "      - enable cache entries for the object which does not exist.\n"
    "      s3fs always has to check whether file(or sub directory) exists \n"
    "      under object(path) when s3fs does some command, since s3fs has \n"
    "      recognized a directory which does not exist and has files or \n"
    "      sub directories under itself. It increases ListBucket request \n"
    "      and makes performance bad.\n"
    "      You can specify this option for performance, s3fs memorizes \n"
    "      in stat cache that the object(file or directory) does not exist.\n"
    "\n"
    "   nodnscache - disable dns cache\n"
    "      - s3fs is always using dns cache, this option make dns cache disable.\n"
    "\n"
    "   url (default=\"http://s3.amazonaws.com\")\n"
    "      - sets the url to use to access amazon s3\n"
    "\n"
    "   nomultipart - disable multipart uploads\n"
    "\n"
    "   enable_content_md5 (default is disable)\n"
    "      - verifying uploaded object without multipart by content-md5 header.\n"
    "\n"
    "   noxmlns - disable registing xml name space.\n"
    "        disable registing xml name space for response of \n"
    "        ListBucketResult and ListVersionsResult etc. Default name \n"
    "        space is looked up from \"http://s3.amazonaws.com/doc/2006-03-01\".\n"
    "        This option should not be specified now, because s3fs looks up\n"
    "        xmlns automatically after v1.66.\n"
    "\n"
    "   nocopyapi - for other incomplete compatibility object storage.\n"
    "        For a distributed object storage which is compatibility S3\n"
    "        API without PUT(copy api).\n"
    "        If you set this option, s3fs do not use PUT with \n"
    "        \"x-amz-copy-source\"(copy api). Because traffic is increased\n"
    "        2-3 times by this option, we do not recommend this.\n"
    "\n"
    "   norenameapi - for other incomplete compatibility object storage.\n"
    "        For a distributed object storage which is compatibility S3\n"
    "        API without PUT(copy api).\n"
    "        This option is a subset of nocopyapi option. The nocopyapi\n"
    "        option does not use copy-api for all command(ex. chmod, chown,\n"
    "        touch, mv, etc), but this option does not use copy-api for\n"
    "        only rename command(ex. mv). If this option is specified with\n"
    "        nocopapi, the s3fs ignores it.\n"
    "\n"
    "FUSE/mount Options:\n"
    "\n"
    "   Most of the generic mount options described in 'man mount' are\n"
    "   supported (ro, rw, suid, nosuid, dev, nodev, exec, noexec, atime,\n"
    "   noatime, sync async, dirsync).  Filesystems are mounted with\n"
    "   '-onodev,nosuid' by default, which can only be overridden by a\n"
    "   privileged user.\n"
    "   \n"
    "   There are many FUSE specific mount options that can be specified.\n"
    "   e.g. allow_other  See the FUSE's README for the full set.\n"
    "\n"
    "Miscellaneous Options:\n"
    "\n"
    " -h, --help        Output this help.\n"
    "     --version     Output version info.\n"
    " -d  --debug       Turn on DEBUG messages to syslog. Specifying -d\n"
    "                   twice turns on FUSE debug messages to STDOUT.\n"
    " -f                FUSE foreground option - do not run as daemon.\n"
    " -s                FUSE singlethread option\n"
    "                   disable multi-threaded operation\n"
    "\n"
    "\n"
    "Report bugs to <s3fs-devel@googlegroups.com>\n"
    "s3fs home page: <http://code.google.com/p/s3fs/>\n"
  );
  return;
}

void show_version(void)
{
  printf(
  "Amazon Simple Storage Service File System %s\n"
  "Copyright (C) 2010 Randy Rizun <rrizun@gmail.com>\n"
  "License GPL2: GNU GPL version 2 <http://gnu.org/licenses/gpl.html>\n"
  "This is free software: you are free to change and redistribute it.\n"
  "There is NO WARRANTY, to the extent permitted by law.\n", VERSION );
  return;
}

