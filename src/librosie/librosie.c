/*  -*- Mode: C; -*-                                                         */
/*                                                                           */
/* librosie.c    Expose the Rosie API                                        */
/*                                                                           */
/*  © Copyright IBM Corporation 2016, 2017                                   */
/*  LICENSE: MIT License (https://opensource.org/licenses/mit-license.html)  */
/*  AUTHOR: Jamie A. Jennings                                                */

/* Protocol:
 * 
 * Call new() to get an engine.  Every thread must have its own engine.
 *
 * Call free() to destroy an engine and free its memory.
 *
*/


#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "lauxlib.h"
#include "lualib.h"

#include "librosie.h"

#include <dlfcn.h>
#include <libgen.h>

int luaopen_lpeg (lua_State *L);
int luaopen_cjson (lua_State *L);

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

static char libname[MAXPATHLEN];
static char libdir[MAXPATHLEN];
static char rosiehomedir[MAXPATHLEN];
static char bootscript[MAXPATHLEN];

/* ---------------------------------------------------------------------------------------- 
 * The following keys are used to store values in the lua registry 
 * ----------------------------------------------------------------------------------------
 */
enum KEYS {
  engine_key = 0,
  rosie_key,
  rplx_table_key,
  json_encoder_key,
  alloc_limit_key,
  prev_string_result_key,
  KEY_ARRAY_SIZE
};

static int key_array[KEY_ARRAY_SIZE];

#define keyval(key) ((void *)&key_array[(key)])

#define get_registry(key) \
  do { lua_pushlightuserdata(L, keyval(key));  \
       lua_gettable(L, LUA_REGISTRYINDEX);     \
  } while (0)

/* Call set_registry with val on top of stack.  Stack will be unchanged after call. */
#define set_registry(key) \
  do { lua_pushlightuserdata(L, keyval(key)); \
       lua_pushvalue(L, -2);	              \
       lua_settable(L, LUA_REGISTRYINDEX);    \
  } while (0)


/* ----------------------------------------------------------------------------------------
 * Logging and debugging
 * ----------------------------------------------------------------------------------------
 */

/* Compile with DEBUG=1 to enable logging */
#ifdef DEBUG
#define LOGGING 1
#else
#define LOGGING 0
#endif

#define LOG(msg) \
     do { if (LOGGING) fprintf(stderr, "%s:%d:%s(): %s", __FILE__, \
			       __LINE__, __func__, msg);	   \
	  fflush(NULL);						   \
     } while (0)

#define LOGf(fmt, ...) \
     do { if (LOGGING) fprintf(stderr, "%s:%d:%s(): " fmt, __FILE__, \
			       __LINE__, __func__, __VA_ARGS__);     \
	  fflush(NULL);						     \
     } while (0)

#define LOGstack(L)		      \
  do { if (LOGGING) {		      \
    fprintf(stderr, "%s:%d:%s(): lua stack dump: ", __FILE__,	     \
	    __LINE__, __func__);			     \
    stackDump(L);						     \
    fflush(NULL);						     \
  } \
} while (0)

static void stackDump (lua_State *L) {
      int i;
      int top = lua_gettop(L);
      if (top==0) { printf("EMPTY STACK\n"); return;}
      for (i = top; i >= 1; i--) {
        int t = lua_type(L, i);
        switch (t) {
          case LUA_TSTRING:  /* strings */
	       printf("%d: '%s'", i, lua_tostring(L, i));
            break;
          case LUA_TBOOLEAN:  /* booleans */
	       printf("%d: %s", i, (lua_toboolean(L, i) ? "true" : "false"));
            break;
          case LUA_TNUMBER:  /* numbers */
	       printf("%d: %g", i, lua_tonumber(L, i));
            break;
          default:  /* other values */
	       printf("%d: %s", i, lua_typename(L, t));
            break;
        }
        printf("  ");
      }
      printf("\n");
      fflush(NULL);
    }


#ifdef DEBUG
#define CHECK_TYPE(label, typ, expected_typ) \
  do { if (DEBUG) check_type((label), (typ), (expected_typ)); } while (0)
#else
#define CHECK_TYPE(label, typ, expected_typ)
#endif

void check_type(const char *thing, int t, int expected) {
  if (t != expected)
    LOGf("type mismatch for %s.  received %d, expected %d.\n", thing, t, expected);
}

/* ----------------------------------------------------------------------------------------
 * Utility functions
 * ----------------------------------------------------------------------------------------
 */

static void display (const char *msg) {
  fprintf(stderr, "%s: ", libname);
  fprintf(stderr, "%s\n", msg);
  fflush(NULL);
}

static void set_libinfo() {
  Dl_info dl;
  int ok = dladdr((void *)set_libinfo, &dl);
  if (!ok) {
    display("librosie: call to dladdr failed");
    exit(ERR_SYSCALL_FAILED);
  }
  LOGf("dli_fname is %s\n", dl.dli_fname);
  if (!basename_r((char *)dl.dli_fname, libname) ||
      !dirname_r((char *)dl.dli_fname, libdir)) {
    display("librosie: call to basename/dirname failed");
    exit(ERR_SYSCALL_FAILED);
  }
  LOGf("libdir is %s, and libname is %s\n", libdir, libname);
}

static void set_bootscript() {
  size_t len;
  static char *last;
  if (!*libdir) set_libinfo();
  /* set rosiehomedir */
  len = strnlen(ROSIEHOME, MAXPATHLEN);
  last = stpncpy(rosiehomedir, libdir, (MAXPATHLEN - len - 1));
  last = stpncpy(last, ROSIEHOME, len);
  *last = '\0';
  /* set bootscript (absolute path to boot script) */
  len = strnlen(BOOTSCRIPT, MAXPATHLEN);
  last = stpncpy(bootscript, rosiehomedir, (MAXPATHLEN - len - 1));
  last = stpncpy(last, BOOTSCRIPT, len);
  *last = '\0';
  assert((last-bootscript) < MAXPATHLEN);
  LOGf("Bootscript filename set to %s\n", bootscript);
}

static int boot (lua_State *L) {
  if (!*bootscript) set_bootscript();
  assert(bootscript);
  LOGf("Booting rosie from %s\n", bootscript);
  int status = luaL_loadfile(L, bootscript);
  if (status != LUA_OK) {
    LOG("Failed to read boot code (using loadfile)\n");
    return FALSE;
  }
  LOG("Reading of boot code succeeded (using loadfile)\n");
  status = lua_pcall(L, 0, LUA_MULTRET, 0);
  if (status != LUA_OK) {
    LOG("Loading of boot code failed\n");
    return FALSE;
  }
  LOG("Loading of boot code succeeded\n");
  lua_pushlstring(L, (const char *)rosiehomedir, strnlen(rosiehomedir, MAXPATHLEN));
  status = lua_pcall(L, 1, LUA_MULTRET, 0);
  if (status!=LUA_OK) {
    LOG("Boot function failed.  Lua stack is: \n");
    LOGstack(L);
    return FALSE;
  }
  LOG("Boot function succeeded\n");
  return TRUE;
}

str *to_json_string(lua_State *L, int pos) {
     size_t len;
     byte_ptr str;
     int t;
     int top = lua_gettop(L);
     get_registry(json_encoder_key);
     lua_pushvalue(L, pos-1);	/* offset becaus we pushed json_encoder */
     t = lua_pcall(L, 1, LUA_MULTRET, 0);
     if (t != LUA_OK) {
       /* TODO: return a message? */
       LOG("call to json encoder failed\n");
       LOGstack(L);
       return NULL;
     }
     if ((lua_gettop(L) - top) > 1) {
       /* Top of stack is error msg */
       LOG("call to json encoder returned more than one value\n");
       if (lua_isstring(L, -1) && lua_isnil(L, -2)) {
	 /* TO DO: return something to indicate the error */
	 LOGf("error message from json encoder: %s\n", lua_tolstring(L, -1, NULL));
	 LOGstack(L);
	 return NULL;
       }
       else {
	 /* TO DO: something really strange happened! what to return? */
	 LOG("call to json encoder returned unexpected values\n");
	 LOGstack(L);
	 return NULL;
       }
     }
     str = (byte_ptr) lua_tolstring(L, -1, &len);
     return rosie_new_string_ptr(str, len);
}

/* ----------------------------------------------------------------------------------------
 * Exported functions
 * ----------------------------------------------------------------------------------------
 */

int rosie_set_alloc_limit(lua_State *L, int newlimit) {
  int memusg, actual_limit;
  if ((newlimit != 0) && (newlimit < MIN_ALLOC_LIMIT_MB)) return ERR_ENGINE_CALL_FAILED;
  else {
    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_gc(L, LUA_GCCOLLECT, 0);	/* second time to free resources marked for finalization */
    memusg = lua_gc(L, LUA_GCCOUNT, 0); /* KB */
    actual_limit = memusg + (newlimit * 1024);
    lua_pushinteger(L, (newlimit == 0) ? 0 : actual_limit);
    set_registry(alloc_limit_key);
    lua_pop(L, 1);
    if (newlimit == 0) LOGf("set alloc limit to UNLIMITED above current usage level of %0.1f MB\n", memusg/1024.0);
    else LOGf("set alloc limit to %d MB above current usage level of %0.1f MB\n", newlimit, memusg/1024.0);
  }
  return SUCCESS;
}

void *rosie_new() {
  int t;
  lua_State *L = luaL_newstate();
  if (L == NULL) {
    display("Cannot initialize: not enough memory");
    return NULL;
  }
  luaL_checkversion(L);		/* Ensures several critical things needed to use Lua */
  luaL_openlibs(L);

  if (!boot(L)) {
    display("Cannot initialize: bootstrap failed\n");
    return NULL;
  }
  if (!lua_checkstack(L, 6)) {
    display("Cannot initialize: not enough memory for stack");
    return NULL;
  }
  t = lua_getglobal(L, "rosie");
  CHECK_TYPE("rosie", t, LUA_TTABLE);
  set_registry(rosie_key);
  
  t = lua_getfield(L, -1, "engine");
  CHECK_TYPE("engine", t, LUA_TTABLE);
  t = lua_getfield(L, -1, "new");
  CHECK_TYPE("new", t, LUA_TFUNCTION);
  t = lua_pcall(L, 0, 1, 0);
  if (t != LUA_OK) {
    display("rosie.engine.new() failed");
    return NULL;
  }

  /* engine instance is at top of stack */
  set_registry(engine_key);

#if (LOGGING)
  display(luaL_tolstring(L, -1, NULL));
  lua_pop(L, 1); 
#endif

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

  rosie_set_alloc_limit(L, INITIAL_ALLOC_LIMIT_MB);

  lua_settop(L, 0);
  LOG("Engine created\n");
  return L;
}

str rosie_new_string(byte_ptr msg, size_t len) {
  str retval;
  retval.len = len;
  retval.ptr = malloc(len+1);
  if (!retval.ptr) {
    display("Out of memory (new1)");
    return retval;
  }
  memcpy((char *)retval.ptr, msg, retval.len);    /* sure it is allocated on the heap. */
  retval.ptr[len]=0;		/* add null terminator. */
  return retval;
}

str *rosie_new_string_ptr(byte_ptr msg, size_t len) {
  str temp = rosie_new_string(msg, len);
  str *retval = malloc(sizeof(str));
  if (!retval) {
    display("Out of memory (new2)");
    return NULL;
  }
  retval->len = temp.len;
  retval->ptr = temp.ptr;
  return retval;
}     

void rosie_free_string_ptr(str *ref) {
     free(ref->ptr);
     free(ref);
}

void rosie_free_string(str s) {
     free(s.ptr);
}

int rosie_config(lua_State *L, str *retval) {
  int t;
  str *r;
  get_registry(rosie_key);
  /* t = lua_getfield(L, -1, "config_json"); */
  t = lua_getfield(L, -1, "config");
  CHECK_TYPE("config_json", t, LUA_TFUNCTION);
  t = lua_pcall(L, 0, 1, 0);
  if (t != LUA_OK) {
    /* LOG("rosie.config_json() failed"); */
    LOG("rosie.config() failed");
    lua_settop(L, 0);
    return ERR_ENGINE_CALL_FAILED;
  }
  /* Client must free retval */
  r = to_json_string(L, -1);
  if (r == NULL) {
    lua_settop(L, 0);
    return ERR_ENGINE_CALL_FAILED;
  }
  retval->len = r->len;
  retval->ptr = r->ptr;
  lua_settop(L, 0);
  return SUCCESS;
}

int rosie_free_rplx(lua_State *L, int pat) {
  LOGf("freeing rplx object with index %d\n", pat);
  get_registry(rplx_table_key);
  luaL_unref(L, -1, pat);
  lua_settop(L, 0);
  return SUCCESS;
}

/* N.B. Client must free errors */
int rosie_compile(lua_State *L, str *expression, int *pat, str *errors) {
  int t;
  str *temp_rs;

  if (!pat) {
    LOG("null pointer passed to compile for pattern argument");
    lua_settop(L, 0);
    return ERR_ENGINE_CALL_FAILED;
  }
  if (!expression) {
    LOG("null pointer passed to compile for expression argument");
    lua_settop(L, 0);
    return ERR_ENGINE_CALL_FAILED;
  }  

  get_registry(rplx_table_key);

  get_registry(engine_key);
  t = lua_getfield(L, -1, "compile");
  CHECK_TYPE("compile", t, LUA_TFUNCTION);
  /* overwrite engine table with compile function */
  lua_replace(L, -2);
  get_registry(engine_key);

  lua_pushlstring(L, (const char *)expression->ptr, expression->len);
  t = lua_pcall(L, 2, 2, 0);
  if (t != LUA_OK) {
    LOG("compile() failed");
    LOGstack(L);
    lua_settop(L, 0);
    return ERR_ENGINE_CALL_FAILED;
  }

  if ( lua_isboolean(L, -2) ) {
    *pat = 0;
    CHECK_TYPE("compile errors", lua_type(L, -1), LUA_TTABLE);
    temp_rs = to_json_string(L, -1);
    if (temp_rs == NULL) {
      lua_settop(L, 0);
      return ERR_ENGINE_CALL_FAILED;
    }
    errors->len = temp_rs->len;
    errors->ptr = temp_rs->ptr;
    lua_settop(L, 0);
    return SUCCESS;
  }
  
  lua_pushvalue(L, -2);
  CHECK_TYPE("new rplx object", lua_type(L, -1), LUA_TTABLE);
  *pat = luaL_ref(L, 1);
  LOGf("storing rplx object at index %d\n", *pat);

  temp_rs = to_json_string(L, -1);
    if (temp_rs == NULL) {
      lua_settop(L, 0);
      return ERR_ENGINE_CALL_FAILED;
    }
  errors->len = temp_rs->len;
  errors->ptr = temp_rs->ptr;

  lua_settop(L, 0);
  return SUCCESS;
}

void collect_if_needed(lua_State *L) {
  int limit, memusg;
  get_registry(alloc_limit_key);
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

int rosie_match(lua_State *L, int pat, int start, char *encoder, str *input, match *match) {
  int t, top;
  size_t temp_len;
  unsigned char *temp_str;
  rBuffer *buf;

  collect_if_needed(L);
  
  get_registry(rplx_table_key);
  t = lua_geti(L, -1, pat);
  if (t == LUA_TNIL) {
    LOGf("rosie_match() called with invalid compiled pattern reference: %d\n", pat);
    match = NULL;
    lua_settop(L, 0);
    return SUCCESS;
  }
  
  /* TODO: store peg instead of full rplx object */
  CHECK_TYPE("rplx object", t, LUA_TTABLE);
  t = lua_getfield(L, -1, "pattern");
  CHECK_TYPE("rplx pattern slot", t, LUA_TTABLE);
  t = lua_getfield(L, -1, "peg");
  CHECK_TYPE("rplx pattern peg slot", t, LUA_TUSERDATA);

  /* TODO: The encoder values that do not require Lua processing
   * should take a different code path from the ones that do.  When no
   * Lua processing is needed, we can (1) use a lightuserdata to hold
   * a ptr to the rosie_string holding the input, and (2) call into a
   * refactored rmatch such that it expects this.  While we are doing
   * this, we should bypass the overhead of the rplx.match function,
   * which calls _match which checks to make sure the input is
   * compiled -- and in our case it always is.
   *
   * OTHERWISE: If we are going to use Lua to generate the output,
   * then we will wrap the input in a userdata using r_newbuffer_wrap,
   * and call the rplx.match function like we do now.
   *
   * IMPLICATION: The api has to have a table of encoders or a way to
   * know which ones need Lua.  ALSO, it's better in several ways to
   * use a small integer to specify an encoder.  SO, maybe on the Lua
   * side, maintain a table of encoders, and assign a number to each
   * one.  Better, maintain TWO tables:

   * In the BUILT-IN table, list the names of the output types
   * produced by rpeg, and for each one the code to pass to rpeg.

   * In the EXTENSION table, list the names of the output types
   * produced by registered Lua encoders, and for each one the code to
   * pass to rpeg and the lua function to call.

   * The EXTENSION table can be handled entirely by Lua -- from
   * librosie, we just have to wrap the input into a userdata and call
   * a Lua entrypoint to do the match and process the result.

   * When the librosie client wants an encoder that is in the BUILT-IN
   * table, we should be able to bypass Lua entirely and call a new
   * rpeg entry point with the input pointer and length.  No
   * lightuserdata is needed.

   * PERHAPS assign negative numbers to the BUILT-IN entries, positive
   * numbers to the EXTENSION entries, and leave 0 for the default?

   */

  lua_pushcfunction(L, r_match_C);
  lua_copy(L, -1, 1);
  lua_copy(L, -2, 2);
  lua_settop(L, 2);

  /* Don't make a copy of the input.  Wrap it in an rbuf, which will
     be gc'd later (but will not free the original source data. */
  /* buf = r_newbuffer_wrap(L, (char *)input->ptr, input->len); */
  lua_pushlightuserdata(L, input); 
  lua_pushinteger(L, start);

  /* lua_pushstring(L, encoder);   */
  /* TEMP: hardcoded force encoder to be 1 */
  lua_pushinteger(L, 1);
  
  t = lua_pcall(L, 4, LUA_MULTRET, 0); 
  if (t != LUA_OK) { 
    LOG("match() failed\n"); 
    LOGstack(L);
    lua_settop(L, 0);
    return ERR_ENGINE_CALL_FAILED; 
  } 

  if (lua_gettop(L) != 5) {
    int actual = lua_gettop(L);
    LOGf("Internal error: wrong number of return values (expected 4, received %d)\n", actual);
    LOGstack(L);
    lua_settop(L, 0);
    return ERR_ENGINE_CALL_FAILED;
  }

  (*match).tmatch = lua_tointeger(L, -1);
  (*match).ttotal = lua_tointeger(L, -2);
  /* (*match).abend = lua_toboolean(L, -3); */
  (*match).leftover = lua_tointeger(L, -4);
  lua_pop(L, 4);

  buf = lua_touserdata(L, -1);
  if (buf) {
    (*match).data.ptr = (unsigned char *)buf->data;
    (*match).data.len = buf->n;
  }
  else if (lua_isboolean(L, -1)) {
    (*match).data.ptr = NULL;
    (*match).data.len = 0;
  }
  else if (lua_isstring(L, -1)) {
    /* The client does not need to manage the storage for match
     * results when they are in an rBuffer, so we do not want the
     * client to manage the storage when it has the form of a Lua
     * string (returned by common.rmatch).  So we alloc the string,
     * and stash a pointer to it in the registry, to be freed the next
     * time around.
    */
    get_registry(prev_string_result_key);
    if (lua_isuserdata(L, -1)) {
      str *rs = lua_touserdata(L, -1);
      rosie_free_string_ptr(rs);
      lua_pop(L, 1);
    }
    temp_str = (unsigned char *)lua_tolstring(L, -1, &temp_len);
    str *rs = rosie_new_string_ptr(temp_str, temp_len);
    lua_pushlightuserdata(L, (void *) rs);
    set_registry(prev_string_result_key);
    (*match).data.ptr = rs->ptr;
    (*match).data.len = rs->len;
  }

  lua_settop(L, 0);
  return SUCCESS;
}

/* N.B. Client must free 'errors' */
int rosie_load(lua_State *L, int *ok, str *src, str *pkgname, str *errors) {
  int t;
  size_t temp_len;
  unsigned char *temp_str;
  str *temp_rs;
  
  get_registry(engine_key);
  t = lua_getfield(L, -1, "load");
  CHECK_TYPE("engine load function", t, LUA_TFUNCTION);
  lua_pushvalue(L, -2);		/* push engine object again */
  lua_pushlstring(L, (const char *)src->ptr, src->len);

  t = lua_pcall(L, 2, 3, 0); 
  if (t != LUA_OK) { 
    display("load() failed"); 
    /* TODO: Return error msg */
    LOGstack(L);
    lua_settop(L, 0);
    return ERR_ENGINE_CALL_FAILED; 
  } 

  if (lua_isboolean(L, -3)) {
    *ok = lua_toboolean(L, -3);
    LOGf("load() %s\n", ok ? "succeeded" : "failed");
  }
  
  if (lua_isstring(L, -2)) {
    temp_str = (unsigned char *)lua_tolstring(L, -2, &temp_len);
    *pkgname = rosie_new_string(temp_str, temp_len);
  }
  else {
    pkgname = NULL;
  }
  
  temp_rs = to_json_string(L, -1);
    if (temp_rs == NULL) {
      lua_settop(L, 0);
      return ERR_ENGINE_CALL_FAILED;
    }
  errors->len = temp_rs->len;
  errors->ptr = temp_rs->ptr;

  lua_settop(L, 0);
  return SUCCESS;
}

void rosie_finalize(void *L) {
  get_registry(prev_string_result_key);
  if (lua_isuserdata(L, -1)) {
    str *rs = lua_touserdata(L, -1);
    rosie_free_string_ptr(rs);
    lua_pop(L, 1);
  }
  lua_close(L);
}
