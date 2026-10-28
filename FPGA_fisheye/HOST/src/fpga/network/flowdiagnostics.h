#pragma once

#include <QByteArray>
#include <QStringList>
#include <QtEndian>

// Additive FLOW heartbeat. Cumulative PS counters are differenced on the PS clock;
// stage windows have separate denominators and must never be added as a latency.
class FlowDiagnostics {
  public:
    void reset() {
        m_previous.clear();
        m_replaced = m_timeouts = m_failures = 0;
    }
    QString sample(const QByteArray &body, quint64 replaced, quint64 timeouts, quint64 failures) {
        if (body.size() != 256 && body.size() != 280)
            return {};
        const bool version2 = body.size() == 280 && u32(body, 4) == 2;
        if (u32(body, 0) != 0x464c4f57u ||
            (!version2 && !(body.size() == 256 && u32(body, 4) == 1)))
            return {};
        if (u32(body, 8) > 1)
            return {};
        if (!m_previous.isEmpty() &&
            (u32(body, 120) < u32(m_previous, 120) ||
             (u32(body, 120) == u32(m_previous, 120) && u64(body, 16) <= u64(m_previous, 16))))
            return {}; // UDP duplicates/reordering must not move the baseline backwards.
        const bool active = u32(body, 8) == 1;
        bool valid = active && m_previous.size() == body.size() && u32(m_previous, 8) == 1 &&
                     u32(body, 120) == u32(m_previous, 120) &&
                     u64(body, 16) > u64(m_previous, 16) && replaced >= m_replaced &&
                     timeouts >= m_timeouts && failures >= m_failures;
        for (int o : {24, 28, 48, 52, 80, 84, 88, 92, 96, 100})
            valid = valid && (m_previous.isEmpty() || u32(body, o) >= u32(m_previous, o));
        for (int o : {32, 40, 56, 64, 72})
            valid = valid && (m_previous.isEmpty() || u64(body, o) >= u64(m_previous, o));
        for (int stage = 0; stage < 3; ++stage) {
            const int o = 128 + 40 * stage;
            for (int p : {0, 4, 32})
                valid =
                    valid && (m_previous.isEmpty() || u32(body, o + p) >= u32(m_previous, o + p));
            for (int p : {8, 16, 24})
                valid =
                    valid && (m_previous.isEmpty() || u64(body, o + p) >= u64(m_previous, o + p));
        }
        if (version2 && m_previous.size() == body.size()) {
            valid = valid && u32(body, 256) >= u32(m_previous, 256) &&
                    u64(body, 264) >= u64(m_previous, 264) &&
                    u64(body, 272) >= u64(m_previous, 272);
        }
        QString result =
            active ? QStringLiteral("完整流水诊断：建立正常视频统计基线…")
                   : QStringLiteral("完整流水诊断：暂停或隔离测试中，恢复后重新建立基线。");
        if (valid) {
            auto d32 = [&](int o) { return u32(body, o) - u32(m_previous, o); };
            auto d64 = [&](int o) { return u64(body, o) - u64(m_previous, o); };
            auto average = [&](int sum, int count) {
                return d32(count) ? QString::number(double(d64(sum)) / d32(count) / 1000., 'f', 2)
                                  : QStringLiteral("—");
            };
            QStringList lines;
            if (version2)
                lines << QStringLiteral("输入校验：整包CRC %1 ms/次、双JPEG格式检查 %2 "
                                        "ms/次（%3次）；正常视频窗口，CRC失败的尝试也计入样本。")
                             .arg(average(264, 256), average(272, 256))
                             .arg(d32(256));
            lines << QStringLiteral("输出拆分（PS %1秒窗口）：CRC %2 ms/帧、后处理准备 %3 "
                                    "ms/帧（%4帧）；准备完成→首包 %5 ms、首包→末包交付 %6 "
                                    "ms（%7帧）；相邻输出就绪间隔 %8 ms（%9段）")
                         .arg(double(d64(16)) / 1000000., 0, 'f', 2)
                         .arg(average(32, 24), average(40, 24))
                         .arg(d32(24))
                         .arg(average(56, 48), average(64, 48))
                         .arg(d32(48))
                         .arg(average(72, 52))
                         .arg(d32(52));
            const QStringList names{QStringLiteral("双路解码"), QStringLiteral("全景校正"),
                                    QStringLiteral("JPEG编码")};
            QStringList stages;
            for (int i = 0; i < 3; ++i) {
                const int o = 128 + 40 * i;
                stages << QStringLiteral(
                              "%1：忙 %2 ms（%3帧）/空闲 %4 ms（%5段）/完成间隔 %6 ms（%7段）")
                              .arg(names[i], average(o + 8, o))
                              .arg(d32(o))
                              .arg(average(o + 16, o + 4))
                              .arg(d32(o + 4))
                              .arg(average(o + 24, o + 32))
                              .arg(d32(o + 32));
            }
            lines << stages.join(QStringLiteral("；"));
            lines << QStringLiteral(
                         "帧槽快照：空闲%1、接收%2、待处理%3、处理中%4、待发/"
                         "发送中%5；本窗可用额度通告%6次。替换/"
                         "丢弃：Host待上传%7、PS待处理%8、PS未发送输出%9、发送失败%10、接收不完整%"
                         "11、CRC错%12、无槽%13；Host上传超时%14/失败%15。")
                         .arg(u32(body, 12))
                         .arg(u32(body, 104))
                         .arg(u32(body, 108))
                         .arg(u32(body, 112))
                         .arg(u32(body, 116))
                         .arg(d32(28))
                         .arg(replaced - m_replaced)
                         .arg(d32(80))
                         .arg(d32(84))
                         .arg(d32(88))
                         .arg(d32(92))
                         .arg(d32(96))
                         .arg(d32(100))
                         .arg(timeouts - m_timeouts)
                         .arg(failures - m_failures);
            lines << QStringLiteral("忙/"
                                    "空闲为软件观察值，空闲含等待输入和调度；各阶段可重叠，不能相加"
                                    "。末包交付指PS交给网络栈，非Host确认收到。");
            result = lines.join('\n');
        }
        m_previous = body;
        m_replaced = replaced;
        m_timeouts = timeouts;
        m_failures = failures;
        return result;
    }

  private:
    static quint32 u32(const QByteArray &b, int o) {
        return qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(b.constData()) + o);
    }
    static quint64 u64(const QByteArray &b, int o) {
        return qFromBigEndian<quint64>(reinterpret_cast<const uchar *>(b.constData()) + o);
    }
    QByteArray m_previous;
    quint64 m_replaced = 0, m_timeouts = 0, m_failures = 0;
};
