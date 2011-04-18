#include <sys/mount.h>
#include <sys/stat.h>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <list>
#include <string>
#include <vector>

#include "exec_defs.hpp"

#define CHECK(cond, args...) \
    do { \
        if(!(cond)) { \
            fprintf(stderr, args); \
            return 1; \
        } \
    } while(0)

#ifndef MNT_DETACH // glibc 2.7 isn't new enough
#define MNT_DETACH      0x00000002
#endif

int usage(const char *progname)
{
    fprintf(stderr, "usage: %s cmd args... -- mount-name target1=src1 target2=src ... -- env\n"
        "    This is a setuid utility helper for with_exec.lua and /usr/bin/with\n"
        "    For each target=src, makes a symlink mount-name/target1 => src.\n",
        progname);
    return 1;
}

int mkdir_p(const std::string& dir, mode_t mode)
{
    int ret = mkdir(dir.c_str(), mode);
    if (ret < 0)
    {
        if (errno == ENOENT)
        {
            // If we couldn't make the whole directory because of ENOENT (a
            // parent directory doesn't exist), then try recursively to make
            // the immediate parent directory.

            size_t slash = dir.rfind('/');
            if (slash != std::string::npos)
            {
                std::string prefix = dir.substr(0, slash);
                mkdir_p(prefix, mode);
                return mkdir(dir.c_str(), mode);
            }
        }
    }
    return ret;
}

// using the namespace vector, create all the symlinks under WITH_MOUNTPOINT
// also writes out .ns metadata file
int create_symlinks_and_metadata(const char* progname, const std::list<char*>& ns_args)
{
    // create all the symlinks under WITH_MOUNTPOINT
    for (std::list<char*>::const_iterator it = ++ns_args.begin(), end = ns_args.end(); it != end; ++it)
    {
        // split out the target=source
        std::string target_source = *it;
        size_t equal_index = target_source.find('=');
        CHECK(equal_index != std::string::npos && target_source[equal_index + 1],
            "%s argument %s is must be of the form target=src\n", progname, target_source.c_str());
        const std::string target = target_source.substr(0, equal_index);
        const std::string source = target_source.substr(equal_index + 1, std::string::npos);

        // create dir for mount_path, if necessary
        std::string mount_path = WITH_MOUNTPOINT "/" + target;
        size_t path_end_index = mount_path.rfind('/');
        if (path_end_index != std::string::npos)
        {
            std::string dir_path = mount_path.substr(0, path_end_index);
            int ret = mkdir_p(dir_path, S_IRWXU | S_IRGRP | S_IXGRP | S_IRUSR | S_IXUSR | S_IROTH | S_IXOTH);
            CHECK(ret >= 0 || errno == EEXIST, "%s: create %s failed: %m\n", progname, dir_path.c_str());
        }

        // symlink
        int ret = symlink(source.c_str(), mount_path.c_str());
        CHECK(ret >= 0, "%s: symlink %s -> %s failed: %m\n", progname, mount_path.c_str(), source.c_str());
    };

    // write the namespace metadata
    FILE* fd = fopen(WITH_MOUNTPOINT "/.ns", "w");
    CHECK(fd >= 0, "%s: unable to write namespace metadata: %m\n%s\n", progname, WITH_MOUNTPOINT "/.ns");
    for (std::list<char*>::const_iterator it = ns_args.begin(), end = ns_args.end(); it != end; ++it)
        fprintf(fd, "%s ", *it);
    fclose(fd);

    return 0;
}

int main(int argc, char** argv)
{
    const char* progname = basename(strdup(argv[0]));
    if (argc <= 1)
        return usage(progname);

    // special case -- if the first arg is "--init.d", then we just build symlinks
    if (strcmp(argv[1], "--init.d") == 0)
    {
        std::list<char*> ns_args;
        for (int i = 1; i < argc; ++i)
            ns_args.push_back(argv[i]);
        int ret = create_symlinks_and_metadata(progname, ns_args);
        return ret;
    }

    // Search **backwards** from the end of the commandline for --
    //     from end to 1st -- is the environment args (env_args)
    //     up to 2nd -- is the with namespace args (ns_args)
    //     rest is the command args (exec_args)
    // This also means you need to push to **front**, not **back**.
    std::list<char*> env_args, ns_args, exec_args;
    int i = argc - 1; // start at 1 since 0 is progname
    while (i > 0 && strcmp(argv[i], "--") != 0)
        env_args.push_front(argv[i--]);
    i--; // skip --
    while (i > 0 && strcmp(argv[i], "--") != 0)
        ns_args.push_front(argv[i--]);
    if (ns_args.size() == 0) // must at least have mount name
        return usage(progname);
    i--; // skip --
    while (i > 0)
        exec_args.push_front(argv[i--]);
    exec_args.push_back(NULL); // execvp requires final argument be NULL

    // detach from our parent's namespace
    CHECK(unshare(CLONE_NEWNS) == 0, "%s: unshare failed: %m\n", progname);

    // umount the old /with (this mount is now private for us)
    // the MNT_DETACH is needed if some joker set getcwd() to /with.
    int ret = umount2(WITH_MOUNTPOINT, MNT_DETACH);
    CHECK(ret >= 0, "%s: umount2 tmpfs " WITH_MOUNTPOINT " failed: %m\n", progname);

    // after the -- is mount_name target1=src1 target2=src2 -- env
    char* mount_name = ns_args.front();
    assert(mount_name);
    ret = mount(mount_name, "/with", "tmpfs", 0, NULL);
    CHECK(ret >= 0, "%s: mount tmpfs " WITH_MOUNTPOINT " failed: %m\n", progname);

    // build out the symlinks from the namespace
    ret = create_symlinks_and_metadata(progname, ns_args);
    if (ret != 0)  // CHECKs are performed in the function
        return ret;

    // write env metadata
    FILE* fd = fopen(WITH_MOUNTPOINT "/.env", "w");
    CHECK(fd >= 0, "%s: unable to write env metadata: %m\n%s\n", progname, WITH_MOUNTPOINT "/.env");
    for (std::list<char*>::const_iterator it = env_args.begin(), end = env_args.end(); it != end; ++it)
        fprintf(fd, "%s\n", *it);
    fclose(fd);

    // drop setuid
    int uid = getuid(), gid = getgid();
    CHECK(setresuid(uid, uid, uid) >= 0 && setresgid(gid, gid, gid) >= 0,
        "%s: setresuid/setresgid failed: %m\n", progname);

    // now that we've dropped privileges, install the environment
    // that was passed on the commandline.
    clearenv();
    for (std::list<char*>::const_iterator it = env_args.begin(), end = env_args.end(); it != end; ++it)
    {
        char* env_var = *it;
        assert(env_var);
        putenv(env_var);
    }

    // we need to copy exec_args to a vector so it's laid out like an array
    std::vector<char*> exec_args_as_array(exec_args.begin(), exec_args.end());
    CHECK(execvp(exec_args_as_array[0], &exec_args_as_array[0]) != -1, "%s: cannot exec %s: %m\n", progname, exec_args_as_array[0]);
    return 1;
}
