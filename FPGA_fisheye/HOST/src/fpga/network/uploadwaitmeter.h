#pragma once
#include <QtGlobal>
#include <array>

// Sample the previous state over monotonic elapsed time. Suspended tests and
// paused streams never contribute to normal-video waiting statistics.
class UploadWaitMeter {
  public:
    enum State { Suspended, Source, Credit, Sending, Ready, Count };
    struct Window {
        std::array<qint64, Count> ms{};
        qint64 longestCreditMs = 0;
        qint64 activeMs() const {
            return ms[Source] + ms[Credit] + ms[Sending] + ms[Ready];
        }
    };
    void reset() {
        *this = UploadWaitMeter();
    }
    bool sample(qint64 now, State state, Window &result) {
        if (m_last < 0) {
            m_last = m_windowStart = now;
            m_state = state;
            return false;
        }
        const qint64 dt = qMax<qint64>(0, now - m_last);
        m_window.ms[m_state] += dt;
        if (m_state == Credit) {
            m_creditRun += dt;
            m_window.longestCreditMs = qMax(m_window.longestCreditMs, m_creditRun);
        }
        if (state != Credit)
            m_creditRun = 0;
        m_state = state;
        m_last = now;
        if (now - m_windowStart < 1000)
            return false;
        result = m_window;
        m_window = {};
        m_windowStart = now;
        return true;
    }

  private:
    qint64 m_last = -1, m_windowStart = 0, m_creditRun = 0;
    State m_state = Suspended;
    Window m_window;
};
