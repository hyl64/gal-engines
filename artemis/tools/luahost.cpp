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
#include <map>
#include <algorithm>

#include "../src/pngio.h"

// ---------------------------------------------------------------------------
// the mini renderer: layers by id, composited bottom-up onto one canvas
// ---------------------------------------------------------------------------
struct Layer
{
    artemis::Image img;
    int x = 0, y = 0;
    int cx = 0, cy = 0, cw = 0, ch = 0;   // the clip within the source image
    int alpha = 255;
    bool visible = true;
    long order = 0;                       // insertion order = z order
};

static std::map<std::string, Layer> g_layers;
static long g_layerOrder = 0;
static std::string g_rootDir = ".";

// the click bindings the lyevent tags register: key -> layer id
static std::map<std::string, std::string> g_clickKeys;
static void TagLyevent(lua_State *L, int idx)
{
    lua_getfield(L, idx, "type");
    const char *type = lua_tostring(L, -1);
    lua_pop(L, 1);
    if (!type || strcmp(type, "click")) return;      // only clicks matter here
    lua_getfield(L, idx, "id");
    const char *id = lua_tostring(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, idx, "key");
    const char *key = lua_tostring(L, -1);
    lua_pop(L, 1);
    if (id && key) g_clickKeys[key] = id;
}

static bool ParseInts(const std::string& text, int* out, int count)
{
    // "a,b,c,.." -> ints; returns false when any field is missing
    size_t pos = 0;
    for (int i = 0; i < count; i++){
        if (pos >= text.size()) return false;
        out[i] = atoi(text.c_str() + pos);
        size_t comma = text.find(',', pos);
        if (comma == std::string::npos) pos = text.size();
        else pos = comma + 1;
    }
    return true;
}

static void TagLyc(lua_State *L, int idx)
{
    // e:tag{"lyc", id=..., file=...}: create a layer holding the image
    lua_getfield(L, idx, "id");
    const char *id = lua_tostring(L, -1);
    lua_getfield(L, idx, "file");
    const char *file = lua_tostring(L, -1);
    if (!id || !file){ lua_pop(L, 2); return; }
    Layer layer;
    layer.order = g_layerOrder++;
    if (!strncmp(file, ":bg/", 4)){
        // an engine built-in: ":bg/black" is a black screen
        layer.img.width = 1280; layer.img.height = 720;
        layer.img.rgba.assign((size_t)layer.img.width * layer.img.height * 4, 0);
    } else {
        std::string path = g_rootDir + "/" + file + ".png";
        std::string why;
        if (!artemis::ReadPng(path, layer.img, why))
            fprintf(stderr, "lyc: %s: %s\n", path.c_str(), why.c_str());
    }
    lua_pop(L, 2);
    g_layers[id] = layer;
}

static void TagLyprop(lua_State *L, int idx)
{
    lua_getfield(L, idx, "id");
    const char *id = lua_tostring(L, -1);
    lua_pop(L, 1);
    if (!id) return;
    auto it = g_layers.find(id);
    if (it == g_layers.end()){
        /* a logical parent (the engine groups 500.d.2 over 500.d.2.0 without
         * ever creating a layer for it): the position still belongs to the
         * children, so hand it straight down */
        const std::string prefix = std::string(id) + ".";
        bool hit = false;
        for (auto &kv : g_layers){
            if (kv.first.compare(0, prefix.size(), prefix) == 0){
                Layer &child = kv.second;
                if (lua_getfield(L, idx, "left"), lua_isnumber(L, -1)){ child.x = (int)lua_tointeger(L, -1); }
                lua_pop(L, 1);
                if (lua_getfield(L, idx, "top"), lua_isnumber(L, -1)){ child.y = (int)lua_tointeger(L, -1); }
                lua_pop(L, 1);
                hit = true;
            }
        }
        (void)hit;
        return;
    }
    Layer &layer = it->second;

    struct Field { const char *name; int *target; };
    int x = layer.x, y = layer.y, left = layer.x, top = layer.y;
    (void)left; (void)top;
    bool movedX = false, movedY = false;
    lua_getfield(L, idx, "x");
    if (lua_isnumber(L, -1)){ layer.x = (int)lua_tointeger(L, -1); movedX = true; }
    lua_pop(L, 1);
    lua_getfield(L, idx, "y");
    if (lua_isnumber(L, -1)){ layer.y = (int)lua_tointeger(L, -1); movedY = true; }
    lua_pop(L, 1);
    lua_getfield(L, idx, "left");
    if (lua_isnumber(L, -1)){ layer.x = (int)lua_tointeger(L, -1); movedX = true; }
    lua_pop(L, 1);
    lua_getfield(L, idx, "top");
    if (lua_isnumber(L, -1)){ layer.y = (int)lua_tointeger(L, -1); movedY = true; }
    lua_pop(L, 1);
    lua_getfield(L, idx, "alpha");
    if (lua_isnumber(L, -1)) layer.alpha = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, idx, "visible");
    if (lua_isnumber(L, -1)) layer.visible = lua_tointeger(L, -1) != 0;
    lua_pop(L, 1);
    const bool moved = movedX || movedY;
    lua_getfield(L, idx, "clip");
    if (const char *clip = lua_tostring(L, -1)){
        int v[4];
        if (ParseInts(clip, v, 4)){ layer.cx = v[0]; layer.cy = v[1]; layer.cw = v[2]; layer.ch = v[3]; }
    }
    lua_pop(L, 1);

    /* the engine's layer tree: positioning 500.d.2 moves its child layers
     * (500.d.2.0 - the clickable slices) with it.  A moved parent repositions
     * every layer whose id extends this one with a dot. */
    if (moved){
        const std::string prefix = std::string(id) + ".";
        for (auto &kv : g_layers){
            if (kv.first.compare(0, prefix.size(), prefix) == 0){
                kv.second.x = layer.x;
                kv.second.y = layer.y;
            }
        }
    }
}

static void TagLydel(lua_State *L, int idx)
{
    lua_getfield(L, idx, "id");
    const char *id = lua_tostring(L, -1);
    lua_pop(L, 1);
    if (id) g_layers.erase(id);
}

static std::vector<std::string> g_called;

static int debugOn()
{
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("ONS_LUA_DEBUG") ? 1 : 0;
    return dbg;
}


static std::vector<std::string> g_tags;   // the e:tag stream

static void DumpLuaTable(lua_State *L, int idx)
{
    // the game calls e:tag{ "name", key=value, ... }: record the name and the
    // named attributes so the render side knows what to draw
    if (lua_type(L, idx) != LUA_TTABLE) return;
    std::string line;
    lua_pushnil(L);
    int i = 0;
    while (lua_next(L, idx) != 0){
        // key at -2, value at -1
        const int kt = lua_type(L, -2), vt = lua_type(L, -1);
        if (i == 0 && kt == LUA_TNUMBER && vt == LUA_TSTRING)
            line += lua_tostring(L, -1);          // the tag name
        else if (kt == LUA_TSTRING){
            const char *k = lua_tostring(L, -2);
            std::string v;
            if (vt == LUA_TSTRING) v = lua_tostring(L, -1);
            else if (vt == LUA_TNUMBER) v = lua_tostring(L, -1);
            else if (vt == LUA_TBOOLEAN) v = lua_toboolean(L, -1) ? "true" : "false";
            if (!v.empty()) line += std::string(" ") + k + "=" + v;
        }
        lua_pop(L, 1);                            // pop the value, keep the key
        i++;
    }
    g_tags.push_back(line);
}

static int engineStub(lua_State *L)
{
    const char *name = lua_tostring(L, lua_upvalueindex(1));
    const char *name2 = lua_isnone(L, lua_upvalueindex(2))
                      ? NULL : lua_tostring(L, lua_upvalueindex(2));
    const std::string called = name2 ? name2 : (name ? name : "?");
    g_called.push_back(called);
    /* e:tag{...} is the game's draw command stream: record it */
    /* e:tag{...} is a colon call: arg 1 is the e object itself, the tag
     * table is arg 2 */
    if ((called == "tag" || called == "e.tag") && lua_istable(L, 2)){
        if (debugOn()) DumpLuaTable(L, 2);
        /* the render side: the tags that place pixels */
        lua_getfield(L, 2, "1");                      /* e:tag{"name", ...}: [1] = name.
                                                         getfield with the numeric key
                                                         needs lua_pushinteger instead */
        lua_pop(L, 1);
        lua_pushinteger(L, 1);
        lua_gettable(L, 2);
        const char *tagName = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
        lua_pop(L, 1);
        if (!strcmp(tagName, "lyc")) TagLyc(L, 2);
        else if (!strcmp(tagName, "lyprop")) TagLyprop(L, 2);
        else if (!strcmp(tagName, "lydel")) TagLydel(L, 2);
        else if (!strcmp(tagName, "lyevent")) TagLyevent(L, 2);
    }
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

    /* Sensible defaults for the common case - a game extracted under a root
     * that holds system/, with the title Lua named on the command line:
     *   luahost <game>/system/ui/title.lua
     * boots, initializes and renders <game>/system/ui/title-rendered.png
     * without any environment setup. */
    {
        std::string self = argv[1];
        size_t sl = self.find_last_of('/');
        std::string selfDir = (sl == std::string::npos) ? "." : self.substr(0, sl);
        if (!getenv("ONS_LUA_ROOT"))
            setenv("ONS_LUA_ROOT", ".", 1);   /* the game root is the cwd */
        if (!getenv("ONS_LUA_TABLES"))
            setenv("ONS_LUA_TABLES", "gscr,flg,sv,scr,csv,game", 1);
        if (!getenv("ONS_RENDER")){
            std::string out = selfDir + "/title-rendered.png";
            setenv("ONS_RENDER", out.c_str(), 1);
        }
        g_rootDir = getenv("ONS_LUA_ROOT") ? getenv("ONS_LUA_ROOT") : ".";
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

    // ONS_CLICK="x,y": simulate a click - hit-test the lyevent bindings, run
    // the game's own over/click chain (btn_over sets btn.cursor, btn_click
    // executes the button), then the post-click state renders below
    if (const char *click = getenv("ONS_CLICK")){
        int cx = 0, cy = 0;
        if (ParseInts(click, &cx, 1)){
            int v2[2] = {cx, cy};
            if (ParseInts(click, v2, 2)){ cx = v2[0]; cy = v2[1]; }
            std::string hitKey;
            const Layer *hitLayer = NULL;
            long hitOrder = -1;
            for (const auto &kv : g_clickKeys){
                auto it = g_layers.find(kv.second);
                if (it == g_layers.end()) continue;
                const Layer &layer = it->second;
                const int w = layer.cw > 0 ? layer.cw : layer.img.width;
                const int h = layer.ch > 0 ? layer.ch : layer.img.height;
                if (cx >= layer.x && cx < layer.x + w && cy >= layer.y && cy < layer.y + h
                    && layer.order > hitOrder){
                    hitOrder = layer.order;
                    hitKey = kv.first;
                    hitLayer = &layer;
                }
            }
            if (!hitKey.empty()){
                printf("click at %d,%d hit %s (key=%s)\n", cx, cy,
                       hitLayer ? hitLayer->img.width ? hitKey.c_str() : "?" : "?",
                       hitKey.c_str());
                // the game's own handlers, with the game's own conventions:
                // btn_over(e, {key=...}) then btn_click(e, {click=true, key=...})
                lua_getglobal(L, "btn_over");
                if (lua_isfunction(L, -1)){
                    lua_getglobal(L, "e");            /* self */
                    lua_newtable(L);
                    lua_pushstring(L, "key");
                    lua_pushstring(L, hitKey.c_str());
                    lua_settable(L, -3);
                    if (lua_pcall(L, 2, 0, 0)){
                        fprintf(stderr, "btn_over: %s\n", lua_tostring(L, -1));
                        lua_pop(L, 1);
                    }
                } else lua_pop(L, 1);
                lua_getglobal(L, "btn_click");
                if (lua_isfunction(L, -1)){
                    lua_getglobal(L, "e");            /* self */
                    lua_newtable(L);
                    lua_pushstring(L, "click");
                    lua_pushboolean(L, 1);
                    lua_settable(L, -3);
                    lua_pushstring(L, "key");
                    lua_pushstring(L, hitKey.c_str());
                    lua_settable(L, -3);
                    if (lua_pcall(L, 2, 0, 0)){
                        fprintf(stderr, "btn_click: %s\n", lua_tostring(L, -1));
                        lua_pop(L, 1);
                    }
                } else lua_pop(L, 1);
            } else {
                printf("click at %d,%d: no button there\n", cx, cy);
                if (debugOn())
                    for (const auto &kv : g_clickKeys){
                        auto it = g_layers.find(kv.second);
                        if (it == g_layers.end())
                            printf("  %s -> %s: no layer\n", kv.first.c_str(), kv.second.c_str());
                        else
                            printf("  %s -> %s at %d,%d %dx%d\n", kv.first.c_str(),
                                   kv.second.c_str(), it->second.x, it->second.y,
                                   it->second.cw ?: it->second.img.width,
                                   it->second.ch ?: it->second.img.height);
                    }
            }
        }
    }

    // the composite: every layer bottom-up, clipped, alpha-blended
    if (const char *render = getenv("ONS_RENDER")){
        const int W = 1280, H = 720;
        std::vector<uint8_t> canvas((size_t)W * H * 4, 0);
        for (int y = 0; y < H; y++)          // a black base, like :bg/black
            for (int x = 0; x < W; x++){
                uint8_t *p = &canvas[((size_t)y * W + x) * 4];
                p[0] = p[1] = p[2] = 0; p[3] = 255;
            }
        std::vector<const Layer*> order;
        for (const auto &kv : g_layers)
            if (kv.second.visible && !kv.second.img.empty()) order.push_back(&kv.second);
        std::sort(order.begin(), order.end(),
                  [](const Layer *a, const Layer *b){ return a->order < b->order; });
        for (const Layer *layer : order){
            const int sw = layer->cw > 0 ? layer->cw : layer->img.width;
            const int sh = layer->ch > 0 ? layer->ch : layer->img.height;
            const int sx = layer->cx, sy = layer->cy;
            for (int y = 0; y < sh; y++){
                const int dy = layer->y + y;
                if (dy < 0 || dy >= H) continue;
                for (int x = 0; x < sw; x++){
                    const int dx = layer->x + x;
                    if (dx < 0 || dx >= W) continue;
                    const int sxp = sx + x, syp = sy + y;
                    if (sxp < 0 || syp < 0 || sxp >= layer->img.width || syp >= layer->img.height) continue;
                    const uint8_t *sp = &layer->img.rgba[((size_t)syp * layer->img.width + sxp) * 4];
                    uint8_t *dp = &canvas[((size_t)dy * W + dx) * 4];
                    const int sa = (sp[3] * layer->alpha) / 255;
                    for (int c = 0; c < 3; c++)
                        dp[c] = (uint8_t)((sp[c] * sa + dp[c] * (255 - sa)) / 255);
                    dp[3] = 255;
                }
            }
        }
        std::string why;
        if (artemis::WritePng(render, W, H, canvas.data(), why))
            printf("rendered %s (%zu layers)\n", render, order.size());
        else
            fprintf(stderr, "render: %s\n", why.c_str());
    }

    if (getenv("ONS_LUA_DEBUG")){
        FILE *f = fopen(getenv("ONS_TAG_DUMP") ? getenv("ONS_TAG_DUMP") : "tags.txt", "w");
        if (f){
            for (const std::string &t : g_tags) fprintf(f, "%s\n", t.c_str());
            fclose(f);
            fprintf(stderr, "tags: %zu written\n", g_tags.size());
        }
    }
    printf("engine calls: %zu\n", g_called.size());
    for (size_t i = 0; i < g_called.size() && i < 24; i++)
        printf("  %s\n", g_called[i].c_str());

    lua_close(L);
    return rc;
}
