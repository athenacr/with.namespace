#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>

#include <libgen.h>

extern "C"
{
#include <lua.h>
LUALIB_API int luaopen_with_exec_c(lua_State *L);
}
#include <luabind/luabind.hpp>
#include <luabind/exception_handler.hpp>

#include "exec.hpp"
#include "exec_defs.hpp"
#include "pipe.hpp"

template<typename T>
void copyCmdFromLua(T &cmd, const luabind::object &obj, const char *errName)
{
    int i = 1;
    for(luabind::iterator iter(obj), end; iter != end; ++iter)
    {
        int keytype = luabind::type(iter.key()), valtype = luabind::type(*iter);
        if(keytype != LUA_TNUMBER)
            throw failure("bad key in %s (number expected, got %s)", errName, lua_typename(obj.interpreter(), keytype));
        if(valtype != LUA_TSTRING)
            throw failure("bad value in %s (string expected, got %s)", errName, lua_typename(obj.interpreter(), valtype));
        int idx = luabind::object_cast<int>(iter.key());
        if(idx != i)
            throw failure("keys must be consecutive in %s; %dth key was %d", errName, i, idx);
        cmd.push_back(luabind::object_cast<const char *>(*iter));
        ++i;
    }
}

static void exec_with_namespace_internal(
        const std::string &devname,
        const luabind::object &namespace_obj,
        const luabind::object &cmd_argv_obj)
{
    std::vector<std::string> cmd_argv, namespace_argv;
    copyCmdFromLua(namespace_argv, namespace_obj, "exec_with_namespace.namespace");
    copyCmdFromLua(cmd_argv, cmd_argv_obj, "exec_with_namespace.cmd");
    exec_with_namespace(devname, namespace_argv, cmd_argv);
}

static std::string luadirname(const std::string &path)
{
    char *buf = strdup(path.c_str()),
         *ret = dirname(buf);
    std::string retStr(ret);
    free(buf);
    return retStr;
}

static std::string luabasename(const std::string &path)
{
    char *buf = strdup(path.c_str()),
         *ret = basename(buf);
    std::string retStr(ret);
    free(buf);
    return retStr;
}

static daemon_proc_spec_ptr daemon_pipe_add_proc(daemon_pipe_ptr const &pipe, luabind::object const &tbl)
{
    daemon_proc_spec_ptr proc(new daemon_proc_spec);
    bool cmdFound = false;

    for(luabind::iterator iter(tbl), end; iter != end; ++iter)
    {
        int keytype = luabind::type(iter.key());
        if(keytype != LUA_TSTRING)
            throw failure("bad key in daemon_pipe:add_proc (string expected, got %s)", lua_typename(tbl.interpreter(), keytype));
        const char *key = luabind::object_cast<const char *>(iter.key());
        if(strcmp(key, "forward_signals") == 0)
            proc->m_forwardSignals = luabind::object_cast<bool>(*iter);
        else if(strcmp(key, "stdin") == 0)
            proc->m_stdin = luabind::object_cast<file_spec_ptr>(*iter);
        else if(strcmp(key, "stdout") == 0)
            proc->m_stdout = luabind::object_cast<file_spec_ptr>(*iter);
        else if(strcmp(key, "stderr") == 0)
            proc->m_stderr = luabind::object_cast<file_spec_ptr>(*iter);
        else if(strcmp(key, "cmd") == 0)
        {
            copyCmdFromLua(proc->m_cmdArgv, *iter, "daemon_pipe:add_proc.cmd");
            cmdFound = true;
        }
        else
            throw failure("unknown key %s in daemon_pipe:add_proc", key);
    }

    if(!cmdFound)
        throw failure("daemon_pipe:add_proc: cmd is required");

    pipe->add_proc(proc);
    return proc;
}

static void try_error_write(const luabind::object &cmd_argv, const std::string &input)
{
    daemon_pipe args;
    daemon_proc_spec_ptr proc(new daemon_proc_spec);
    proc->m_forwardSignals = true;
    copyCmdFromLua(proc->m_cmdArgv, cmd_argv, "try_error_write argument 1");

    args.add_proc(proc);
    args.try_error_write(input);
}

static luabind::object daemon_proc_get_pid(lua_State *st, daemon_proc_spec_ptr const &proc)
{
    if(!proc->started()) return luabind::object();
    else                 return luabind::object(st, proc->getPID());
}

static luabind::object daemon_proc_exited(lua_State *st, daemon_proc_spec_ptr const &proc)
{
    if(!proc->finished()) return luabind::object();
    else                  return luabind::object(st, bool(WIFEXITED(proc->getStatus())));
}

static luabind::object daemon_proc_signaled(lua_State *st, daemon_proc_spec_ptr const &proc)
{
    if(!proc->finished()) return luabind::object();
    else                  return luabind::object(st, bool(WIFSIGNALED(proc->getStatus())));
}

static luabind::object daemon_proc_exitstatus(lua_State *st, daemon_proc_spec_ptr const &proc)
{
    if(!proc->finished() || !WIFEXITED(proc->getStatus()))
        return luabind::object();
    else
        return luabind::object(st, int(WEXITSTATUS(proc->getStatus())));
}

static luabind::object daemon_proc_termsig(lua_State *st, daemon_proc_spec_ptr const &proc)
{
    if(!proc->finished() || !WIFSIGNALED(proc->getStatus()))
        return luabind::object();
    else
        return luabind::object(st, int(WTERMSIG(proc->getStatus())));
}

void translate_failure(lua_State* L, failure const& e)
{
    // prevents lua errormessages from having "std::exception:" tacked on front
    lua_pushstring(L, e.what());
}

LUALIB_API int luaopen_with_exec_c(lua_State *L)
{
    using namespace luabind;
    const char *libname = "with_exec_c";
    open(L);
    register_exception_handler<failure>(&translate_failure);

    module(L, libname)
    [
        def("exec_with_namespace_internal", exec_with_namespace_internal),
        def("dirname", luadirname),
        def("basename", luabasename),
        def("try_error_write", try_error_write),
        class_<file_spec, file_spec_ptr>("file_spec"),
        class_<daemon_proc_spec, daemon_proc_spec_ptr>("daemon_proc_spec")
            .property("finished", &daemon_proc_spec::finished)
            .property("pid", &daemon_proc_get_pid)
            .property("WIFEXITED", &daemon_proc_exited)
            .property("WIFSIGNALED", &daemon_proc_signaled)
            .property("WEXITSTATUS", &daemon_proc_exitstatus)
            .property("WTERMSIG", &daemon_proc_termsig),
        class_<daemon_pipe, daemon_pipe_ptr>("daemon_pipe")
            .def(constructor<>())
            .def("pipe", &daemon_pipe::add_pipe)
            .def("file", (file_spec_ptr (daemon_pipe::*)(const std::string &))&daemon_pipe::add_file)
            .def("file", (file_spec_ptr (daemon_pipe::*)(const std::string &, bool))&daemon_pipe::add_file)
            .def_readwrite("lock_file", &daemon_pipe::m_lockFile)
            .property("devnull", &daemon_pipe::get_devnull)
            .property("caller_stdin", &daemon_pipe::get_caller_stdin)
            .property("caller_stdout", &daemon_pipe::get_caller_stdout)
            .property("caller_stderr", &daemon_pipe::get_caller_stderr)
            .def("add_proc", &daemon_pipe_add_proc)
            .def("run", &daemon_pipe::exec)
    ];

    luabind::object lib(luabind::globals(L)[libname]);
    lib["MOUNTPOINT"] = std::string(WITH_MOUNTPOINT);
    lib["RUNFILE"] = std::string(WITH_RUNFILE);
    lib["VERSION"] = VERSION;
    lib["ENOENT"] = ENOENT;
    lib["EEXIST"] = EEXIST;
    lib["SIGTERM"] = SIGTERM;

    return 0;
}
