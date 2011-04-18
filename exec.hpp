#ifndef WITH_EXEC_H
#define WITH_EXEC_H

#include <algorithm>
#include <vector>
#include <string>

#include <boost/noncopyable.hpp>

class failure : public std::exception
{
public:
    failure() { m_err[0] = '\0'; }
    failure(const char *fmt, ...);
    const char* what() const throw() { return m_err; }
    char m_err[256];
};

/// A holder for an argv[] array for passing to execvp and friends.
struct exec_args : public boost::noncopyable
{
    exec_args() { m_args.push_back(NULL); }
    ~exec_args() { std::for_each(m_args.begin(), m_args.end(), free); }
    void push_back(const char *arg) { m_args.back() = strdup(arg); m_args.push_back(NULL); }
    void push_back(const std::string &arg) { push_back(arg.c_str()); }

    bool empty() const { return m_args.size() <= 1; }
    const char *exec_name() const { return m_args.front(); }
    void do_execvp() const; // calls execvp() or throws failure
    void do_execve(char * const environ[]) const;

    std::vector<char *> m_args;
};

void exec_with_namespace(
    const std::string &devname,
    // the target=src key-value pairs defining the namespace
    const std::vector<std::string> &target_src_argv,
    // the command we want to run inside the namespace
    const std::vector<std::string> &cmd_argv);

#endif // WITH_EXEC_H
