/*  -*- Mode: C; -*-                                                         */
/*                                                                           */
/* librosie.c    Expose the Rosie API                                        */
/*                                                                           */
/*  © Copyright IBM Corporation 2016, 2017                                   */
/*  LICENSE: MIT License (https://opensource.org/licenses/mit-license.html)  */
/*  AUTHOR: Jamie A. Jennings                                                */

/* Protocol:
 * 
 * rosie_new() makes a new engine.  Every thread must have its own engine.
 * rosie_finalize() destroys an engine and frees its memory.
 *
 * Most functions have an argument 'str *messages':
 *
 * (1) If messages->ptr is NULL after the call, then there were no
 *     messages.
 * (2) If the return code from the call is non-zero, then the code
 *     will indicate the kind of error, and there MAY be a
 *     human-readable string explaining the error in messages.
 * (3) If the return code is zero, indicating success, there MAY be a
 *     JSON-encoded structure in messages.
 * (4) If messages->ptr is not NULL, then the caller must free
 *     messages when its value is no longer needed.
 *
*/

/* FUTURE: 
 * 
 * - Add a function that unloads all the dynamic libs, erases the
 *   global information about the libs, and reinitializes the
 *   ready_to_boot lock.
 * 
 *   - Probably need a list of engines so we can destroy those before
 *     unloading and resetting?  This could be tricky to get right,
 *     since a client could hold an engine pointer.  However, if we
 *     keep an array of extant engines, we can validate the engine
 *     passed into librosie by the client on each call.  
 *
 *   - Maybe we give the client a small integer to identify an engine,
 *     instead of a pointer.  The cost would be one array index
 *     operation per call to librosie, to ensure the engine is live.
 *     Yes, this is a good idea.
 */

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "librosie.h"

#include <dlfcn.h>
#include <libgen.h>

typedef void *(*func_ptr_t)();
int (*fp_r_match_C)();		/* defined in lptree.c */ 
typedef rBuffer* (*foo_t)(lua_State *L, char *data, size_t len);
foo_t fp_r_newbuffer_wrap;	/* defined in rbuf.c */ 


/* ----------------------------------------------------------------------------------------
 * Paths relative to where librosie.so is found (for example):
 *  /usr/local/lib/librosie.so => 
 *    libname = librosie.so
 *    dirname = /usr/local/lib
 *    rosiehomedir = /usr/local/lib/rosie
 *    bootscript = /usr/local/lib/rosie/lib/boot.luac
 * ----------------------------------------------------------------------------------------
 */

#define ROSIEHOME "/rosie"
#define BOOTSCRIPT "/lib/boot.luac"
#define RPEG_LOCATION "/lib/lpeg.so"
#define LIBLUA_LOCATION "/lib/liblua.5.3.so"

static char libname[MAXPATHLEN];
static char libdir[MAXPATHLEN];
static char rosiehomedir[MAXPATHLEN];
static char bootscript[MAXPATHLEN];

char rpeg_path[MAXPATHLEN];
void *rpeg_lib;
char liblua_path[MAXPATHLEN];
void *lua_lib;

#include "logging.c"
#include "registry.c"
#include "rosiestring.c"

/* Symbol visibility in the final library */
#define EXPORT __attribute__ ((visibility("default")))

/* ----------------------------------------------------------------------------------------
 * Engine locks
 * ----------------------------------------------------------------------------------------
 */

#define ACQUIRE_ENGINE_LOCK(e) do {				    \
    int r = pthread_mutex_lock(&((e)->lock));			    \
    if (r) {                                                        \
        fprintf(stderr, "pthread_mutex_lock failed with %d\n", r);  \
        abort();                                                    \
    }                                                               \
} while (0)

#define RELEASE_ENGINE_LOCK(e) do {				    \
    int r = pthread_mutex_unlock(&((e)->lock));			    \
    if (r) {                                                        \
        fprintf(stderr, "pthread_mutex_unlock failed with %d\n", r);\
        abort();                                                    \
    }                                                               \
} while (0)

/* ----------------------------------------------------------------------------------------
 * Start-up / boot functions
 * ----------------------------------------------------------------------------------------
 */

static void set_libinfo() {
  Dl_info dl;
  char *base, *dir;
  char buf[MAXPATHLEN];
  int ok = dladdr((void *)rosie_new, &dl);
  if (!ok) {
    display("librosie: call to dladdr failed");
    exit(ERR_SYSCALL_FAILED);
  }
  LOGf("dli_fname is %s\n", dl.dli_fname);
  /* basename and dirname may MODIFY the string you pass to them. arghh. */
  strncpy(buf, dl.dli_fname, MAXPATHLEN);
  base = basename(buf);
  strncpy(buf, dl.dli_fname, MAXPATHLEN);
  dir = dirname(buf);
  if (!base || !dir) {
    display("librosie: call to basename/dirname failed");
    exit(ERR_SYSCALL_FAILED);
  }
  /* basename and dirname return pointers to temporary internal storage. arghh. */
  strncpy(libname, base, MAXPATHLEN);
  strncpy(libdir, dir, MAXPATHLEN);
  LOGf("libdir is %s, and libname is %s\n", libdir, libname);
}

static void set_bootscript() {
  size_t len;
  static char *last;
  /* set rosiehomedir using libinfo */
  len = strnlen(ROSIEHOME, MAXPATHLEN);
  last = stpncpy(rosiehomedir, libdir, (MAXPATHLEN - len - 1));
  last = stpncpy(last, ROSIEHOME, len);
  *last = '\0';
  /* set absolute path to boot script */
  len = strnlen(BOOTSCRIPT, MAXPATHLEN);
  last = stpncpy(bootscript, rosiehomedir, (MAXPATHLEN - len - 1));
  last = stpncpy(last, BOOTSCRIPT, len);
  *last = '\0';
  assert((last-bootscript) < MAXPATHLEN);
  LOGf("Bootscript filename set to %s\n", bootscript);
}

static int encoder_name_to_code(const char *name) {
  const r_encoder_t *entry = r_encoders;
  while (entry->name) {
    if (!strncmp(name, entry->name, MAX_ENCODER_NAME_LENGTH)) return entry->code;
    entry++;
  }
  return 0;
}

static void prepare_for_boot() {
  /* boot incoming */

  char *next = stpncpy(rpeg_path, rosiehomedir, MAXPATHLEN); 
  if ((MAXPATHLEN - (unsigned int)(next - rpeg_path + 1)) < strlen(RPEG_LOCATION)) {
    LOG("rpeg_path exceeds MAXPATHLEN\n");
    return;
  }
  strncpy(next, RPEG_LOCATION, (MAXPATHLEN - (next - rpeg_path + 1)));
  LOGf("rpeg path (calculated) is %s\n", rpeg_path);
  
/*   rpeg_lib = dlopen(rpeg_path, RTLD_NOW); /\* reopen to get handle *\/ */
/*   if (rpeg_lib == NULL) { */
/*     LOG("dlopen(rpeg) returned NULL: unable to reopen rpeg library\n"); */
/*     return; */
/*   } */

/*   char *msg = NULL; */

  /* TEMPORARY */
  fp_r_match_C = &r_match_C;
  fp_r_newbuffer_wrap = &r_newbuffer_wrap;

/*   fp_r_match_C = dlsym(rpeg_lib, "r_match_C"); */
/*   if ((msg = dlerror()) != NULL) LOGf("r_match_C dlerror = %s\n", msg); */

/*   fp_r_newbuffer_wrap = (foo_t) dlsym(rpeg_lib, "r_newbuffer_wrap"); */
/*   if ((msg = dlerror()) != NULL) LOGf("r_newbuffer_wrap dlerror = %s\n", msg); */

/*   if ((fp_r_match_C == NULL) || (fp_r_newbuffer_wrap == NULL)) { */
/*     LOG("Failed to find rpeg functions\n"); */
/*     return; */
/*   } */
}  

static pthread_once_t initialized = PTHREAD_ONCE_INIT;
static int all_is_lost = TRUE;

static void initialize() {
  LOG("INITIALIZE start\n");
  set_libinfo();
  set_bootscript();
  char *next = stpncpy(liblua_path, rosiehomedir, MAXPATHLEN); 
  if ((MAXPATHLEN - (unsigned int)(next - liblua_path + 1)) < strlen(LIBLUA_LOCATION)) {
    LOG("liblua_path exceeds MAXPATHLEN\n");
    return;
  }
  strncpy(next, LIBLUA_LOCATION, (MAXPATHLEN - (next - liblua_path + 1)));
  LOGf("liblua path (calculated) is %s\n", liblua_path);
/*   lua_lib = dlopen(liblua_path, RTLD_NOW | RTLD_GLOBAL); */
/*   if (lua_lib == NULL) { */
/*     LOGf("dlopen(liblua) returned NULL: unable to open %s\n", liblua_path); */
/*     return; */
/*   } */
  /* TEMPORARY: */
  lua_lib = &lua_lib;		/* any pointer */
  
  prepare_for_boot();
  all_is_lost = FALSE;
  LOG("INITIALIZE finish\n");
  return;
}

static int boot(lua_State *L, str *messages) {
  char *msg = NULL;
  if (!*bootscript) {
    *messages = rosie_new_string_from_const("failed to set bootscript or libinfo");
    return FALSE;
  }
  LOGf("Booting rosie from %s\n", bootscript);

  int status = luaL_loadfile(L, bootscript);
  if (status != LUA_OK) {
    LOG("Failed to read boot code (using loadfile)\n");
    if (asprintf(&msg, "missing or corrupt rosie boot loader %s", bootscript)) {
      *messages = rosie_string_from((byte_ptr) msg, strlen(msg));
    }
    else {
      *messages = rosie_new_string_from_const("cannot find rosie boot code");
    }
    return FALSE;
  }
  LOG("Reading of boot code succeeded (using loadfile)\n");
  status = lua_pcall(L, 0, LUA_MULTRET, 0);
  if (status != LUA_OK) {
    LOG("Loading of boot code failed\n");
    if (asprintf(&msg, "loading failed for %s", bootscript)) {
      *messages = rosie_string_from((byte_ptr) msg, strlen(msg));
    }
    else {
      *messages = rosie_new_string_from_const("loading of boot code failed");
    }
    return FALSE;
  }
  LOG("Loading of boot code succeeded\n");
  lua_pushlstring(L, (const char *)rosiehomedir, strnlen(rosiehomedir, MAXPATHLEN));
  status = lua_pcall(L, 1, LUA_MULTRET, 0);
  if (status!=LUA_OK) {
    LOG("Boot function failed.  Lua stack is: \n");
    LOGstack(L);
    *messages = rosie_new_string_from_const("execution of boot loader failed");
    return FALSE;
  }
  LOG("Boot function succeeded\n");

  return TRUE;
}

/* FUTURE: Return any errors from the json encoder to the client? */
static int to_json_string(lua_State *L, int pos, str *json_string) {
     size_t len;
     byte_ptr str;
     int t;
     int top = lua_gettop(L);
     get_registry(json_encoder_key);
     lua_pushvalue(L, pos-1);                /* offset because we pushed json_encoder */
     *json_string = rosie_string_from(NULL, 0);
     if (!lua_istable(L, pos)) return ERR_SYSCALL_FAILED;
     /* When the messages table is empty, be sure to return a null rosie_string */
     lua_pushnil(L);
     if (!lua_next(L, pos-1)) {
       return LUA_OK;
     } else {
       lua_pop(L, 2);
     }
     t = lua_pcall(L, 1, LUA_MULTRET, 0);
     if (t != LUA_OK) {
       LOG("call to json encoder failed\n"); /* more detail may not be useful to the user */
       LOGstack(L);
       return ERR_SYSCALL_FAILED;
     }
     if ((lua_gettop(L) - top) > 1) {
       /* Top of stack is error msg */
       LOG("call to json encoder returned more than one value\n");
       if (lua_isstring(L, -1) && lua_isnil(L, -2)) {
	 LOGf("error message from json encoder: %s\n", lua_tolstring(L, -1, NULL));
	 LOGstack(L);
	 return ERR_SYSCALL_FAILED;
       }
       else {
	 /* Something really strange happened!  Is there any useful info to return? */
	 LOG("call to json encoder returned unexpected values\n");
	 LOGstack(L);
	 return ERR_SYSCALL_FAILED;
       }
     }
     str = (byte_ptr) lua_tolstring(L, -1, &len);
     *json_string = rosie_new_string(str, len);
     return LUA_OK;
}

static int strip_violation_messages(lua_State *L) {
  int t;

  get_registry(violation_strip_key);

  /* Now have this stack: strip_each(), messages[], ...
     And violation.strip_each() mutates its argument.
  */
  lua_pushvalue(L, -2);		/* push copy of messages table */
  t = lua_pcall(L, 1, 1, 0);	/* violation.strip_each() */
  if (t != LUA_OK) { 
    LOG("violation.strip_each() failed\n"); 
    LOGstack(L);
    return ERR_ENGINE_CALL_FAILED; 
  } 
  return LUA_OK;
}


int luaopen_lpeg (lua_State *L);
int luaopen_cjson_safe(lua_State *l);

static lua_State *newstate() {
  lua_State *newL = luaL_newstate();
  luaL_checkversion(newL); /* Ensures several critical things needed to use Lua */
  luaL_openlibs(newL);     /* Open lua's standard libraries */
  luaL_requiref(newL, "lpeg", luaopen_lpeg, 0);
  luaL_requiref(newL, "cjson.safe", luaopen_cjson_safe, 0);
  return newL;
}
  

/* ----------------------------------------------------------------------------------------
 * Exported functions
 * ----------------------------------------------------------------------------------------
 */

EXPORT
Engine *rosie_new(str *messages) {

  pthread_once(&initialized, initialize);
  if (all_is_lost) {
    *messages = rosie_new_string_from_const("initialization failed; enable DEBUG output for details");
    return NULL;
  }

  int t;
  Engine *e = malloc(sizeof(Engine));
  lua_State *L = newstate();
  if (L == NULL) {
    *messages = rosie_new_string_from_const("not enough memory to initialize");
    return NULL;
  }

  if (!boot(L, messages)) {
    return NULL;		/* messages already set by boot */
  }
  if (!lua_checkstack(L, 6)) {
    LOG("Cannot initialize: not enough memory for stack\n");
    *messages = rosie_new_string_from_const("not enough memory for stack");
    return NULL;
  }
  t = lua_getglobal(L, "rosie");
  CHECK_TYPE("rosie", t, LUA_TTABLE);
  set_registry(rosie_key);
  
  t = lua_getfield(L, -1, "engine");
  CHECK_TYPE("engine", t, LUA_TTABLE);
  t = lua_getfield(L, -1, "new");
  CHECK_TYPE("engine.new", t, LUA_TFUNCTION);
  t = lua_pcall(L, 0, 1, 0);
  if (t != LUA_OK) {
    LOG("rosie.engine.new() failed\n");
    *messages = rosie_new_string_from_const("rosie.engine.new() failed");
    return NULL;
  }

  /* Engine instance is at top of stack */
  set_registry(engine_key);
  t = lua_getfield(L, -1, "match");
  CHECK_TYPE("engine.match", t, LUA_TFUNCTION);
  set_registry(engine_match_key);

  lua_createtable(L, INITIAL_RPLX_SLOTS, 0);
  set_registry(rplx_table_key);

  lua_getglobal(L, "rosie");
  t = lua_getfield(L, -1, "env");
  CHECK_TYPE("rosie.env", t, LUA_TTABLE);
  t = lua_getfield(L, -1, "cjson");
  CHECK_TYPE("rosie.env.cjson", t, LUA_TTABLE);
  t = lua_getfield(L, -1, "encode");
  CHECK_TYPE("rosie.env.cjson.encode", t, LUA_TFUNCTION);
  set_registry(json_encoder_key);

  lua_getglobal(L, "rosie");
  t = lua_getfield(L, -1, "env");
  CHECK_TYPE("rosie.env", t, LUA_TTABLE);
  t = lua_getfield(L, -1, "violation");
  CHECK_TYPE("rosie.env.violation", t, LUA_TTABLE);
  t = lua_getfield(L, -1, "strip_each");
  CHECK_TYPE("rosie.env.violation.strip_each", t, LUA_TFUNCTION);
  set_registry(violation_strip_key);

  lua_pushinteger(L, 0);
  set_registry(alloc_set_limit_key);

  pthread_mutex_init(&(e->lock), NULL);
  e->L = L;

  lua_settop(L, 0);
  LOGf("Engine %p created\n", e);
  return e;
}
     
/* newlimit of -1 means query for current limit */
EXPORT
int rosie_alloc_limit (Engine *e, int *newlimit, int *usage) {
  int memusg, actual_limit;
  lua_State *L = e->L;
  LOGf("rosie_alloc_limit() called with int pointers %p, %p\n", newlimit, usage);
  ACQUIRE_ENGINE_LOCK(e);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);        /* second time to free resources marked for finalization */
  memusg = lua_gc(L, LUA_GCCOUNT, 0); /* KB */
  if (usage) *usage = memusg;
  if (newlimit) {
    int limit = *newlimit;
    if ((limit != -1) && (limit != 0) && (limit < MIN_ALLOC_LIMIT_MB)) {
      RELEASE_ENGINE_LOCK(e);
      return ERR_ENGINE_CALL_FAILED;
    } 
    if (limit == -1) {
      /* query */
      get_registry(alloc_set_limit_key);
      *newlimit = lua_tointeger(L, -1);
    } else {
      /* set new limit */
      lua_pushinteger(L, limit);
      set_registry(alloc_set_limit_key);
      actual_limit = memusg + limit;
      lua_pushinteger(L, (limit == 0) ? 0 : actual_limit);
      set_registry(alloc_actual_limit_key);
      if (limit == 0) {
	LOGf("set alloc limit to UNLIMITED above current usage level of %0.1f MB\n", memusg/1024.0);
      } else {
	LOGf("set alloc limit to %d MB above current usage level of %0.1f MB\n", *newlimit, memusg/1024.0);
      }
    }
  }
  lua_settop(L, 0);
  RELEASE_ENGINE_LOCK(e);
  return SUCCESS;
}

/* N.B. Client must free retval */
EXPORT
int rosie_config(Engine *e, str *retval) {
  int t;
  str r;
  lua_State *L = e->L;
  ACQUIRE_ENGINE_LOCK(e);
  get_registry(rosie_key);
  t = lua_getfield(L, -1, "config");
  CHECK_TYPE("config", t, LUA_TFUNCTION);
  t = lua_pcall(L, 0, 1, 0);
  if (t != LUA_OK) {
    LOG("rosie.config() failed\n");
    *retval = rosie_new_string_from_const("rosie.config() failed");
    lua_settop(L, 0);
    RELEASE_ENGINE_LOCK(e);
    return ERR_ENGINE_CALL_FAILED;
  }
  t = to_json_string(L, -1, &r);
  if (t != LUA_OK) {
    LOG("in config(), could not convert config information to json\n");
    *retval = rosie_new_string_from_const("in config(), could not convert config information to json");
    lua_settop(L, 0);
    RELEASE_ENGINE_LOCK(e);
    return ERR_ENGINE_CALL_FAILED;
  }
  retval->len = r.len;
  retval->ptr = r.ptr;
  lua_settop(L, 0);
  RELEASE_ENGINE_LOCK(e);
  return SUCCESS;
}

EXPORT
int rosie_libpath(Engine *e, str *newpath) {
  int t;
  lua_State *L = e->L;
  ACQUIRE_ENGINE_LOCK(e);
  get_registry(engine_key);
  if (newpath->ptr) {
    t = lua_getfield(L, -1, "set_libpath");
    CHECK_TYPE("engine.set_libpath()", t, LUA_TFUNCTION);
  } else {
    t = lua_getfield(L, -1, "get_libpath");
    CHECK_TYPE("engine.get_libpath()", t, LUA_TFUNCTION);
  }    
  lua_pushvalue(L, -2);
  if (newpath->ptr) lua_pushlstring(L, (const char *)newpath->ptr, newpath->len);
  t = lua_pcall(L, (newpath->ptr) ? 2 : 1, (newpath->ptr) ? 0 : 1, 0);
  if (t != LUA_OK) {
    if (newpath) {
	LOG("engine.set_libpath() failed\n");
      } else {
	LOG("engine.get_libpath() failed\n");
    }      
    lua_settop(L, 0);
    RELEASE_ENGINE_LOCK(e);
    return ERR_ENGINE_CALL_FAILED;
  }
#if LOGGING
  do {
    get_registry(engine_key);
    t = lua_getfield(L, -1, "searchpath");
    LOGf("searchpath obtained directly from engine object is: %s\n", lua_tostring(L, -1));
    lua_pop(L, 1);
  } while (0);
#endif
  if (!newpath->ptr) {
    size_t tmplen;
    const char *tmpptr = lua_tolstring(L, -1, &tmplen);
    str tmpstr = rosie_new_string((byte_ptr)tmpptr, tmplen);
    (*newpath).ptr = tmpstr.ptr;
    (*newpath).len = tmpstr.len;
  }
  lua_settop(L, 0);
  RELEASE_ENGINE_LOCK(e);
  return SUCCESS;
}

EXPORT
int rosie_free_rplx (Engine *e, int pat) {
  lua_State *L = e->L;
  LOGf("freeing rplx object with index %d\n", pat);
  ACQUIRE_ENGINE_LOCK(e);
  get_registry(rplx_table_key);
  luaL_unref(L, -1, pat);
  lua_settop(L, 0);
  RELEASE_ENGINE_LOCK(e);
  return SUCCESS;
}

/* N.B. Client must free messages */
EXPORT
int rosie_compile(Engine *e, str *expression, int *pat, str *messages) {
  int t;
  str temp_rs;
  lua_State *L = e->L;
  
  LOGf("compile(): L = %p\n", L);
  ACQUIRE_ENGINE_LOCK(e);
  if (!pat) {
    LOG("null pointer passed to compile for pattern argument\n");
    lua_settop(L, 0);
    RELEASE_ENGINE_LOCK(e);
    return ERR_ENGINE_CALL_FAILED;
  }
  *pat = 0;
  if (!expression) {
    LOG("null pointer passed to compile for expression argument\n");
    lua_settop(L, 0);
    RELEASE_ENGINE_LOCK(e);
    return ERR_ENGINE_CALL_FAILED;
  }  

#if LOGGING
  if (lua_gettop(L)) LOG("Entering compile(), stack is NOT EMPTY!\n");
#endif  

  get_registry(rplx_table_key);
  get_registry(engine_key);
  t = lua_getfield(L, -1, "compile");
  CHECK_TYPE("compile", t, LUA_TFUNCTION);

  lua_replace(L, -2); /* overwrite engine table with compile function */
  get_registry(engine_key);

  lua_pushlstring(L, (const char *)expression->ptr, expression->len);

  t = lua_pcall(L, 2, 2, 0);

  if (t != LUA_OK) {
    LOG("compile() failed\n");
    LOGstack(L);
    lua_settop(L, 0);
    RELEASE_ENGINE_LOCK(e);
    return ERR_ENGINE_CALL_FAILED;
  }

  if ( lua_isboolean(L, -2) ) {
    *pat = 0;
    CHECK_TYPE("compile messages", lua_type(L, -1), LUA_TTABLE);
    t = to_json_string(L, -1, &temp_rs);
    if (t != LUA_OK) {
      LOG("in compile() could not convert compile messages to json\n");
      lua_settop(L, 0);
      RELEASE_ENGINE_LOCK(e);
      *messages = rosie_new_string_from_const("could not convert compile messages to json");
      return ERR_ENGINE_CALL_FAILED;
    }
    (*messages).ptr = temp_rs.ptr;
    (*messages).len = temp_rs.len;
    lua_settop(L, 0);
    RELEASE_ENGINE_LOCK(e);
    return SUCCESS;
  }
  
  lua_pushvalue(L, -2);
  CHECK_TYPE("new rplx object", lua_type(L, -1), LUA_TTABLE);
  *pat = luaL_ref(L, 1);
#if LOGGING
  if (*pat == LUA_REFNIL) LOG("error storing rplx object\n");
#endif
  LOGf("storing rplx object at index %d\n", *pat);

  t = to_json_string(L, -1, &temp_rs);
  if (t != LUA_OK) {
    LOG("in compile(), could not convert warning information to json\n");
    lua_settop(L, 0);
    RELEASE_ENGINE_LOCK(e);
    return ERR_ENGINE_CALL_FAILED;
  }
  
  (*messages).ptr = temp_rs.ptr;
  (*messages).len = temp_rs.len;

  lua_settop(L, 0);
  RELEASE_ENGINE_LOCK(e);
  return SUCCESS;
}

static inline void collect_if_needed(lua_State *L) {
  int limit, memusg;
  get_registry(alloc_actual_limit_key);
  limit = lua_tointeger(L, -1);	/* nil will convert to zero */
  lua_pop(L, 1);
  if (limit) {
    memusg = lua_gc(L, LUA_GCCOUNT, 0);
    if (memusg > limit) {
      LOGf("invoking collection of %0.1f MB heap\n", memusg/1024.0);
      lua_gc(L, LUA_GCCOLLECT, 0);
#if (LOGGING)
      memusg = lua_gc(L, LUA_GCCOUNT, 0);
      LOGf("post-collection heap has %0.1f MB\n", memusg/1024.0);
#endif
    }
  }
}

#define set_match_error(match, errno) \
  do { (*(match)).data.ptr = NULL;    \
    (*(match)).data.len = (errno);    \
  } while (0);

EXPORT
int rosie_match(Engine *e, int pat, int start, char *encoder_name, str *input, match *match) {
  int t, encoder;
  size_t temp_len;
  unsigned char *temp_str;
  rBuffer *buf;
  lua_State *L = e->L;
  LOG("rosie_match called\n");
  ACQUIRE_ENGINE_LOCK(e);
  collect_if_needed(L);
  if (!pat)
    LOGf("rosie_match() called with invalid compiled pattern reference: %d\n", pat);
  else {
    get_registry(rplx_table_key);
    t = lua_rawgeti(L, -1, pat);
    if (t == LUA_TTABLE) goto have_pattern;
  }
  set_match_error(match, ERR_NO_PATTERN);
  lua_settop(L, 0);
  RELEASE_ENGINE_LOCK(e);
  return SUCCESS;

have_pattern:

  /* The encoder values that do not require Lua processing
   * take a different code path from the ones that do.  When no
   * Lua processing is needed, we can (1) use a lightuserdata to hold
   * a ptr to the rosie_string holding the input, and (2) call into a
   * refactored rmatch such that it allows this. 
   *
   * Otherwise, we call the lua function rplx.match().
   */

  encoder = encoder_name_to_code(encoder_name);
  LOGf("in rosie_match, encoder value is %d\n", encoder);
  if (!encoder) {
    /* Path through Lua */
    t = lua_getfield(L, -1, "match");
    CHECK_TYPE("rplx.match()", t, LUA_TFUNCTION);
    lua_replace(L, 1);
    lua_settop(L, 2);
    /* Don't make a copy of the input.  Wrap it in an rbuf, which will
       be gc'd later (but will not free the original source data). */
    (*fp_r_newbuffer_wrap)(L, (char *)input->ptr, input->len); 
    lua_pushinteger(L, start);
    lua_pushstring(L, encoder_name);
    assert(lua_gettop(L) == 5);
  }
  else {
    /* Path through C */
    t = lua_getfield(L, -1, "pattern");
    CHECK_TYPE("rplx pattern slot", t, LUA_TTABLE);
    t = lua_getfield(L, -1, "peg");
    CHECK_TYPE("rplx pattern peg slot", t, LUA_TUSERDATA);
    lua_pushcfunction(L, *fp_r_match_C);
    lua_copy(L, -1, 1);
    lua_copy(L, -2, 2);
    lua_settop(L, 2);
    lua_pushlightuserdata(L, input); 
    lua_pushinteger(L, start);
    lua_pushinteger(L, encoder);
  }
  
  t = lua_pcall(L, 4, 5, 0); 
  if (t != LUA_OK) {  
    LOG("match() failed\n");  
    LOGstack(L); 
    lua_settop(L, 0); 
    RELEASE_ENGINE_LOCK(e);
    return ERR_ENGINE_CALL_FAILED;  
  }  

  (*match).tmatch = lua_tointeger(L, -1);
  (*match).ttotal = lua_tointeger(L, -2);
  (*match).abend = lua_toboolean(L, -3);
  (*match).leftover = lua_tointeger(L, -4);
  lua_pop(L, 4);

  buf = lua_touserdata(L, -1);
  if (buf) {
    LOG("in rosie_match, match succeeded\n");
    (*match).data.ptr = (unsigned char *)buf->data;
    (*match).data.len = buf->n;
  }
  else if (lua_isboolean(L, -1)) {
    LOG("in rosie_match, match failed\n");
    set_match_error(match, ERR_NO_MATCH);
    (*match).data.ptr = NULL;
    (*match).data.len = 0;
  }
  else if (lua_isstring(L, -1)) {
    if (encoder) {
      LOG("Invalid return type from rmatch (string)\n");
      match = NULL;
      lua_settop(L, 0);
      RELEASE_ENGINE_LOCK(e);
      return ERR_ENGINE_CALL_FAILED;
    }
    /* The client does not need to manage the storage for match
     * results when they are in an rBuffer (userdata), so we do not
     * want the client to manage the storage when it has the form of a
     * Lua string (returned by common.rmatch).  So we alloc the
     * string, and stash a pointer to it in the registry, to be freed
     * the next time around.
    */
    get_registry(prev_string_result_key);
    if (lua_isuserdata(L, -1)) {
      str *rs = lua_touserdata(L, -1);
      rosie_free_string_ptr(rs);
    }
    lua_pop(L, 1);
    temp_str = (unsigned char *)lua_tolstring(L, -1, &temp_len);
    str *rs = rosie_new_string_ptr(temp_str, temp_len);
    lua_pushlightuserdata(L, (void *) rs);
    set_registry(prev_string_result_key);
    (*match).data.ptr = rs->ptr;
    (*match).data.len = rs->len;
  }
  else {
    t = lua_type(L, -1);
    LOGf("Invalid return type from rmatch (%d)\n", t);
    match = NULL;
    lua_settop(L, 0);
    RELEASE_ENGINE_LOCK(e);
    return ERR_ENGINE_CALL_FAILED;
  }

  lua_settop(L, 0);
  RELEASE_ENGINE_LOCK(e);
  return SUCCESS;
}

/* N.B. Client must free trace */
EXPORT
int rosie_trace(Engine *e, int pat, int start, char *trace_style, str *input, int *matched, str *trace) {
  int t;
  str rs;
  lua_State *L = e->L;
  ACQUIRE_ENGINE_LOCK(e);
  collect_if_needed(L);
  get_registry(engine_key);
  t = lua_getfield(L, -1, "trace");
  CHECK_TYPE("engine.trace()", t, LUA_TFUNCTION);
  get_registry(engine_key);	/* first arg to trace */
  if (!pat)
    LOGf("rosie_trace() called with invalid compiled pattern reference: %d\n", pat);
  else {
    get_registry(rplx_table_key);
    t = lua_rawgeti(L, -1, pat); /* arg 2 to trace*/
    if (t == LUA_TTABLE) goto have_pattern;
  }
  (*trace).ptr = NULL;
  (*trace).len = ERR_NO_PATTERN;
  lua_settop(L, 0);
  RELEASE_ENGINE_LOCK(e);
  return SUCCESS;

have_pattern:

  lua_replace(L, -2); 		/* overwrite rplx table with rplx object */
  if (!trace_style) {
    LOG("rosie_trace() called with null trace_style arg\n");
    (*trace).ptr = NULL;
    (*trace).len = ERR_NO_ENCODER;
    lua_settop(L, 0);
    RELEASE_ENGINE_LOCK(e);
    return SUCCESS;
  }

  lua_pushlstring(L, (const char *)input->ptr, input->len); /* arg 3 */
  lua_pushinteger(L, start);	                            /* arg 4 */
  lua_pushstring(L, trace_style);                           /* arg 5 */

  t = lua_pcall(L, 5, 3, 0); 
  if (t != LUA_OK) {  
    LOG("trace() failed\n");  
    LOGstack(L); 
    lua_settop(L, 0); 
    RELEASE_ENGINE_LOCK(e);
    return ERR_ENGINE_CALL_FAILED;  
  }  

  /* The first return value from trace indicates whether the pattern
     compiled, and we are always sending in a compiled pattern, so the
     first return value is always true. 
  */
  assert( lua_isboolean(L, -3) );
  assert( lua_isboolean(L, -2) );
  (*matched) = lua_toboolean(L, -2);

  if (lua_istable(L, -1)) {
    t = to_json_string(L, -1, &rs);
    if (t != LUA_OK) {
      rs = rosie_new_string_from_const("error: could not convert trace data to json");      
      goto fail_with_message;
    }
  }
  else if (lua_isstring(L, -1)) {
    byte_ptr temp_str;
    size_t temp_len;
    temp_str = (byte_ptr) lua_tolstring(L, -1, &(temp_len));
    rs = rosie_new_string(temp_str, temp_len);
  }
  else {
    LOG("trace() failed with unexpected return value from engine.trace()\n");
    LOGstack(L);
    lua_settop(L, 0);
    RELEASE_ENGINE_LOCK(e);
    return ERR_ENGINE_CALL_FAILED;
  }

 fail_with_message:
  
  (*trace).ptr = rs.ptr;
  (*trace).len = rs.len;

  lua_settop(L, 0);
  RELEASE_ENGINE_LOCK(e);
  return SUCCESS;
}

/* N.B. Client must free 'messages' */
EXPORT
int rosie_load(Engine *e, int *ok, str *src, str *pkgname, str *messages) {
  int t;
  size_t temp_len;
  unsigned char *temp_str;
  str temp_rs;
  lua_State *L = e->L;
  ACQUIRE_ENGINE_LOCK(e);
  get_registry(engine_key);
  t = lua_getfield(L, -1, "load");
  CHECK_TYPE("engine.load()", t, LUA_TFUNCTION);
  lua_pushvalue(L, -2);		/* push engine object again */
  lua_pushlstring(L, (const char *)src->ptr, src->len);

  t = lua_pcall(L, 2, 3, 0); 
  if (t != LUA_OK) { 
    /* Details will likely not be helpful to the user */
    LOG("engine.load() failed\n"); 
    *messages = rosie_new_string_from_const("engine.load() failed"); 
    LOGstack(L);
    lua_settop(L, 0);
    RELEASE_ENGINE_LOCK(e);
    return ERR_ENGINE_CALL_FAILED; 
  } 

  *ok = lua_toboolean(L, -3);
  LOGf("engine.load() %s\n", *ok ? "succeeded\n" : "failed\n");
  
  if (lua_isstring(L, -2)) {
    temp_str = (unsigned char *)lua_tolstring(L, -2, &temp_len);
    *pkgname = rosie_new_string(temp_str, temp_len);
  }
  else {
    pkgname->ptr = NULL;
    pkgname->len = 0;
  }
  
  t = to_json_string(L, -1, &temp_rs);
  if (t != LUA_OK) {
    LOG("in load(), could not convert error information to json\n");
    temp_rs = rosie_new_string_from_const("in load(), could not convert error information to json");
    goto fail_load_with_messages;
    }

 fail_load_with_messages:
  (*messages).ptr = temp_rs.ptr;
  (*messages).len = temp_rs.len;

  lua_settop(L, 0);
  RELEASE_ENGINE_LOCK(e);
  return SUCCESS;
}

/* N.B. Client must free 'messages' */
EXPORT
int rosie_loadfile(Engine *e, int *ok, str *fn, str *pkgname, str *messages) {
  int t;
  size_t temp_len;
  unsigned char *temp_str;
  str temp_rs;
  lua_State *L = e->L;
  ACQUIRE_ENGINE_LOCK(e);
  get_registry(engine_key);
  t = lua_getfield(L, -1, "loadfile");
  CHECK_TYPE("engine.loadfile()", t, LUA_TFUNCTION);
  lua_pushvalue(L, -2);		/* push engine object again */
  const char *fname = lua_pushlstring(L, (const char *)fn->ptr, fn->len);

  LOGf("engine.loadfile(): about to load %s\n", fname);
  t = lua_pcall(L, 2, 3, 0); 
  if (t != LUA_OK) { 
    display("Internal error: call to engine.loadfile() failed"); 
    /* Details will likely not be helpful to the user */
    LOGstack(L);
    lua_settop(L, 0);
    RELEASE_ENGINE_LOCK(e);
    return ERR_ENGINE_CALL_FAILED; 
  } 

  *ok = lua_toboolean(L, -3);
  LOGf("engine.loadfile() %s\n", *ok ? "succeeded" : "failed");
  LOGstack(L);
  
  if (lua_isstring(L, -2)) {
    temp_str = (unsigned char *)lua_tolstring(L, -2, &temp_len);
    str loaded_pkgname = rosie_new_string(temp_str, temp_len);
    (*pkgname).ptr = loaded_pkgname.ptr;
    (*pkgname).len = loaded_pkgname.len;
  }
  else {
    (*pkgname).ptr = NULL;
    (*pkgname).len = 0;
  }
  
  t = strip_violation_messages(L);
  if (t != LUA_OK) { 
    LOG("violation.strip_each() failed\n"); 
    LOGstack(L);
    lua_settop(L, 0);
    RELEASE_ENGINE_LOCK(e);
    return ERR_ENGINE_CALL_FAILED; 
  } 
  t = to_json_string(L, -1, &temp_rs);
  if (t != LUA_OK) {
    LOG("in load(), could not convert error information to json\n");
    lua_settop(L, 0);
    RELEASE_ENGINE_LOCK(e);
    return ERR_ENGINE_CALL_FAILED;
  }
  (*messages).ptr = temp_rs.ptr;
  (*messages).len = temp_rs.len;

  lua_settop(L, 0);
  RELEASE_ENGINE_LOCK(e);
  return SUCCESS;
}

/* N.B. Client must free 'messages' */
EXPORT
int rosie_import(Engine *e, int *ok, str *pkgname, str *as, str *actual_pkgname, str *messages) {
  int t;
  size_t temp_len;
  unsigned char *temp_str;
  str temp_rs;
  lua_State *L = e->L;
  
  ACQUIRE_ENGINE_LOCK(e);
  get_registry(engine_key);
  t = lua_getfield(L, -1, "import");
  CHECK_TYPE("engine.import()", t, LUA_TFUNCTION);
  lua_pushvalue(L, -2);		/* push engine object again */
  lua_pushlstring(L, (const char *)pkgname->ptr, pkgname->len);
  if (as) {
    lua_pushlstring(L, (const char *)as->ptr, as->len);
  }
  else {
    lua_pushnil(L);
  }

  t = lua_pcall(L, 3, 3, 0); 
  if (t != LUA_OK) { 
    LOG("engine.import() failed\n"); 
    LOGstack(L);
    lua_settop(L, 0);
    RELEASE_ENGINE_LOCK(e);
    return ERR_ENGINE_CALL_FAILED; 
  } 

  *ok = lua_toboolean(L, -3);
  LOGf("engine.import() %s\n", *ok ? "succeeded" : "failed");
  
  if (lua_isstring(L, -2)) {
    temp_str = (unsigned char *)lua_tolstring(L, -2, &temp_len);
    *actual_pkgname = rosie_new_string(temp_str, temp_len);
    LOGf("engine.import reports that package %s was loaded\n", temp_str);
  }
  else {
    (*actual_pkgname).ptr = NULL;
    (*actual_pkgname).len = 0;
  }
  
  t = strip_violation_messages(L);
  if (t != LUA_OK) { 
    LOG("violation.strip_each() failed\n"); 
    LOGstack(L);
    lua_settop(L, 0);
    RELEASE_ENGINE_LOCK(e);
    return ERR_ENGINE_CALL_FAILED; 
  } 

  t = to_json_string(L, -1, &temp_rs);
  if (t != LUA_OK) {
    LOG("in import(), could not convert error information to json\n");
    lua_settop(L, 0);
    RELEASE_ENGINE_LOCK(e);
    return ERR_ENGINE_CALL_FAILED;
  }
  (*messages).ptr = temp_rs.ptr;
  (*messages).len = temp_rs.len;

  lua_settop(L, 0);
  RELEASE_ENGINE_LOCK(e);
  return SUCCESS;
}

/* FUTURE: Expose engine_process_file() ? */

/* N.B. Client must free messages */
EXPORT
int rosie_matchfile(Engine *e, int pat, char *encoder, int wholefileflag,
		    char *infilename, char *outfilename, char *errfilename,
		    int *cin, int *cout, int *cerr,
		    str *err) {
  int t;
  unsigned char *temp_str;
  size_t temp_len;
  lua_State *L = e->L;
  (*err).ptr = NULL;
  (*err).len = 0;

  ACQUIRE_ENGINE_LOCK(e);
  collect_if_needed(L);
  get_registry(engine_key);
  t = lua_getfield(L, -1, "matchfile");
  CHECK_TYPE("engine.matchfile()", t, LUA_TFUNCTION);
  get_registry(engine_key);	/* first arg */

  get_registry(rplx_table_key);
  t = lua_rawgeti(L, -1, pat); /* arg 2 */
  if (t != LUA_TTABLE) {
    LOGf("rosie_matchfile() called with invalid compiled pattern reference: %d\n", pat);
    (*cin) = -1;
    (*cout) = ERR_NO_PATTERN;
    lua_settop(L, 0);
    RELEASE_ENGINE_LOCK(e);
    return SUCCESS;
  }

  lua_replace(L, -2); 		/* overwrite rplx table with rplx object */
  if (!encoder) {
    LOG("rosie_matchfile() called with null encoder name\n");
    (*cin) = -1;
    (*cout) = ERR_NO_ENCODER;
    lua_settop(L, 0);
    RELEASE_ENGINE_LOCK(e);
    return SUCCESS;
  }

  lua_pushstring(L, infilename);  /* arg 3 */
  lua_pushstring(L, outfilename); /* arg 4 */
  lua_pushstring(L, errfilename); /* arg 5 */
  lua_pushstring(L, encoder);	  /* arg 6 */
  lua_pushboolean(L, wholefileflag); /* arg 7 */

  t = lua_pcall(L, 7, 3, 0); 
  if (t != LUA_OK) {  
    LOG("matchfile() failed\n");  
    LOGstack(L); 
    /* FUTURE: return the error, if there's a situation where it may help */
    lua_settop(L, 0); 
    RELEASE_ENGINE_LOCK(e);
    return ERR_ENGINE_CALL_FAILED;  
  }  

  if (lua_isnil(L, -1)) {

       LOGstack(L);

       /* i/o issue with one of the files */
       (*cin) = -1;
       (*cout) = 3;
       temp_str =  (unsigned char *)lua_tolstring(L, -2, &temp_len);
       str msg = rosie_new_string(temp_str, temp_len);
       (*err).ptr = msg.ptr;
       (*err).len = msg.len;
       lua_settop(L, 0);
       RELEASE_ENGINE_LOCK(e);
       return SUCCESS;
  }

  (*cin) = lua_tointeger(L, -3);  /* cerr */
  (*cout) = lua_tointeger(L, -2); /* cout, or error code if error */
  (*cerr) = lua_tointeger(L, -1); /* cin, or -1 if error */
  
  lua_settop(L, 0);
  RELEASE_ENGINE_LOCK(e);
  return SUCCESS;
}


EXPORT
void rosie_finalize(Engine *e) {
  lua_State *L = e->L;
  ACQUIRE_ENGINE_LOCK(e);
  get_registry(prev_string_result_key); 
  if (lua_isuserdata(L, -1)) { 
    str *rs = lua_touserdata(L, -1); 
    if (rs->ptr) rosie_free_string_ptr(rs); 
    lua_pop(L, 1); 
  } 
  LOGf("Finalizing engine %p\n", L);
  lua_close(L);
  /*
   * We do not RELEASE_ENGINE_LOCK(e) here because a waiting thread
   * would then have access to an engine which we have closed, and
   * whose memory we are about to free.
   *
   * The caller should take care to have each engine be created, used,
   * destroyed, and then never used again.  
   *
   * One way to achieve this is to have each thread responsible for
   * creating and destroying its own engines.  In that scenario, a
   * thread's engine should be private to that thread.
   * 
   * Alternatively, an engine pool could be created (in the client
   * code).  The pool manager would be responsible for calling
   * rosie_finalize() when there is no danger of any thread attempting
   * to use the engine being destroyed.
   *
   */
  free(e);
}

/* ----------------------------------------------------------------------------------------
 * Functions to support the Lua implementation of the CLI
 * ----------------------------------------------------------------------------------------
 */

/*
 * Message handler that will be invoked if the CLI should throw an
 * exception (which would indicate a bug).  This handler is from
 * Lua.org's lua.c (stand-alone Lua interpreter).
 */
static int msghandler (lua_State *L) {
  const char *msg = lua_tostring(L, 1);
  if (msg == NULL) {  /* is error object not a string? */
    if (luaL_callmeta(L, 1, "__tostring") &&  /* does it have a metamethod */
        lua_type(L, -1) == LUA_TSTRING)  /* that produces a string? */
      return 1;  /* that is the message */
    else
      msg = lua_pushfstring(L, "(error object is a %s value)",
                               luaL_typename(L, 1));
  }
  luaL_traceback(L, L, msg, 1);  /* append a standard traceback */
  return 1;  /* return the traceback */
}

/*
 * Interface to 'lua_pcall', which sets appropriate message function
 * and C-signal handler. Used to run all chunks.  This is from
 * Lua.org's lua.c.
 */
static lua_State *globalL = NULL;
static void lstop (lua_State *L, lua_Debug *ar) {
  (void)ar;  /* unused arg. */
  lua_sethook(L, NULL, 0, 0);  /* reset hook */
  luaL_error(L, "interrupted!");
}
static void laction (int i) {
  signal(i, SIG_DFL); /* if another SIGINT happens, terminate process */
  lua_sethook(globalL, lstop, LUA_MASKCALL | LUA_MASKRET | LUA_MASKCOUNT, 1);
}
static int docall (lua_State *L, int narg, int nres) {
  int status;
  int base = lua_gettop(L) - narg;  /* function index */
  lua_pushcfunction(L, msghandler);  /* push message handler */
  lua_insert(L, base);  /* put it under function and args */
  globalL = L;  /* to be available to 'laction' */
  signal(SIGINT, laction);  /* set C-signal handler */
  status = lua_pcall(L, narg, nres, base);
  signal(SIGINT, SIG_DFL); /* reset C-signal handler */
  lua_remove(L, base);  /* remove message handler from the stack */
  return status;
}

#define CLI_LUAC "/lib/cli.luac"

static void pushargs(lua_State *L, int argc, char **argv) {
  lua_createtable(L, argc+1, 0);
  for (int i = 0; i < argc; i++) {
    lua_pushstring(L, argv[i]);
    lua_rawseti(L, -2, i);
  }
  lua_setglobal(L, "arg");
}

int luaopen_readline (lua_State *L); /* will dynamically load the system libreadline/libedit */

EXPORT
int rosie_exec_cli(Engine *e, int argc, char **argv, char **err) {
  char fname[MAXPATHLEN];
  size_t len = strnlen(rosiehomedir, MAXPATHLEN);
  char *last = stpncpy(fname, rosiehomedir, (MAXPATHLEN - len - 1));
  last = stpncpy(last, CLI_LUAC, len);
  *last = '\0';

  LOGf("Entering rosie_exec_cli, computed cli filename is %s\n", fname);

  ACQUIRE_ENGINE_LOCK(e);
  lua_State *L = e->L;
  luaL_requiref(L, "readline", luaopen_readline, 0);

  get_registry(engine_key);
  lua_setglobal(L, "cli_engine");
  
  pushargs(L, argc, argv);

  int status = luaL_loadfile(L, fname);
  if (status != LUA_OK) {
    LOGf("Failed to load cli from %s\n", fname);
    *err = strndup(lua_tostring(L, -1), MAXPATHLEN);
    lua_settop(L, 0);
    RELEASE_ENGINE_LOCK(e);
    return status;
  }  
  globalL = L;
  status = docall(L, 0, 1);
  if (status != LUA_OK) {
    const char *err = lua_tostring(L, -1);
    lua_pop(L, 1);  /* remove message */
    const char *progname = NULL;
    if (argv[0] && argv[0][0]) progname = argv[0];
    fprintf(stderr, "%s: error (%d) executing CLI (please report this as a bug):\n%s\n", progname, status, err);
  } else {
    status = lua_tointeger(L, -1);
  }
  lua_settop(L, 0);
  RELEASE_ENGINE_LOCK(e);
  return status;
}

#ifdef LUADEBUG

int lua_repl(lua_State *L, const char *main_progname);

EXPORT
int rosie_exec_lua_repl(Engine *e, int argc, char **argv) {
  LOG("Entering rosie_exec_lua_repl\n");

  ACQUIRE_ENGINE_LOCK(e);
  lua_State *L = e->L;
  luaL_requiref(L, "readline", luaopen_readline, 0);

  get_registry(engine_key);
  lua_setglobal(L, "cli_engine");
  
  pushargs(L, argc, argv);
  lua_repl(L, argv[0]);
  lua_settop(L, 0);
  RELEASE_ENGINE_LOCK(e);
  return SUCCESS;
}

#endif	/* LUADEBUG */
