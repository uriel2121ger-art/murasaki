#include "landlock.h"

#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>

// The kernel interface, spelled out here so the helper builds against any kernel headers (the ABI numbers are what
// decide at run time what is used).
namespace {

constexpr uint64_t kFsExecute = 1ULL << 0;
constexpr uint64_t kFsWriteFile = 1ULL << 1;
constexpr uint64_t kFsReadFile = 1ULL << 2;
constexpr uint64_t kFsReadDir = 1ULL << 3;
constexpr uint64_t kFsRemoveDir = 1ULL << 4;
constexpr uint64_t kFsRemoveFile = 1ULL << 5;
constexpr uint64_t kFsMakeChar = 1ULL << 6;
constexpr uint64_t kFsMakeDir = 1ULL << 7;
constexpr uint64_t kFsMakeReg = 1ULL << 8;
constexpr uint64_t kFsMakeSock = 1ULL << 9;
constexpr uint64_t kFsMakeFifo = 1ULL << 10;
constexpr uint64_t kFsMakeBlock = 1ULL << 11;
constexpr uint64_t kFsMakeSym = 1ULL << 12;
constexpr uint64_t kFsRefer = 1ULL << 13;     // ABI 2
constexpr uint64_t kFsTruncate = 1ULL << 14;  // ABI 3
constexpr uint64_t kFsIoctlDev = 1ULL << 15;  // ABI 5

constexpr uint64_t kNetBindTcp = 1ULL << 0;    // ABI 4
constexpr uint64_t kNetConnectTcp = 1ULL << 1; // ABI 4

constexpr uint64_t kScopeAbstractUnixSocket = 1ULL << 0; // ABI 6
constexpr uint64_t kScopeSignal = 1ULL << 1;             // ABI 6

constexpr uint32_t kCreateRulesetVersion = 1U << 0;
constexpr int kRulePathBeneath = 1;

struct RulesetAttr {
    uint64_t handledAccessFs;
    uint64_t handledAccessNet;
    uint64_t scoped;
};

struct PathBeneathAttr {
    uint64_t allowedAccess;
    int32_t parentFd;
} __attribute__((packed));

long createRuleset(const RulesetAttr *attr, size_t size, uint32_t flags)
{
    return ::syscall(SYS_landlock_create_ruleset, attr, size, flags);
}

long addRule(int rulesetFd, int type, const void *attr)
{
    return ::syscall(SYS_landlock_add_rule, rulesetFd, type, attr, 0U);
}

long restrictSelf(int rulesetFd)
{
    return ::syscall(SYS_landlock_restrict_self, rulesetFd, 0U);
}

// Every filesystem right the given ABI knows.
uint64_t fsRightsForAbi(int abi)
{
    uint64_t rights = kFsExecute | kFsWriteFile | kFsReadFile | kFsReadDir | kFsRemoveDir | kFsRemoveFile | kFsMakeChar
        | kFsMakeDir | kFsMakeReg | kFsMakeSock | kFsMakeFifo | kFsMakeBlock | kFsMakeSym;
    if (abi >= 2)
        rights |= kFsRefer;
    if (abi >= 3)
        rights |= kFsTruncate;
    if (abi >= 5)
        rights |= kFsIoctlDev;
    return rights;
}

// What a rule on a plain file may carry; the kernel refuses directory-only rights there.
constexpr uint64_t kFileRights = kFsExecute | kFsWriteFile | kFsReadFile | kFsTruncate | kFsIoctlDev;

std::string errnoText(const char *what)
{
    return std::string(what) + ": " + std::strerror(errno);
}

} // namespace

namespace murasaki::sandbox {

int kernelAbi()
{
    const long abi = createRuleset(nullptr, 0, kCreateRulesetVersion);
    return abi < 1 ? 0 : static_cast<int>(abi);
}

Result apply(const Policy &policy)
{
    Result result;
    // Without this, restrict_self is refused for an unprivileged process, and it also stops the engine from
    // gaining rights through a setuid helper. Done even when Landlock is missing.
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        result.error = errnoText("PR_SET_NO_NEW_PRIVS");
        return result;
    }
    int abi = kernelAbi();
    if (policy.maxAbi >= 0)
        abi = std::min(abi, policy.maxAbi);
    if (abi < 1) {
        result.error = "Landlock is not available on this kernel";
        return result;
    }
    result.abi = abi;

    const uint64_t handledFs = fsRightsForAbi(abi);
    RulesetAttr attr{handledFs, 0, 0};
    if (policy.denyTcp && abi >= 4) {
        attr.handledAccessNet = kNetBindTcp | kNetConnectTcp; // no rule is added: all TCP bind and connect is refused
        result.netDenied = true;
    }
    if (abi >= 6) {
        attr.scoped = kScopeAbstractUnixSocket | kScopeSignal;
        result.scoped = true;
    }
    const int ruleset = static_cast<int>(createRuleset(&attr, sizeof(attr), 0));
    if (ruleset < 0) {
        result.error = errnoText("landlock_create_ruleset");
        result.netDenied = result.scoped = false;
        return result;
    }

    const uint64_t readRights = (kFsExecute | kFsReadFile | kFsReadDir) & handledFs;
    const uint64_t writeRights = handledFs & ~kFsExecute;
    const auto add = [&](const std::string &path, uint64_t rights) {
        const int fd = ::open(path.c_str(), O_PATH | O_CLOEXEC);
        if (fd < 0) {
            if (errno == ENOENT || errno == ENOTDIR) {
                result.skipped.push_back(path);
                return true;
            }
            result.error = errnoText(("cannot open " + path).c_str());
            return false;
        }
        struct stat st {};
        if (::fstat(fd, &st) != 0) {
            result.error = errnoText(("cannot stat " + path).c_str());
            ::close(fd);
            return false;
        }
        if (!S_ISDIR(st.st_mode))
            rights &= kFileRights;
        bool ok = true;
        if (rights != 0) {
            const PathBeneathAttr rule{rights, fd};
            if (addRule(ruleset, kRulePathBeneath, &rule) != 0) {
                result.error = errnoText(("cannot allow " + path).c_str());
                ok = false;
            }
        }
        ::close(fd);
        return ok;
    };
    for (const std::string &path : policy.readOnly) {
        if (!add(path, readRights)) {
            ::close(ruleset);
            result.netDenied = result.scoped = false;
            return result;
        }
    }
    for (const std::string &path : policy.readWrite) {
        if (!add(path, writeRights | readRights)) {
            ::close(ruleset);
            result.netDenied = result.scoped = false;
            return result;
        }
    }

    if (restrictSelf(ruleset) != 0) {
        result.error = errnoText("landlock_restrict_self");
        result.netDenied = result.scoped = false;
        ::close(ruleset);
        return result;
    }
    ::close(ruleset);
    result.applied = true;
    return result;
}

} // namespace murasaki::sandbox
