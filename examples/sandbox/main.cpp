// sandbox: runs a command in new Linux namespaces, as an ordinary user.
// In a user namespace of its own the command is root (uid and gid 0 map to
// yours), in a PID namespace of its own it is process 1, and its hostname
// (UTS), System V IPC and mounts are its own too. With -net=false it gets
// a network namespace with nothing in it but a loopback interface.
//
//     $ sandbox -- sh -c 'id -u; echo pid $$; hostname box; hostname'
//     0
//     pid 1
//     box
//     $ hostname                                  # unchanged
//     $ sandbox -net=false -- ping -c1 1.1.1.1
//     ping: connect: Network is unreachable
//
// exec::Cmd's SysProcAttr asks for the namespaces (clone(2) flags) and the
// ID mappings, which the parent writes before the child goes on. Being
// root in the namespace grants nothing outside it. This needs unprivileged
// user namespaces (sysctl kernel.unprivileged_userns_clone, and on some
// distributions an AppArmor profile).
#include <gopp/errors.hpp>
#include <gopp/flag.hpp>
#include <gopp/fmt.hpp>
#include <gopp/os.hpp>
#include <gopp/os/exec.hpp>
#include <gopp/syscall.hpp>

using namespace gopp;
namespace exec = os::exec;

int main(int argc, char** argv) {
    bool net = true;
    string root;
    flag::BoolVar(&net, "net", net, "share the network; with -net=false the command has none");
    flag::StringVar(&root, "chroot", "", "a directory to make the command's root (a root file system)");
    flag::Parse(argc, argv);
    const auto& args = flag::Args();
    if (args.empty()) {
        fmt::Println(os::Stderr, "usage: sandbox [-net=false] [-chroot dir] [--] command [args...]");
        return 2;
    }

    auto cmd = exec::Command(args[0], std::span<const string>(args).subspan(1));
    cmd->Stdin = ptr<io::Reader>(&os::Stdin);
    cmd->Stdout = ptr<io::Writer>(&os::Stdout);
    cmd->Stderr = ptr<io::Writer>(&os::Stderr);

    syscall::SysProcAttr attr;
    attr.Cloneflags = syscall::CLONE_NEWUSER_ | syscall::CLONE_NEWPID_ | syscall::CLONE_NEWNS_ |
                      syscall::CLONE_NEWUTS_ | syscall::CLONE_NEWIPC_;
    if (!net) attr.Cloneflags |= syscall::CLONE_NEWNET_;
    // Root in the namespace is this user outside it. An unprivileged
    // process may map only its own IDs, and only with setgroups(2) denied.
    attr.UidMappings = {{.ContainerID = 0, .HostID = os::Getuid(), .Size = 1}};
    attr.GidMappings = {{.ContainerID = 0, .HostID = os::Getgid(), .Size = 1}};
    attr.Chroot = root;
    // Don't outlive the sandbox.
    attr.Pdeathsig = syscall::SIGKILL_;
    cmd->SysProcAttr = attr;

    if (error err = cmd->Run()) {
        // The command ran and failed: pass its status on.
        if (auto* exit = errors::As<exec::ExitError>(err)) return exit->ExitCode() >= 0 ? exit->ExitCode() : 1;
        fmt::Println(os::Stderr, "sandbox: {}", err);
        if (errors::Is(err, fs::ErrPermission)) {
            fmt::Println(os::Stderr, "sandbox: are unprivileged user namespaces allowed? (kernel.unprivileged_userns_clone)");
        }
        return 1;
    }
    return 0;
}
