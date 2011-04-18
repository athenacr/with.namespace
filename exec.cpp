#include <sys/types.h>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "exec.hpp"
#include "exec_defs.hpp"

failure::failure(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m_err, sizeof(m_err)-1, fmt, ap);
    m_err[255] = '\0';
    va_end(ap);
}

void exec_args::do_execvp() const
{
    execvp(exec_name(), &m_args.front());
    throw failure("execvp %s failed: %m", exec_name());
}

void exec_args::do_execve(char * const environ[]) const
{
    execve(exec_name(), &m_args.front(), environ);
    throw failure("execve %s failed: %m", exec_name());
}

void exec_with_namespace(
    const std::string &devname,
    // the target=src key-value pairs defining the namespace
    const std::vector<std::string> &namespace_argv,
    // the command we want to run inside the namespace
    const std::vector<std::string> &cmd_argv)
{
    // push args and execve exec_with_namespace
    // usage: exec_with_namespace cmd args... -- mount-name target1=src1 target2=src
    exec_args ns_argv;
    ns_argv.push_back(WITH_NAMESPACE_DIR "/exec_with_namespace");
    for (std::vector<std::string>::const_iterator i = cmd_argv.begin(), end = cmd_argv.end();
        i != end; ++i)
        ns_argv.push_back(i->c_str());

    ns_argv.push_back("--");
    ns_argv.push_back(devname);

    for (std::vector<std::string>::const_iterator i = namespace_argv.begin(), end = namespace_argv.end();
        i != end; ++i)
        ns_argv.push_back(i->c_str());

    ns_argv.push_back("--");

    char **env = environ;
    for (; *env; ++env)
        ns_argv.push_back(*env);

    // exec_with_namespace must be setuid. This means it receives
    // a sanitized copy of the environment thanks to glibc/ld.so.
    // However, we don't want to modify the environment; as a workaround,
    // pass the environment on the commandline. We can also empty out
    // with_namespace_suid's environ since it doesn't need it;

    char *exec_environ[] = { NULL };
    ns_argv.do_execve(exec_environ);
}
