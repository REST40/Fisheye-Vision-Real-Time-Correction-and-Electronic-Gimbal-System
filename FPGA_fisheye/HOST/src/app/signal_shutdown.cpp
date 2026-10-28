#include "signal_shutdown.h"
#ifdef Q_OS_UNIX
#include <QSocketNotifier>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif
namespace df {
#ifdef Q_OS_UNIX
static volatile sig_atomic_t writeFd = -1;
static void shutdownSignal(int signal) {
    int saved = errno;
    char value = char(signal);
    if (writeFd >= 0) {
        auto result = ::write(int(writeFd), &value, 1);
        (void)result;
    }
    errno = saved;
}
#endif
SignalShutdown::SignalShutdown(QObject *parent, std::function<void()> close) : QObject(parent) {
#ifdef Q_OS_UNIX
    if (::pipe2(descriptors, O_NONBLOCK | O_CLOEXEC) != 0)
        return;
    writeFd = descriptors[1];
    struct sigaction action {};
    action.sa_handler = shutdownSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    sigaction(SIGINT, &action, &previousInt);
    sigaction(SIGTERM, &action, &previousTerm);
    auto *notifier = new QSocketNotifier(descriptors[0], QSocketNotifier::Read, this);
    connect(notifier, &QSocketNotifier::activated, this, [this, close] {
        char bytes[32];
        while (::read(descriptors[0], bytes, sizeof bytes) > 0) {
        }
        close();
    });
#else
    (void)close;
#endif
}
SignalShutdown::~SignalShutdown() {
#ifdef Q_OS_UNIX
    if (descriptors[0] < 0)
        return;
    writeFd = -1;
    sigaction(SIGINT, &previousInt, nullptr);
    sigaction(SIGTERM, &previousTerm, nullptr);
    for (int fd : descriptors)
        if (fd >= 0)
            ::close(fd);
#endif
}
} // namespace df
