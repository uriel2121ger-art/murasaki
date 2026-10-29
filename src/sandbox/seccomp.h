#pragma once

#include <linux/filter.h>

#include <string>
#include <vector>

// A hand-written seccomp-BPF denylist for murasaki-sandbox (no libseccomp: the helper stays dependency free). It refuses the
// kernel interfaces a GPU program of hostile data never needs and that are the usual way out of a sandbox: they fail with EPERM
// and the program goes on. Everything else is allowed (Mesa and the Vulkan drivers use too many calls for an allowlist).
namespace murasaki::sandbox {

// The syscall numbers the puppet filter refuses on this architecture (io_uring_*, bpf, userfaultfd, perf_event_open, ptrace,
// process_vm_*, keyctl and its siblings, open_by_handle_at, mount and friends, unshare, setns, kexec_*, module loading, pidfd_getfd, kcmp, swap, reboot, acct). The filter also answers `clone` with
// any CLONE_NEW* flag with EPERM (a nested user namespace would give back every capability inside it, which would make refusing unshare and setns pointless) and `clone3` with ENOSYS (its flags live in a
// struct a filter cannot read; glibc falls back to `clone` on ENOSYS, whereas on EPERM a thread could not be created). `clone3` is therefore not in this list.
std::vector<long> puppetDeniedSyscalls();

// The filter as a program (no kernel involved, so a test can run it over any seccomp_data). Empty on an architecture it has no numbers for.
std::vector<sock_filter> buildPuppetFilter();

// The filter of the screen matching helper (murasaki-screenmatch): the puppet list, and in addition socket, connect, bind, listen, accept and accept4 (the helper has its two inherited descriptors
// and never needs another), execve and execveat (it starts no program). Unlike the puppet filter it is applied by the helper itself, as its first act, because it refuses the exec that started it.
// socketpair, recvmsg and sendmsg stay: PipeWire passes buffer descriptors over its socket. Refused calls fail with EPERM.
std::vector<long> screenmatchDeniedSyscalls();
std::vector<sock_filter> buildScreenmatchFilter();
bool applyScreenmatchSeccomp(std::string &error);

// Installs the filter on the calling thread (and what it creates or execs). Needs PR_SET_NO_NEW_PRIVS, which Landlock's apply()
// has already set; this sets it too. Returns false with a reason when the kernel refuses the filter.
bool applyPuppetSeccomp(std::string &error);

} // namespace murasaki::sandbox
