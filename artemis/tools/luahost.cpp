// luahost - stage 1 of the Artemis Lua host: embed Lua 5.1, register the
// engine API the game's UI Lua calls, load a real title's Lua, and report
// which engine functions the game actually invokes.
//
//   luahost <game.lua> [more.lua ...]
//
// The API surface comes from docs/artemis-lua-host.md (extracted from the
// retained system_ui_title.lua of the pf8 title).

extern "C" {
#include "../third_party/lua/lua.h"
#include "../third_party/lua/lauxlib.h"
#include "../third_party/lua/lualib.h"
}

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Every engine function the game's Lua may call.  Each stub records its name
// and returns a benign value; stage 2 replaces the interesting ones with real
// implementations.
static std::vector<std::string> g_called;

static int engineStub(lua_State *L)
{
    const char *name = lua_tostring(L, lua_upvalueindex(1));
    g_called.push_back(name ? name : "?");
    lua_pushinteger(L, 0);
    return 1;
}

// get_eval("g.rur"): walk the dotted path from the global table, creating
// missing tables on the way, and return the value (nil reads as 0).  This is
// the state dual in miniature - the game's Lua reads engine variables through
// it, and the values live in the host's tables.
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
        lua_remove(L, -2);                           // drop the parent
        pos = dot + 1;
    }
    if (lua_isnil(L, -1)){ lua_pop(L, 1); lua_pushinteger(L, 0); }
    return 1;
}

static int engineStubNil(lua_State *L)
{
    const char *name = lua_tostring(L, lua_upvalueindex(1));
    g_called.push_back(name ? name : "?");
    lua_pushnil(L);
    return 1;
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
        fprintf(stderr, "usage: %s <game.lua> [more.lua ...]\n", argv[0]);
        return 2;
    }

    lua_State *L = luaL_newstate();
    if (!L){ fprintf(stderr, "lua: state allocation failed\n"); return 1; }
    luaL_openlibs(L);

    // The engine API surface from the retained title Lua.  Each stub is an
    // upvalue-named closure so the trace says who was called.
    static const char *kStubsInt[] = {
        "get_eval", "tn", "estag", "message", "ui_message", "uitrans",
        "se_ok", "se_cancel", "sysvo", "allkeyoff", "allkeyon", "key_reset",
        "patch_check", "get_language", "get_trial", "get_savedatatime",
        "getScriptStartup", "getTitle", "getExtra", "loading_off",
        "reset_backlog", "vartable_init", "screen_crop", "mulpos",
        "adv_config", "adv_exit", "adv_flagreset", "adv_info", "adv_load",
        "adv_qload", "titlecall", "ResetStack",
    };
    static const char *kStubsTable[] = {
        "getBtnInfo", "getBtnID", "setBtnStat", "setonpush_ui",
        "btn_active", "delbtn", "csvbtn3",
    };
    registerStub(L, "get_eval", engineGetEval);   // real: the state dual
    for (const char *n : kStubsInt) if (strcmp(n, "get_eval")) registerStub(L, n, engineStub);
    for (const char *n : kStubsTable) registerStub(L, n, engineStubNil);

    // The game's global state tables, auto-vivifying: gscr.scene.rur_hex = true
    // style deep assignments land in real tables even when the path is new.
    // Which globals get the treatment comes from ONS_LUA_TABLES (a comma list);
    // a fresh title reveals them one "attempt to index global" at a time, and
    // the driver extends the list until title_init completes.
    const char *tbls = getenv("ONS_LUA_TABLES");
    if (tbls && *tbls){
        char *names = strdup(tbls);
        for (char *n = strtok(names, ","); n; n = strtok(NULL, ",")){
            lua_newtable(L);
            lua_newtable(L);                                  // the metatable
            lua_pushstring(L, "k");
            lua_pushcclosure(L, [](lua_State *L) -> int {     // __index: create + return
                lua_newtable(L);
                lua_pushvalue(L, -1);
                lua_rawset(L, 1);                             // t[k] = {} (t at stack 1)
                return 1;
            }, 1);
            lua_setfield(L, -2, "__index");
            lua_setmetatable(L, -2);
            lua_setglobal(L, n);
        }
        free(names);
    }

    int rc = 0;
    for (int i = 1; i < argc; i++){
        if (luaL_dofile(L, argv[i])){
            fprintf(stderr, "luahost: %s failed: %s\n", argv[i], lua_tostring(L, -1));
            rc = 1;
        } else {
            printf("loaded %s\n", argv[i]);
        }
    }

    // The title entry point the real UI calls at init.
    lua_getglobal(L, "title_init");
    if (lua_isfunction(L, -1)){
        if (lua_pcall(L, 0, 0, 0)){
            fprintf(stderr, "luahost: title_init failed: %s\n", lua_tostring(L, -1));
            rc = 1;
        } else {
            printf("title_init ran\n");
        }
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
