#pragma once

// Bytes that must not outlive their use nor end up in a core dump (the picture of a pending screen capture, docs/security.md): they live in their own page-aligned mapping marked
// MADV_DONTDUMP, which is overwritten with zeroes before it is given back. Move-only; the mapping is the buffer's alone, so nothing else shares a page with it.
//
// This protects the copy that is KEPT (up to a minute); the short-lived copies that Qt makes while the bytes are produced and sent (a few milliseconds) are on the ordinary heap and
// are wiped where the code owns them (wipe()), but not elsewhere.

#include <QByteArray>
#include <QByteArrayView>
#include <QtGlobal>

#include <functional>

namespace murasaki {

class SecureBytes
{
public:
    SecureBytes() = default;
    // Copies `data` into a mapping of its own. Empty data, or a failed mapping, gives an empty buffer (isEmpty()).
    explicit SecureBytes(QByteArrayView data);
    ~SecureBytes() { clear(); }
    SecureBytes(const SecureBytes &) = delete;
    SecureBytes &operator=(const SecureBytes &) = delete;
    SecureBytes(SecureBytes &&other) noexcept : m_data(other.m_data), m_size(other.m_size), m_mapped(other.m_mapped) { other.m_data = nullptr; other.m_size = other.m_mapped = 0; }
    SecureBytes &operator=(SecureBytes &&other) noexcept;

    bool isEmpty() const { return m_size == 0; }
    qsizetype size() const { return m_size; }
    QByteArrayView view() const { return QByteArrayView(m_data, m_size); }
    // A copy on the ordinary heap, for a caller that needs a QByteArray (wipe() it when done).
    QByteArray toByteArray() const { return QByteArray(m_data, m_size); }

    // Overwrites with zeroes and unmaps.
    void clear();

    // Overwrites the contents of `bytes` with zeroes, in a way the compiler may not drop, and empties it.
    static void wipe(QByteArray &bytes);

    // For the tests: how many buffers (and bytes of them) exist, and a callback that sees a buffer's whole mapping after it was overwritten and before it is unmapped.
    static int liveBuffers();
    static qint64 liveBytes();
    static void setWipeObserver(std::function<void(const char *data, qsizetype size, bool allZero)> observer);

private:
    char *m_data = nullptr;
    qsizetype m_size = 0;
    qsizetype m_mapped = 0;
};

} // namespace murasaki
