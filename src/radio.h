// Hytera S1 Pro (HYT-S35) programming link over Bluetooth LE, via BlueZ on D-Bus.
//
// Protocol recovered from packet captures of the official Hytera Radio app; see docs/PROTOCOL.md.
//
//   BLE     write-without-response to the RX characteristic, notifications from TX.
//   Link    5E 6E | ver 01 | op | src | dst | pn(2) | 8 fixed bytes | len(2) | cksum(2) | payload
//           op: FE connect, FD accept, 00 data, 10 ack. The checksum is the 16-bit internet
//           checksum of the whole frame with the checksum field zeroed.
//   App     0A 0B | cmd(2) | module(2) | seq(4) | len(2) | data            (big-endian)
//           Replies carry cmd | 0x1000; unsolicited notifications use cmd 0x3101.
//   Params  cmd 0x0101, module 6: data = 02 | n_items | items              (little-endian)
//           03 03 n ids...               read scalar parameters
//           03 13 row n ids...           read parameters of one table row
//           06 03 01 id                  number of rows in the table that owns `id`
//           03 61 zone 0 7 6 FFFF        read a zone's channel list (reply items 03 64, 4 bytes each)
//           02 14 row n (id len value)   write parameters of one table row
//           00 04 n (id len value)       append a row
//           01 07 id 01 row 01           delete a row
//           00 14 zone 2 (1323 ch)(1324 type)   append a channel to a zone
//           01 17 zone 1323 01 entry 01         remove the zone's entry at that position
#pragma once
#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QVariantMap>
#include <stdexcept>

struct RadioError : std::runtime_error {
    explicit RadioError(const QString &m) : std::runtime_error(m.toStdString()) {}
};

using Fields = QList<QPair<quint32, QByteArray>>;   // parameter id -> raw value, in order

class Radio : public QObject {
    Q_OBJECT
public:
    explicit Radio(QObject *parent = nullptr);
    ~Radio() override;

    // Scan for an S1 Pro. Returns its address ("" if none) and sets *name.
    static QString find(int timeoutMs, QString *name = nullptr);

    void open(const QString &address);   // connect BLE, start the link, open a parameter session
    void close();

    QString query(quint16 item);         // 1 firmware, 11 serial, 2 codeplug version
    int rowCount(quint32 keyId);
    // Rows first .. first+count-1 in one request (keep count small: the reply is ~200 bytes a row).
    QList<Fields> readRows(int first, int count, const QList<quint32> &ids);
    // Members of zones 0 .. count-1, in order: (channel number starting at 1, 0 digital / 1 analog).
    QList<QList<QPair<int, int>>> readZoneChannels(int count);
    QList<QPair<int, int>> readZoneChannels1(int zone) { return readZoneChannelRange(zone, 1).value(0); }
    void writeRow(int row, const Fields &values);
    void addRow(const Fields &values);
    void deleteRow(quint32 keyId, int row);
    void addZoneChannel(int zone, int channel, int type);   // appended at the end of the zone
    void deleteZoneChannel(int zone, int entry);            // entry = position in the zone, from 0

private slots:
    void onProperties(const QString &iface, const QVariantMap &changed, const QStringList &);

private:
    struct Frame { quint8 op; quint16 pn; QByteArray payload; };
    void sendRaw(const QByteArray &frame);
    bool nextFrame(int timeoutMs, Frame *out);
    QList<QByteArray> request(quint16 cmd, quint16 module, const QByteArray &data, int timeoutMs = 8000);
    QByteArray params(const QByteArray &items, int count = 1);
    QList<QList<QPair<int, int>>> readZoneChannelRange(int first, int count);
    void write(const QByteArray &item);

    QString devicePath_, rxPath_, txPath_;
    int chunk_ = 20;
    bool connected_ = false;
    QByteArray buffer_;
    QList<Frame> inbox_;
    quint16 pn_ = 0;
    int ackedPn_ = -1;
    quint32 seq_ = 0x100;
};
