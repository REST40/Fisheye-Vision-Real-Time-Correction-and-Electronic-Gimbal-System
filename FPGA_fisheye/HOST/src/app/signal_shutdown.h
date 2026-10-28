#pragma once
#include <QObject>
#include <functional>
#ifdef Q_OS_UNIX
#include <signal.h>
#endif
namespace df {
// Convert asynchronous Unix shutdown signals to a normal Qt close event.
class SignalShutdown : public QObject {
  public:
    SignalShutdown(QObject *parent, std::function<void()> close);
    ~SignalShutdown() override;

  private:
#ifdef Q_OS_UNIX
    int descriptors[2]{-1, -1};
    struct sigaction previousInt {
    }, previousTerm{};
#endif
};
} // namespace df
