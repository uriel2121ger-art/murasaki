#include "seccomp.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

// The new mount API and a few later calls: numbers are the same on x86_64 and aarch64 (the unified table), so a build against old headers still gets them in the filter instead of silently losing them.
#if defined(__x86_64__) || defined(__aarch64__)
#ifndef SYS_open_tree
#define SYS_open_tree 428
#endif
#ifndef SYS_move_mount
#define SYS_move_mount 429
#endif
#ifndef SYS_fsopen
#define SYS_fsopen 430
#endif
#ifndef SYS_fsconfig
#define SYS_fsconfig 431
#endif
#ifndef SYS_fsmount
#define SYS_fsmount 432
#endif
#ifndef SYS_fspick
#define SYS_fspick 433
#endif
#ifndef SYS_mount_setattr
#define SYS_mount_setattr 442
#endif
#ifndef SYS_io_uring_setup
#define SYS_io_uring_setup 425
#define SYS_io_uring_enter 426
#define SYS_io_uring_register 427
#endif
#ifndef SYS_pidfd_getfd
#define SYS_pidfd_getfd 438
#endif
#endif

namespace murasaki::sandbox {

#if defined(__x86_64__)
constexpr uint32_t kAuditArch = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
constexpr uint32_t kAuditArch = AUDIT_ARCH_AARCH64;
#else
constexpr uint32_t kAuditArch = 0; // (no filter on other architectures: apply reports it)
#endif

std::vector<long> puppetDeniedSyscalls()
{
    std::vector<long> list;
#define DENY(name) list.push_back(SYS_##name)
#ifdef SYS_io_uring_setup
    DENY(io_uring_setup);
    DENY(io_uring_enter);
    DENY(io_uring_register);
#endif
#ifdef SYS_bpf
    DENY(bpf);
#endif
#ifdef SYS_userfaultfd
    DENY(userfaultfd);
#endif
#ifdef SYS_perf_event_open
    DENY(perf_event_open);
#endif
#ifdef SYS_ptrace
    DENY(ptrace);
#endif
#ifdef SYS_process_vm_readv
    DENY(process_vm_readv);
    DENY(process_vm_writev);
#endif
#ifdef SYS_keyctl
    DENY(keyctl);
    DENY(add_key);
    DENY(request_key);
#endif
#ifdef SYS_open_by_handle_at
    DENY(open_by_handle_at);
#endif
#ifdef SYS_mount
    DENY(mount);
    DENY(umount2);
    DENY(pivot_root);
    DENY(chroot);
#endif
#if defined(__x86_64__) || defined(__aarch64__)
    DENY(mount_setattr);
    DENY(move_mount);
    DENY(open_tree);
    DENY(fsopen);
    DENY(fsconfig);
    DENY(fsmount);
    DENY(fspick);
#endif
#ifdef SYS_init_module
    DENY(init_module);
    DENY(finit_module);
    DENY(delete_module);
#endif
#ifdef SYS_pidfd_getfd
    DENY(pidfd_getfd);
#endif
#ifdef SYS_kcmp
    DENY(kcmp);
#endif
#ifdef SYS_swapon
    DENY(swapon);
    DENY(swapoff);
#endif
#ifdef SYS_reboot
    DENY(reboot);
#endif
#ifdef SYS_acct
    DENY(acct);
#endif
#ifdef SYS_unshare
    DENY(unshare);
#endif
#ifdef SYS_setns
    DENY(setns);
#endif
#ifdef SYS_kexec_load
    DENY(kexec_load);
#endif
#ifdef SYS_kexec_file_load
    DENY(kexec_file_load);
#endif
#undef DENY
    return list;
}

namespace {

// Every CLONE_NEW* flag (they all sit in the low 32 bits of clone's first argument).
constexpr uint32_t kCloneNamespaceFlags = 0x00020000u /*NEWNS*/ | 0x02000000u /*NEWCGROUP*/ | 0x04000000u /*NEWUTS*/ | 0x08000000u /*NEWIPC*/ | 0x10000000u /*NEWUSER*/ |
                                          0x20000000u /*NEWPID*/ | 0x40000000u /*NEWNET*/ | 0x00000080u /*NEWTIME*/;

} // namespace

namespace {

std::vector<sock_filter> buildFilter(const std::vector<long> &denied)
{
    std::vector<sock_filter> program;
    if (kAuditArch == 0)
        return program;
    const auto stmt = [&](uint16_t code, uint32_t k) { program.push_back(BPF_STMT(code, k)); };
    const auto jump = [&](uint16_t code, uint32_t k, uint8_t jt, uint8_t jf) { program.push_back(BPF_JUMP(code, k, jt, jf)); };
    // A process of another ABI (32 bit calls on x86_64) has other call numbers: refuse the lot.
    stmt(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch));
    jump(BPF_JMP | BPF_JEQ | BPF_K, kAuditArch, 1, 0);
    stmt(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM);
    stmt(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr));
#if defined(__x86_64__)
    // (the x32 ABI shares the architecture value; its numbers carry this bit)
    jump(BPF_JMP | BPF_JGE | BPF_K, 0x40000000u, 0, 1);
    stmt(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM);
#endif
    // clone3: ENOSYS (glibc falls back to clone). clone: EPERM when it asks for any new namespace; otherwise on to the list with the number loaded again.
    jump(BPF_JMP | BPF_JEQ | BPF_K, static_cast<uint32_t>(SYS_clone3), 0, 1);
    stmt(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS);
    jump(BPF_JMP | BPF_JEQ | BPF_K, static_cast<uint32_t>(SYS_clone), 0, 3);
    stmt(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0])); // (the low word on both little endian targets)
    jump(BPF_JMP | BPF_JSET | BPF_K, kCloneNamespaceFlags, 0, 1);
    stmt(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM);
    stmt(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr));
    for (size_t i = 0; i < denied.size(); ++i) {
        // match: jump over the rest of the list and the ALLOW, to the EPERM at the end; no match: the next test
        const size_t remaining = denied.size() - i - 1;
        jump(BPF_JMP | BPF_JEQ | BPF_K, static_cast<uint32_t>(denied[i]), static_cast<uint8_t>(remaining + 1), 0);
    }
    stmt(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    stmt(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM);
    return program;
}

bool applyFilter(std::vector<sock_filter> program, std::string &error)
{
    if (program.empty()) {
        error = "no seccomp filter for this architecture";
        return false;
    }
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        error = std::string("PR_SET_NO_NEW_PRIVS: ") + std::strerror(errno);
        return false;
    }
    if (program.size() > 4096) {
        error = "seccomp program too long";
        return false;
    }
    sock_fprog prog{static_cast<unsigned short>(program.size()), program.data()};
    // TSYNC: every thread, so a helper that grew one is covered too (it has none today). On failure it returns the id of the thread that could not be synchronised, not -1.
    const long result = ::syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, &prog);
    if (result > 0) {
        error = "seccomp: thread " + std::to_string(result) + " could not be synchronised";
        return false;
    }
    if (result != 0) {
        error = std::string("seccomp: ") + std::strerror(errno);
        return false;
    }
    return true;
}

} // namespace

std::vector<sock_filter> buildPuppetFilter()
{
    return buildFilter(puppetDeniedSyscalls());
}

bool applyPuppetSeccomp(std::string &error)
{
    return applyFilter(buildPuppetFilter(), error);
}

std::vector<long> screenmatchDeniedSyscalls()
{
    std::vector<long> list = puppetDeniedSyscalls();
#define DENY(name) list.push_back(SYS_##name)
    DENY(socket);
    DENY(connect);
    DENY(bind);
    DENY(listen);
    DENY(accept);
    DENY(accept4);
    DENY(execve);
    DENY(execveat);
#undef DENY
    return list;
}

std::vector<sock_filter> buildScreenmatchFilter()
{
    return buildFilter(screenmatchDeniedSyscalls());
}

bool applyScreenmatchSeccomp(std::string &error)
{
    return applyFilter(buildScreenmatchFilter(), error);
}

} // namespace murasaki::sandbox
