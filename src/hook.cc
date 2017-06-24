// TODO split this out or something idk
extern "C" {
#include <dlfcn.h>
#include <dirent.h>
}

#include <iostream>
#include <list>
#include <string>
#include <subhook.h>

#if defined(BLT_USING_LIBCXX) // not used otherwise, no point in wasting compile time :p
#include <vector>
#include <map>
#include <mutex>
#endif

#include <blt/hook.hh>
#include <blt/http.hh>
#include <blt/lapi.hh>
#include <blt/log.hh>
#include <blt/event.hh>
#include <blt/lapi_version.hh>


/**
 * Shorthand to reinstall a hook when a function exits, like a trampoline
 */
#define hook_remove(hookName) SubHook::ScopedRemove _sh_remove_raii(&hookName)

namespace blt {

    using std::cerr;
    using std::cout;
    using std::string;

    void        (*lua_call)         (lua_state*, int, int);
    int         (*lua_pcall)        (lua_state*, int, int, int);
    int         (*lua_gettop)       (lua_state*);
    void        (*lua_settop)       (lua_state*, int);
    const char* (*lua_tolstring)    (lua_state*, int, size_t*);
    int         (*luaL_loadfile)    (lua_state*, const char*);
    int         (*lua_load)         (lua_state*, lua_reader*, void*, const char*);
    void        (*lua_setfield)     (lua_state*, int, const char*);
    void        (*lua_createtable)  (lua_state*, int, int);
    void        (*lua_insert)       (lua_state*, int);
    lua_state*  (*lua_newstate)     (lua_alloc, void*);
    lua_state*  (*luaL_newstate)    (void);
    void*       (*dsl_lua_newstate) (lua_state** /* this */, bool, bool, bool);
    void        (*lua_close)        (lua_state*);
    void        (*lua_rawset)       (lua_state*, int);
    void        (*lua_settable)     (lua_state*, int);
    void        (*lua_pushnumber)   (lua_state*, double);
    void        (*lua_pushinteger)  (lua_state*, ptrdiff_t);
    void        (*lua_pushboolean)  (lua_state*, bool);
    void        (*lua_pushcclosure) (lua_state*, lua_cfunction, int);
    void        (*lua_pushlstring)  (lua_state*, const char*, size_t);
    void        (*luaL_openlib)     (lua_state*, const char*, const luaL_Reg*, int);
    int         (*luaL_ref)         (lua_state*, int);
    void        (*lua_rawgeti)      (lua_state*, int, int);
    void        (*luaL_unref)       (lua_state*, int, int);

    void*       (*do_game_update)   (void* /* this */);


    /*
     * Internal
     */

    std::list<lua_state*> activeStates;


    /*
     * State Management
     */

    void
    add_active_state(lua_state* state)
    {
        activeStates.push_back(state);
    }

    void
    remove_active_state(lua_state* state)
    {
        activeStates.remove(state);
    }

    bool
    check_active_state(lua_state* state)
    {
        std::list<lua_state*>::iterator stateIterator;

        for (stateIterator = activeStates.begin();
             stateIterator != activeStates.end();
             ++stateIterator) // is ++operator implemented? I guess we'll find out
        {
            // is this a real pointer.
            // lol C++
            if (*stateIterator == state)
            {
                return true;
            }
        }

        return false;
    }

    /*
     * Detour Impl
     */

    SubHook     gameUpdateDetour;
    SubHook     luaNewStateDetour;
    SubHook     luaCallDetour;
    SubHook     luaCloseDetour;

    void*
    dt_Application_update(void* parentThis)
    {
        hook_remove(gameUpdateDetour);

        if (HTTPManager::get_instance()->locks_initd() == false)
        {
            HTTPManager::get_instance()->init_locks();
        }

        event::EventQueue::get_instance()->process_events();

        return do_game_update(parentThis);
    }

    void
    dt_lua_call(lua_state* state, int argCount, int resultCount)
    {
        hook_remove(luaNewStateDetour);

        int result = lua_pcall(state, argCount, resultCount, 0);

        if (result != 0)
        {
            size_t len;
            log::log("lua_call: error in lua_pcall: " + string(lua_tolstring(state, -1, &len)), log::LOG_ERROR);
        }
    }

    void*
    dt_dsl_lua_newstate(lua_state** pThis, bool b1, bool b2, bool allocator)
    {
#       define lua_mapfn(name, function) \
            lua_pushcclosure(state, function, 0); \
            lua_setfield(state, LUAGlobalsIndex, name);

        hook_remove(luaNewStateDetour);

        void* returnVal = dsl_lua_newstate(pThis, b1, b2, allocator);
        lua_state* state = *pThis; // wut

        if (!state)
        {
            return returnVal;
        }

        add_active_state(state);

        int stackSize = lua_gettop(state);

        log::log("installing BLT LUA API", log::LOG_INFO);

        /*
         * Install BLT API-extensions in to the LUA context
         */

        lua_mapfn("pcall",      lapi::pcall);
        lua_mapfn("dofile",     lapi::loadfile);
        lua_mapfn("dohttpreq",  lapi::dohttpreq);
        lua_mapfn("log",        lapi::log);
        lua_mapfn("unzip",      lapi::unzip);

        /*
         * Map native libraries
         */

        {
            luaL_Reg consoleLib[] = {
                { "CreateConsole",  lapi::console_noop },
                { "DestroyConsole", lapi::console_noop },
                { NULL, NULL }
            };
            luaL_openlib(state, "console", consoleLib, 0);

            luaL_Reg fileLib[] = {
                { "GetDirectories",     lapi::getdir        },
                { "GetFiles",           lapi::getfiles      },
                { "RemoveDirectory",    lapi::removedir     },
                { "CreateDirectory",    lapi::createdir     },
                { "DirectoryExists",    lapi::dir_exists    },
                { NULL, NULL }
            };
            luaL_openlib(state, "file", fileLib, 0);

            luaL_Reg bltLib[] = {
                { "PlatformName",       lapi::blt_platform  },
                { NULL, NULL }
            };
            luaL_openlib(state, "BLT", bltLib, 0);
        }


        log::log("Loading BLT Base");

        {
            int result;

            result = luaL_loadfile(state, "mods/base/base.lua");
            log::log("luaL_loadfile() = " + std::to_string(result), log::LOG_INFO);

            if (result == LUAErrSyntax) 
            {
                size_t len;
                log::log("Loading BLT Base failed (Syntax Error)", log::LOG_ERROR);
                log::log(lua_tolstring(state, -1, &len), log::LOG_ERROR);
                return returnVal;
            }

            result = lua_pcall(state, 0, 1, 0);
            log::log("lua_pcall(...) = " + std::to_string(result), log::LOG_INFO);

            if (result == LUAErrRun)
            {
                size_t len;
                log::log("Loading BLT Base failed (Runtime Error)", log::LOG_ERROR);
                log::log(lua_tolstring(state, -1, &len), log::LOG_ERROR);
                return returnVal;
            }
        }

        lua_settop(state, stackSize);

        return returnVal;
#       undef lua_mapfn
    }

    void
    dt_lua_close(lua_state* state)
    {
        hook_remove(luaCloseDetour);

        remove_active_state(state);
        lua_close(state);
    }

#if defined(BLT_USING_LIBCXX)
    // Track DFS root dirs by their instance pointers
    std::map<void*, std::string> dfsRootsByInstance;
    std::mutex mx_dfsRootsByInstance;

    /**
     * uber-simple and highly effective mod_overrides fix
     * Requires libcxx for implementation-level compatibility with PAYDAY
     */

    SubHook     sh_dsl_dfs_set_base_path;
    void        (*dsl_dfs_set_base_path)(void*, std::string const*);

    /**
     * Diesel VFS setup calls this to configure DFS instances and point them at a folder.
     * That folder is their absolute root.
     *
     * Since we need that information for the mod_overrides fix, we capture it for later use
     */
    void
    dt_dsl_dfs_set_base_path(void* _this, std::string const* base_path)
    {
        hook_remove(sh_dsl_dfs_set_base_path);
        dsl_dfs_set_base_path(_this, base_path);

        std::lock_guard<std::mutex> guard(mx_dfsRootsByInstance);
        dfsRootsByInstance[_this] = std::string(*base_path); // duplicate base path
        printf("Captured DiskFileSystem %p root (%s)\n",
                _this, base_path->c_str());
    }

    SubHook     sh_dsl_dfs_list_all;
    void        (*dsl_dfs_list_all)(void*, std::vector<std::string>*, std::vector<std::string>*, 
                                    std::string const*);

    /**
     * Diesel FS API function used to list files in a directory.
     * Non-recursive
     *
     * @param _this         instance pointer
     * @param subfiles      subfiles, sometimes null
     * @param subfolders    subfolders listed within, relative names
     * @param dir           path relative to filesystem object root
     */
    void
    dt_dsl_dfs_list_all(void* _this, std::vector<std::string>* subfiles, std::vector<std::string>* subfolders,
                        std::string const* dir)
    {
        // XXX naive
        std::string dfsBase = dfsRootsByInstance[_this];

        // List dirents
        DIR* entries = opendir((dfsBase + "/" + (*dir)).c_str());

        if (entries)
        {
            struct dirent* entry = NULL;
            // Skip specials
            for(uint8_t idx = 0; idx < 2; ++idx) readdir(entries);

            while ((entry = readdir(entries)))
            {
                if ((entry->d_type == DT_DIR) && subfolders)
                {
                    subfolders->push_back(std::string(entry->d_name));
                }
                else if (subfiles)
                {
                    subfiles->push_back(std::string(entry->d_name));
                }
            }
        }
    }

    // --------------------------
#endif

    void
    blt_init_hooks(void* dlHandle)
    {
#       define setcall(symbol,ptr) *(void**) (&ptr) = dlsym(dlHandle, #symbol); \

        log::log("finding lua functions", log::LOG_INFO);

        /*
         * DL Init
         */

        {
            setcall(lua_call,           lua_call);
            setcall(lua_pcall,          lua_pcall);
            setcall(lua_gettop,         lua_gettop);
            setcall(lua_settop,         lua_settop);
            setcall(lua_tolstring,      lua_tolstring);
            setcall(luaL_loadfile,      luaL_loadfile);
            setcall(lua_load,           lua_load);
            setcall(lua_setfield,       lua_setfield);
            setcall(lua_createtable,    lua_createtable);
            setcall(lua_insert,         lua_insert);
            setcall(lua_newstate,       lua_newstate);
            setcall(lua_close,          lua_close);
            setcall(lua_rawset,         lua_rawset);
            setcall(lua_settable,       lua_settable);
            setcall(lua_pushnumber,     lua_pushnumber);
            setcall(lua_pushinteger,    lua_pushinteger);
            setcall(lua_pushboolean,    lua_pushboolean);
            setcall(lua_pushcclosure,   lua_pushcclosure);
            setcall(lua_pushlstring,    lua_pushlstring);
            setcall(luaL_openlib,       luaL_openlib);
            setcall(luaL_ref,           luaL_ref);
            setcall(lua_rawgeti,        lua_rawgeti);
            setcall(luaL_unref,         luaL_unref);
            setcall(luaL_newstate,      luaL_newstate);

            // _ZN3dsl12LuaInterface8newstateEbbNS0_10AllocationE = dsl::LuaInterface::newstate(...) 
            setcall(_ZN3dsl12LuaInterface8newstateEbbNS0_10AllocationE, dsl_lua_newstate); 

            // _ZN11Application6updateEv = Application::update()
            setcall(_ZN11Application6updateEv, do_game_update); 

#if defined(BLT_USING_LIBCXX)
            // dsl::DiskFileSystem
            // Set the base path for a disk-based VFS backend
            setcall(_ZN3dsl14DiskFileSystem13set_base_pathERKNSt3__112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE, dsl_dfs_set_base_path);
            // List entries in a directory within a native-fs based VFS backend (DiskFileSystem)
            setcall(_ZNK3dsl14DiskFileSystem8list_allEPNSt3__16vectorINS1_12basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEENS6_IS8_EEEESB_RKS8_,
                    dsl_dfs_list_all);
#endif
        }

        log::log("installing hooks", log::LOG_INFO);

        /*
         * Intercept Init
         */

        {
            gameUpdateDetour.Install    ((void*) do_game_update,    (void*) dt_Application_update);
            luaNewStateDetour.Install   ((void*) dsl_lua_newstate,  (void*) dt_dsl_lua_newstate);
            luaCloseDetour.Install      ((void*) lua_close,         (void*) dt_lua_close);
            luaCallDetour.Install       ((void*) lua_call,          (void*) dt_lua_call);

#if defined(BLT_USING_LIBCXX)
            // dsl::DiskFileSystem::set_base_path() - Hook captures DFS paths for use later
            sh_dsl_dfs_set_base_path
                .Install((void*) dsl_dfs_set_base_path, (void*) dt_dsl_dfs_set_base_path);

            // dsl::DiskFileSystem::list_all() - Completely replaces defective list_all function
            sh_dsl_dfs_list_all
                .Install((void*) dsl_dfs_list_all, (void*) dt_dsl_dfs_list_all);
#endif
        }

#       undef setcall
    }

}

/* vim: set ts=4 softtabstop=0 sw=4 expandtab: */
