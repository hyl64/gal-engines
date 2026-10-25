// luahost - the Artemis Lua host, stage 2: Lua 5.1.5 embedded, the game's
// Lua files load in the order system_loadinglua() prescribes, the csv data
// layer comes from the platform tbl files (which define the csv global
// themselves), and the engine's `e` object hands out named method stubs.
//
//   ONS_LUA_ROOT=<dir>      the archive root (holds system/)
//   ONS_LUA_ORDER=<file>    the load order, one "system/..." entry per line
//   ONS_LUA_TABLES=<list>   globals that auto-vivify (gscr,flg,sv,scr,csv,game)
//   ONS_LUA_DEBUG=1         the trace
//
//   luahost <game.lua>
//
// Lessons that shaped this file, each paid for with a crash:
//  - a stub must return false, not 0: in Lua every value but nil/false is
//    truthy, and a 0 return made "if getTrial() then" take the trial branch,
//    which then indexed csv with nil;
//  - lua_upvalueindex(n) past a closure's real upvalue count is out of
//    bounds - the one-upvalue stubs must not read upvalue 2;
//  - luaL_dofile's error message stays on the stack - every failure has to
//    pop it, or the stack desyncs and a later setglobal assigns the error
//    text to a global (csv literally became "cannot open ...");
//  - lua_rawset with a nil key segfaults in 5.1 (no type check), so the
//    auto-vivify __index guards the key type first;
//  - a metatable on a non-table (a string global) corrupts the object in
//    release builds - check lua_istable before setmetatable.

extern "C" {
#include "../third_party/lua/lua.h"
#include "../third_party/lua/lauxlib.h"
#include "../third_party/lua/lualib.h"
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <vector>

static std::vector<std::string> g_called;

static int engineStub(lua_State *L)
{
    const char *name = lua_tostring(L, lua_upvalueindex(1));
    const char *name2 = lua_isnone(L, lua_upvalueindex(2))
                      ? NULL : lua_tostring(L, lua_upvalueindex(2));
    g_called.push_back(name2 ? name2 : (name ? name : "?"));
    lua_pushboolean(L, 0);                    /* false: the falsy default */
    return 1;
}

/* get_eval("g.rur"): walk the dotted path from the global table, creating
 * missing tables on the way; nil reads as 0.  The state dual in miniature. */
static int engineGetEval(lua_State *L)
{
    const char *expr = lua_tostring(L, 1);
    g_called.push_back("get_eval");
    if (!expr || !*expr){ lua_pushinteger(L, 0); return 1; }

    std::string path = expr;
    lua_pushvalue(L, LUA_GLOBALSINDEX);
    size_t pos = 0;
    while (pos <= path.size()){
        size_t dot = path.find('.', pos);
        if (dot == std::string::npos) dot = path.size();
        std::string seg = path.substr(pos, dot - pos);
        lua_getfield(L, -1, seg.c_str());
        if (lua_isnil(L, -1)){
            lua_pop(L, 1);
            lua_newtable(L);
            lua_setfield(L, -2, seg.c_str());
            lua_getfield(L, -1, seg.c_str());
        }
        lua_remove(L, -2);                    /* drop the parent */
        pos = dot + 1;
    }
    if (lua_isnil(L, -1)){ lua_pop(L, 1); lua_pushinteger(L, 0); }
    return 1;
}

/* get_language(force): the web trial ships Japanese; the real engine reads
 * the language from the system config.  "ja" matches init.lang's key. */
static int engineGetLanguage(lua_State *L)
{
    g_called.push_back("get_language");
    lua_pushstring(L, "ja");
    return 1;
}

/* e:var(name): read an engine variable.  The screen size comes from the
 * trial's game_scale ({1280,720} in the platform tbl); everything else reads
 * as false until stage 3 wires the real variable area. */
static int engineEVar(lua_State *L)
{
    const char *name = lua_tostring(L, 2);
    g_called.push_back("e.var");
    if (name && !strcmp(name, "game.width")) lua_pushinteger(L, 1280);
    else if (name && !strcmp(name, "game.height")) lua_pushinteger(L, 720);
    else if (name && !strcmp(name, "game.os")) lua_pushstring(L, "wasm");
    else lua_pushboolean(L, 0);
    return 1;
}

/* e:getScriptStack(): the script stack as a list; ResetStack walks it with
 * #ss.  The host stack is empty for now. */
static int engineGetScriptStack(lua_State *L)
{
    g_called.push_back("e.getScriptStack");
    lua_newtable(L);
    return 1;
}

/* e:include(path): the game loads its data files (the platform tbl) through
 * the engine.  dofile them relative to the archive root. */
static int engineInclude(lua_State *L)
{
    const char *path = lua_tostring(L, 2);    /* (self, path) */
    g_called.push_back("e.include");
    if (path){
        std::string root = getenv("ONS_LUA_ROOT") ? getenv("ONS_LUA_ROOT") : ".";
        std::string full = root + "/" + path;
        if (luaL_dofile(L, full.c_str())){
            fprintf(stderr, "luahost: e:include %s failed: %s\n",
                    full.c_str(), lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int debugOn()
{
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("ONS_LUA_DEBUG") ? 1 : 0;
    return dbg;
}

/* The __index used by the auto-vivifying state tables: create t[k] = {} for a
 * string key; anything else (nil, numbers out of range, ...) returns nil
 * instead of rawset'ing a key Lua 5.1 would segfault on. */
static int autoVivifyIndex(lua_State *L)
{
    if (lua_type(L, 2) != LUA_TSTRING){ lua_pushnil(L); return 1; }
    lua_pushvalue(L, 2);                      /* the key */
    lua_newtable(L);                          /* the value */
    lua_rawset(L, 1);                         /* t[k] = value */
    lua_pushvalue(L, -1);                     /* return the value */
    return 1;
}

static void registerAutoVivify(lua_State *L, const char *name)
{
    lua_newtable(L);
    lua_newtable(L);                          /* its metatable */
    lua_pushstring(L, "k");
    lua_pushcclosure(L, autoVivifyIndex, 1);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_setglobal(L, name);
}

static void registerStub(lua_State *L, const char *name, lua_CFunction fn)
{
    lua_pushstring(L, name);
    lua_pushcclosure(L, fn, 1);
    lua_setglobal(L, name);
}

int main(int argc, char **argv)
{
    if (argc < 2){
        fprintf(stderr, "usage: %s <game.lua>\n", argv[0]);
        return 2;
    }

    lua_State *L = luaL_newstate();
    if (!L){ fprintf(stderr, "lua: state allocation failed\n"); return 1; }
    luaL_openlibs(L);

    registerStub(L, "get_eval", engineGetEval);   /* real: the state dual */

    static const char *kStubs[] = {
        "tn", "estag", "message", "ui_message", "uitrans",
        "se_ok", "se_cancel", "sysvo", "allkeyoff", "allkeyon", "key_reset",
        "patch_check", "get_trial", "get_savedatatime",
        "getScriptStartup", "getTitle", "getExtra", "loading_off",
        "reset_backlog", "screen_crop", "mulpos",
        "adv_config", "adv_exit", "adv_flagreset", "adv_info", "adv_load",
        "adv_qload", "titlecall", "ResetStack",
        "getBtnInfo", "getBtnID", "setBtnStat", "setonpush_ui",
        "btn_active", "delbtn", "csvbtn3",
    };
    registerStub(L, "get_language", engineGetLanguage);
    for (const char *n : kStubs) registerStub(L, n, engineStub);

    // The engine's `e` object: the Lua side drives the engine through
    // e:tag / e:var / e:enqueue / ... (colon calls - self first).  Its
    // metatable hands back a named stub for any method the game touches.
    {
        lua_newtable(L);
        lua_newtable(L);                          /* its metatable */
        lua_pushstring(L, "e");
        lua_pushcclosure(L, [](lua_State *L) -> int {     /* __index(t, k) */
            const char *method = lua_tostring(L, 2);
            if (method && !strcmp(method, "getScriptStack")){
                lua_pushstring(L, "e.getScriptStack");
                lua_pushcclosure(L, engineGetScriptStack, 1);
                return 1;
            }
            if (method && !strcmp(method, "var")){
                lua_pushstring(L, "e.var");
                lua_pushcclosure(L, engineEVar, 1);
                return 1;
            }
            if (method && !strcmp(method, "include")){
                lua_pushstring(L, "e.include");
                lua_pushcclosure(L, engineInclude, 1);
                return 1;
            }
            lua_pushstring(L, "e");
            lua_pushvalue(L, 2);                  /* the method name */
            lua_pushcclosure(L, engineStub, 2);   /* upvalues: "e", name */
            return 1;
        }, 1);
        lua_setfield(L, -2, "__index");
        lua_setmetatable(L, -2);
        lua_setglobal(L, "e");
    }

    // The game's global state tables (auto-vivifying: deep assignments like
    // gscr.scene.rur_hex = true land in real tables).
    if (const char *tbls = getenv("ONS_LUA_TABLES")){
        char *names = strdup(tbls);
        for (char *n = strtok(names, ","); n; n = strtok(NULL, ","))
            registerAutoVivify(L, n);
        free(names);
    }
    // conf is the saved config, read with optional-field access
    // (conf.language or init.game_language): auto-vivify would return a
    // truthy empty table for every unset field and break those or-chains.
    // A plain empty table keeps "unset reads as nil".
    {
        lua_newtable(L);
        lua_setglobal(L, "conf");
    }

    int rc = 0;
    if (luaL_dofile(L, argv[1])){
        fprintf(stderr, "luahost: %s failed: %s\n", argv[1], lua_tostring(L, -1));
        lua_pop(L, 1);
        rc = 1;
    } else if (debugOn())
        printf("loaded %s\n", argv[1]);

    // init.lua comes first: it defines the boot functions the engine calls.
    {
        std::string root = getenv("ONS_LUA_ROOT") ? getenv("ONS_LUA_ROOT") : ".";
        std::string initfile = root + "/system/init.lua";
        if (luaL_dofile(L, initfile.c_str())){
            fprintf(stderr, "luahost: %s failed: %s\n", initfile.c_str(), lua_tostring(L, -1));
            lua_pop(L, 1);
        } else if (debugOn()) printf("loaded %s\n", initfile.c_str());
    }

    // system_loadinglua() does the whole boot: it includes the lua files
    // through e:include, defines the game global, detects the platform and
    // includes the platform's tbl.  The host call replaces the hand-rolled
    // order loop that imitated it.
    lua_getglobal(L, "system_loadinglua");
    if (lua_isfunction(L, -1)){
        if (lua_pcall(L, 0, 0, 0)){
            fprintf(stderr, "luahost: system_loadinglua failed: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        } else printf("system_loadinglua ran\n");
    } else lua_pop(L, 1);

    // The load order system_loadinglua() prescribes (kept as a fallback).
    if (const char *order_file = getenv("ONS_LUA_ORDER")){
        FILE *of = fopen(order_file, "rb");
        if (!of){ fprintf(stderr, "luahost: no order file %s\n", order_file); return 1; }
        char line[256];
        int loaded = 0, failed = 0;
        std::string root = getenv("ONS_LUA_ROOT") ? getenv("ONS_LUA_ROOT") : ".";
        while (fgets(line, sizeof line, of)){
            size_t n = strlen(line);
            while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
            if (!n) continue;
            std::string full = root + "/" + line + ".lua";
            if (debugOn()) fprintf(stderr, "[order] %s\n", line);
            if (luaL_dofile(L, full.c_str())){
                failed++;
                if (failed <= 6)
                    fprintf(stderr, "luahost: %s failed: %s\n",
                            full.c_str(), lua_tostring(L, -1));
                lua_pop(L, 1);        /* the error message: leaving it on the
                                         stack desynced every later push/pop */
            } else loaded++;
        }
        fclose(of);
        printf("order: %d loaded, %d failed\n", loaded, failed);
    }

    // The csv data layer: the engine executes system/table/*.tbl, and the
    // platform file (list_wasm.tbl for the web trial) assigns the csv global
    // itself - the whole csv (ui_title, advkey, mw, voice, ...).  No
    // assembly on this side: a hand-collected sparse table would lose every
    // definition the platform file carries.
    {
        std::string root = getenv("ONS_LUA_ROOT") ? getenv("ONS_LUA_ROOT") : ".";
        std::string tdir = root + "/system/table";
        if (DIR *d = opendir(tdir.c_str())){
            struct dirent *de;
            while ((de = readdir(d))){
                std::string f = de->d_name;
                if (f.size() < 5 || f.substr(f.size()-4) != ".tbl") continue;
                /* the platform files all assign the same globals (init, csv,
                 * ...); loading all of them let readdir order decide which
                 * platform's data won.  This title is the web trial: wasm and
                 * its Japanese variant are the right set. */
                if (f != "check.tbl" && f != "list_wasm.tbl" && f != "list_wasm_ja.tbl")
                    continue;
                std::string full = tdir + "/" + f;
                if (luaL_dofile(L, full.c_str())){
                    fprintf(stderr, "luahost: %s failed: %s\n",
                            full.c_str(), lua_tostring(L, -1));
                    lua_pop(L, 1);
                }
            }
            closedir(d);
        }
        lua_getglobal(L, "csv");
        if (debugOn()) fprintf(stderr, "csv after tbl: %s\n",
                               lua_typename(L, lua_type(L, -1)));
        lua_pop(L, 1);
    }

    // The engine calls system_initialize() after loading: it runs
    // vartable_init, storage_path (which sets game.path.ui through
    // set_uipath), the font setup, key config and the rest of the boot.
    lua_getglobal(L, "system_initialize");
    if (lua_isfunction(L, -1)){
        if (lua_pcall(L, 0, 0, 0)){
            fprintf(stderr, "luahost: system_initialize failed: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        } else printf("system_initialize ran\n");
    } else lua_pop(L, 1);

    // The title entry point.
    lua_getglobal(L, "title_init");
    if (lua_isfunction(L, -1)){
        if (lua_pcall(L, 0, 0, 0)){
            fprintf(stderr, "luahost: title_init failed: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
            rc = 1;
        } else printf("title_init ran\n");
    } else {
        printf("no title_init defined\n");
        lua_pop(L, 1);
    }

    printf("engine calls: %zu\n", g_called.size());
    for (size_t i = 0; i < g_called.size() && i < 24; i++)
        printf("  %s\n", g_called[i].c_str());

    lua_close(L);
    return rc;
}
