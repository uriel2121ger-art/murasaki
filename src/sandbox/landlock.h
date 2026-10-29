#pragma once

#include <string>
#include <vector>

// A small, dependency free wrapper around Linux Landlock for murasaki-sandbox: plain C++20 and POSIX, no Qt, so the
// helper stays tiny and can be audited at a glance.
namespace murasaki::sandbox {

struct Policy {
    std::vector<std::string> readOnly;  // files or directories that can be read and executed
    std::vector<std::string> readWrite; // files or directories that can also be written, created and removed
    bool denyTcp = false;               // refuse TCP bind and connect (ABI >= 4)
    int maxAbi = -1;                    // use at most this ABI (0: behave as if Landlock were missing); -1: no limit
};

struct Result {
    int abi = 0;           // the ABI that was used; 0 when Landlock is not available
    bool applied = false;  // the ruleset is in force
    bool netDenied = false;
    bool scoped = false;   // abstract unix sockets and signals to outsiders are refused (ABI >= 6)
    std::string error;     // why it could not be applied (empty when applied)
    std::vector<std::string> skipped; // rule paths that do not exist (not an error)
};

// The kernel's Landlock ABI version, or 0 when it is missing or disabled.
int kernelAbi();

// Sets PR_SET_NO_NEW_PRIVS and restricts the calling thread (and so everything it execs) to the policy. Rights the
// kernel does not know are left out, which is how an older kernel degrades: filesystem rules always apply on ABI >= 1,
// the rest as far as the ABI reaches. On failure nothing is half-applied: the result says why and the process is
// unrestricted (except for no_new_privs).
Result apply(const Policy &policy);

} // namespace murasaki::sandbox
