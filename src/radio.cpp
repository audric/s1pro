#include "radio.h"

#include <algorithm>

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QStringDecoder>
#include <QTimer>
#include <QtEndian>

using InterfaceList = QMap<QString, QVariantMap>;
using ManagedObjects = QMap<QDBusObjectPath, InterfaceList>;
Q_DECLARE_METATYPE(InterfaceList)
Q_DECLARE_METATYPE(ManagedObjects)

namespace {

const char *BLUEZ = "org.bluez";
const char *DEVICE = "org.bluez.Device1";
const char *GATT_CHAR = "org.bluez.GattCharacteristic1";
const char *PROPS = "org.freedesktop.DBus.Properties";
const QString RX_UUID = "0783b03e-8535-b5a0-7140-a304d2495cba";   // laptop -> radio
const QString TX_UUID = "0783b03e-8535-b5a0-7140-a304d2495cb8";   // radio -> laptop
const quint16 HYTERA_MANUFACTURER = 4098;                         // seen in the S1 Pro advertisement

enum : quint8 { OP_DATA = 0x00, OP_ACK = 0x10, OP_CONNECT = 0xFE, OP_ACCEPT = 0xFD };
enum : quint16 { CMD_QUERY = 0x0003, CMD_PARAM = 0x0101, MOD_PARAM = 0x0006 };

QDBusConnection bus() { return QDBusConnection::systemBus(); }

void pump(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

void be16(QByteArray &b, quint16 v) { b.append(char(v >> 8)).append(char(v)); }
void be32(QByteArray &b, quint32 v) { be16(b, v >> 16); be16(b, v & 0xFFFF); }
void le16(QByteArray &b, quint16 v) { b.append(char(v)).append(char(v >> 8)); }
void le32(QByteArray &b, quint32 v) { le16(b, v & 0xFFFF); le16(b, v >> 16); }
quint16 rdBe16(const QByteArray &b, int at) { return qFromBigEndian<quint16>(b.constData() + at); }
quint16 rdLe16(const QByteArray &b, int at) { return qFromLittleEndian<quint16>(b.constData() + at); }
quint32 rdLe32(const QByteArray &b, int at) { return qFromLittleEndian<quint32>(b.constData() + at); }

quint16 checksum(const QByteArray &frame)
{
    quint32 sum = 0;
    for (int i = 0; i < frame.size(); i += 2) {
        quint8 hi = frame[i], lo = i + 1 < frame.size() ? quint8(frame[i + 1]) : 0;
        sum += (hi << 8) | lo;
    }
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return ~sum & 0xFFFF;
}

QByteArray linkFrame(quint8 op, quint16 pn, const QByteArray &payload = {})
{
    QByteArray f = QByteArray::fromHex("5e6e01");
    f.append(char(op)).append(char(0x20)).append(char(0x10));
    be16(f, pn);
    f.append(QByteArray::fromHex("0000000101000000"));
    be16(f, 20 + payload.size());
    be16(f, 0);
    f.append(payload);
    quint16 ck = checksum(f);
    f[18] = char(ck >> 8);
    f[19] = char(ck);
    return f;
}

ManagedObjects managedObjects()
{
    static bool registered = false;
    if (!registered) {
        qDBusRegisterMetaType<InterfaceList>();
        qDBusRegisterMetaType<ManagedObjects>();
        registered = true;
    }
    QDBusInterface om(BLUEZ, "/", "org.freedesktop.DBus.ObjectManager", bus());
    QDBusReply<ManagedObjects> reply = om.call("GetManagedObjects");
    if (!reply.isValid())
        throw RadioError("cannot talk to the Bluetooth service: " + reply.error().message());
    return reply.value();
}

QString adapterPath(const ManagedObjects &objects)
{
    for (auto it = objects.begin(); it != objects.end(); ++it)
        if (it.value().contains("org.bluez.Adapter1")) {
            if (!it.value()["org.bluez.Adapter1"].value("Powered").toBool())
                throw RadioError("Bluetooth is switched off on this computer");
            return it.key().path();
        }
    throw RadioError("this computer has no Bluetooth adapter (airplane mode?)");
}

bool hasManufacturer(const QVariant &v, quint16 wanted)
{
    if (!v.canConvert<QDBusArgument>())
        return false;
    const QDBusArgument arg = v.value<QDBusArgument>();
    bool found = false;
    arg.beginMap();
    while (!arg.atEnd()) {
        quint16 id;
        QDBusVariant data;
        arg.beginMapEntry();
        arg >> id >> data;
        arg.endMapEntry();
        found = found || id == wanted;
    }
    arg.endMap();
    return found;
}

// Runs discovery for the lifetime of the object.
struct Discovery {
    QDBusInterface adapter;
    explicit Discovery(const QString &path) : adapter(BLUEZ, path, "org.bluez.Adapter1", bus())
    {
        adapter.call("StartDiscovery");
    }
    ~Discovery() { adapter.call("StopDiscovery"); }
};

QString callAndWait(QDBusInterface &iface, const QString &method, int timeoutMs)
{
    iface.setTimeout(timeoutMs);
    QDBusPendingCallWatcher watcher(iface.asyncCall(method));
    QEventLoop loop;
    QObject::connect(&watcher, &QDBusPendingCallWatcher::finished, &loop, &QEventLoop::quit);
    loop.exec();
    return watcher.isError() ? watcher.error().message() : QString();
}

} // namespace

Radio::Radio(QObject *parent) : QObject(parent) {}

Radio::~Radio() { close(); }

QString Radio::find(int timeoutMs, QString *name)
{
    Discovery discovery(adapterPath(managedObjects()));
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < timeoutMs) {
        pump(500);
        const ManagedObjects objects = managedObjects();
        for (auto it = objects.begin(); it != objects.end(); ++it) {
            const QVariantMap dev = it.value().value(DEVICE);
            // RSSI is only present for devices heard during this scan.
            if (dev.contains("RSSI") && hasManufacturer(dev.value("ManufacturerData"), HYTERA_MANUFACTURER)) {
                if (name)
                    *name = dev.value("Name").toString();
                return dev.value("Address").toString();
            }
        }
    }
    return {};
}

void Radio::open(const QString &address)
{
    close();
    {
        ManagedObjects objects = managedObjects();
        Discovery discovery(adapterPath(objects));
        QElapsedTimer clock;
        clock.start();
        for (;;) {
            for (auto it = objects.begin(); it != objects.end(); ++it) {
                const QVariantMap dev = it.value().value(DEVICE);
                // A device BlueZ already knows can be connected straight away; BlueZ waits for
                // its next advertisement itself, which is quicker than us polling for it.
                if (dev.value("Address").toString().compare(address, Qt::CaseInsensitive) == 0)
                    devicePath_ = it.key().path();
            }
            if (!devicePath_.isEmpty())
                break;
            if (clock.elapsed() > 12000)
                throw RadioError("radio " + address + " not found: is it on, with Bluetooth enabled "
                                 "and no phone connected to it?");
            pump(500);
            objects = managedObjects();
        }
        QDBusInterface device(BLUEZ, devicePath_, DEVICE, bus());
        const QString error = callAndWait(device, "Connect", 30000);
        if (!error.isEmpty()) {
            devicePath_.clear();
            throw RadioError("cannot connect to the radio: " + error);
        }
        connected_ = true;
    }

    // Wait for GATT discovery, then locate the two characteristics.
    QElapsedTimer clock;
    clock.start();
    QVariantMap rx;
    while (rxPath_.isEmpty() || txPath_.isEmpty()) {
        if (clock.elapsed() > 15000)
            throw RadioError("the radio did not expose its programming service");
        pump(300);
        const ManagedObjects objects = managedObjects();
        for (auto it = objects.begin(); it != objects.end(); ++it) {
            if (!it.key().path().startsWith(devicePath_ + "/"))
                continue;
            const QVariantMap chr = it.value().value(GATT_CHAR);
            if (chr.value("UUID").toString() == RX_UUID) {
                rxPath_ = it.key().path();
                rx = chr;
            } else if (chr.value("UUID").toString() == TX_UUID) {
                txPath_ = it.key().path();
            }
        }
    }
    const int mtu = rx.value("MTU").toInt();
    chunk_ = qBound(20, (mtu ? mtu : 23) - 3, 509);

    bus().connect(BLUEZ, txPath_, PROPS, "PropertiesChanged", this,
                  SLOT(onProperties(QString, QVariantMap, QStringList)));
    QDBusInterface tx(BLUEZ, txPath_, GATT_CHAR, bus());
    QDBusReply<void> notify = tx.call("StartNotify");
    if (!notify.isValid())
        throw RadioError("cannot subscribe to the radio: " + notify.error().message());

    // The radio often misses the first connect frame; the app sends it twice too.
    bool accepted = false;
    for (int attempt = 0; attempt < 10 && !accepted; ++attempt) {
        sendRaw(linkFrame(OP_CONNECT, 0));
        Frame f;
        accepted = nextFrame(600, &f) && f.op == OP_ACCEPT;
    }
    if (!accepted)
        throw RadioError("the radio did not accept the link");

    // Session opening the app sends before using parameters.
    request(CMD_PARAM, MOD_PARAM, QByteArray::fromHex("000200010000000100"));
}

void Radio::close()
{
    if (!txPath_.isEmpty())
        bus().disconnect(BLUEZ, txPath_, PROPS, "PropertiesChanged", this,
                         SLOT(onProperties(QString, QVariantMap, QStringList)));
    if (connected_) {
        QDBusInterface device(BLUEZ, devicePath_, DEVICE, bus());
        device.call("Disconnect");
    }
    connected_ = false;
    devicePath_.clear();
    rxPath_.clear();
    txPath_.clear();
    buffer_.clear();
    inbox_.clear();
    pn_ = 0;
}

// Set S1PRO_TRACE=1 to print every frame, for protocol work.
static void trace(const char *direction, const QByteArray &frame)
{
    static const bool on = qEnvironmentVariableIsSet("S1PRO_TRACE");
    if (on)
        fprintf(stderr, "  %s %s\n", direction, frame.toHex().constData());
}

void Radio::sendRaw(const QByteArray &frame)
{
    trace(">", frame);
    QDBusInterface rx(BLUEZ, rxPath_, GATT_CHAR, bus());
    rx.setTimeout(5000);
    const QVariantMap options{{"type", "command"}};
    for (int i = 0; i < frame.size(); i += chunk_) {
        QDBusReply<void> r = rx.call("WriteValue", frame.mid(i, chunk_), options);
        if (!r.isValid())
            throw RadioError("Bluetooth link lost (" + r.error().message() + ")");
    }
}

void Radio::onProperties(const QString &iface, const QVariantMap &changed, const QStringList &)
{
    if (iface != QLatin1String(GATT_CHAR) || !changed.contains("Value"))
        return;
    buffer_ += changed.value("Value").toByteArray();
    for (;;) {
        const int start = buffer_.indexOf(QByteArray::fromHex("5e6e"));
        if (start < 0) {
            buffer_.clear();
            return;
        }
        buffer_.remove(0, start);
        if (buffer_.size() < 20)
            return;
        const int len = rdBe16(buffer_, 16);
        if (len < 20) {
            buffer_.remove(0, 2);
            continue;
        }
        if (buffer_.size() < len)
            return;
        QByteArray frame = buffer_.left(len);
        buffer_.remove(0, len);
        trace("<", frame);
        const quint16 got = rdBe16(frame, 18);
        frame[18] = frame[19] = 0;
        if (checksum(frame) != got)
            continue;                                   // corrupted; the radio will resend
        const Frame f{quint8(frame[3]), rdBe16(frame, 6), frame.mid(20)};
        if (f.op == OP_ACK) {
            ackedPn_ = f.pn;
            continue;
        }
        if (f.op == OP_DATA) {
            try {
                sendRaw(linkFrame(OP_ACK, f.pn));
            } catch (const RadioError &) {
                // the request waiting for this frame will report the lost link
            }
        }
        inbox_.append(f);
    }
}

bool Radio::nextFrame(int timeoutMs, Frame *out)
{
    QElapsedTimer clock;
    clock.start();
    while (inbox_.isEmpty()) {
        if (clock.elapsed() >= timeoutMs)
            return false;
        pump(10);
    }
    *out = inbox_.takeFirst();
    return true;
}

QList<QByteArray> Radio::request(quint16 cmd, quint16 module, const QByteArray &data, int timeoutMs)
{
    const quint32 seq = ++seq_;
    QByteArray payload = QByteArray::fromHex("0a0b");
    be16(payload, cmd);
    be16(payload, module);
    be32(payload, seq);
    be16(payload, data.size());
    payload += data;
    const QByteArray frame = linkFrame(OP_DATA, ++pn_, payload);
    sendRaw(frame);

    QList<QByteArray> replies;
    int resends = 0, waits = 0;
    for (;;) {
        Frame f;
        if (!nextFrame(2000, &f)) {
            if (ackedPn_ == pn_) {
                // The radio has the command; never resend it (a write could apply twice).
                if (++waits * 2000 >= timeoutMs)
                    throw RadioError("no reply from the radio");
                continue;
            }
            // Not acknowledged: the frame was lost. The app resends as well.
            if (++resends > 3)
                throw RadioError("no reply from the radio");
            sendRaw(frame);
            continue;
        }
        const QByteArray &p = f.payload;
        if (f.op != OP_DATA || p.size() < 12 || !p.startsWith(QByteArray::fromHex("0a0b")))
            continue;
        const quint16 replyCmd = rdBe16(p, 2);
        const quint32 replySeq = qFromBigEndian<quint32>(p.constData() + 6);
        const QByteArray body = p.mid(12, rdBe16(p, 10));
        if (replyCmd == 0x3101 || replySeq != seq)
            continue;                                   // notification, or a late duplicate
        if (!replies.contains(body))
            replies.append(body);
        // Parameter replies can span frames; a first byte of 2 means more follow.
        if (cmd == CMD_PARAM && body.size() >= 8 && body[0] == 0x02)
            continue;
        return replies;
    }
}

QString Radio::query(quint16 item)
{
    QByteArray data;
    be16(data, item);
    const QByteArray body = request(CMD_QUERY, 0x0002, data).last();
    // reply: status(2) item(2) length(2) text, big-endian, text UTF-16BE
    if (body.size() < 6)
        return {};
    QStringDecoder utf16(QStringConverter::Utf16BE);
    QString text = utf16(body.mid(6, rdBe16(body, 4)));
    while (text.endsWith(QChar(0)))
        text.chop(1);
    return text;
}

// Sends `count` read items in one request and returns the reply items of all its frames, joined.
// A reply too long for one frame continues in the next; each frame but the last ends with a
// short marker item naming where the next one resumes, which the callers skip.
QByteArray Radio::params(const QByteArray &items, int count)
{
    QByteArray data;
    data.append(char(0x02)).append(char(count));
    data += items;
    QList<QByteArray> bodies = request(CMD_PARAM, MOD_PARAM, data);
    for (const QByteArray &body : bodies)
        if (body.size() < 8)
            throw RadioError("short parameter reply: " + body.toHex());
    // byte 5 is the frame's position in the reply
    std::sort(bodies.begin(), bodies.end(),
              [](const QByteArray &a, const QByteArray &b) { return quint8(a[5]) < quint8(b[5]); });
    QByteArray blob;
    for (const QByteArray &body : bodies)
        blob += body.mid(8);
    return blob;
}

int Radio::rowCount(quint32 keyId)
{
    QByteArray item = QByteArray::fromHex("0603");
    le16(item, 1);
    le32(item, keyId);
    const QByteArray blob = params(item);
    if (blob.size() < 10 || !blob.startsWith(QByteArray::fromHex("0606")))
        throw RadioError("unexpected reply to row count: " + blob.left(16).toHex());
    return rdLe16(blob, 8);
}

QList<Fields> Radio::readRows(int first, int count, const QList<quint32> &ids)
{
    QByteArray items;
    for (int row = first; row < first + count; ++row) {
        items += QByteArray::fromHex("0313");
        le16(items, row);
        le16(items, ids.size());
        for (quint32 id : ids)
            le32(items, id);
    }
    const QByteArray blob = params(items, count);
    QList<Fields> rows(count);
    // A row can be split between frames: it then appears as two items, each carrying some of
    // its fields (or none at all, as a bare marker). Collect fields per row.
    int pos = 0;
    while (pos + 6 <= blob.size() && blob[pos] == 0x03 && blob[pos + 1] == 0x14) {
        const int row = rdLe16(blob, pos + 2) - first;
        int n = rdLe16(blob, pos + 4);
        pos += 6;
        if (n && (row < 0 || row >= count))
            throw RadioError("the radio answered with a row that was not asked for");
        for (; n > 0; --n) {
            if (pos + 6 > blob.size())
                throw RadioError("truncated row from the radio");
            const quint32 id = rdLe32(blob, pos);
            const int len = rdLe16(blob, pos + 4);
            rows[row].append({id, blob.mid(pos + 6, len)});
            pos += 6 + len;
        }
    }
    for (const Fields &fields : rows)
        if (fields.size() != ids.size() || pos != blob.size())
            throw RadioError("incomplete reply to row read");
    return rows;
}

QList<QList<QPair<int, int>>> Radio::readZoneChannelRange(int first, int count)
{
    QByteArray items;
    for (int zone = first; zone < first + count; ++zone) {
        items += QByteArray::fromHex("0361");
        le16(items, zone);
        le16(items, 0);          // first entry
        le16(items, 7);          // list kind, as sent by the app
        le16(items, 6);
        le16(items, 0xFFFF);     // all entries
    }
    const QByteArray blob = params(items, count);
    QList<QList<QPair<int, int>>> zones(count);
    int pos = 0;
    while (pos + 8 <= blob.size() && blob[pos] == 0x03 && blob[pos + 1] == 0x64) {
        if (rdLe16(blob, pos + 6) == 0) {           // continuation marker: 03 64 zone entry 00 00
            pos += 8;
            continue;
        }
        if (pos + 14 > blob.size())
            break;
        const int zone = rdLe16(blob, pos + 2) - first;
        const int len = rdLe16(blob, pos + 12);
        if (zone < 0 || zone >= count || len < 4 || pos + 14 + len > blob.size())
            throw RadioError("unexpected reply to zone read");
        zones[zone].append({rdLe16(blob, pos + 14), rdLe16(blob, pos + 16)});
        pos += 14 + len;
    }
    if (pos != blob.size())
        throw RadioError("unexpected reply to zone read");
    return zones;
}

QList<QList<QPair<int, int>>> Radio::readZoneChannels(int count)
{
    // Zones can hold a hundred entries each; a few per request keeps replies manageable.
    QList<QList<QPair<int, int>>> zones;
    for (int first = 0; first < count; first += 4)
        zones += readZoneChannelRange(first, qMin(4, count - first));
    return zones;
}

static QByteArray packFields(const Fields &values)
{
    QByteArray out;
    for (const auto &[id, raw] : values) {
        le32(out, id);
        le16(out, raw.size());
        out += raw;
    }
    return out;
}

void Radio::write(const QByteArray &item)
{
    // The radio stores each write to flash and can take many seconds to answer when busy.
    const QByteArray body = request(CMD_PARAM, MOD_PARAM, QByteArray::fromHex("0201") + item, 30000).last();
    if (body.size() < 2 || body[0] != 0 || body[1] != 0)
        throw RadioError("the radio rejected the write: " + body.toHex());
}

void Radio::writeRow(int row, const Fields &values)
{
    QByteArray item = QByteArray::fromHex("0214");
    le16(item, row);
    le16(item, values.size());
    write(item + packFields(values));
}

void Radio::addRow(const Fields &values)
{
    QByteArray item = QByteArray::fromHex("0004");
    le16(item, values.size());
    write(item + packFields(values));
}

void Radio::deleteRow(quint32 keyId, int row)
{
    QByteArray item = QByteArray::fromHex("0107");
    le32(item, keyId);
    le16(item, 1);
    le16(item, row);
    le16(item, 1);
    write(item);
}

void Radio::addZoneChannel(int zone, int channel, int type)
{
    QByteArray number(1, char(channel));
    if (channel > 0xFF)
        number.append(char(channel >> 8));
    QByteArray item = QByteArray::fromHex("0014");
    le16(item, zone);
    le16(item, 2);
    write(item + packFields({{1323, number}, {1324, QByteArray(1, char(type))}}));
}

void Radio::deleteZoneChannel(int zone, int entry)
{
    QByteArray item = QByteArray::fromHex("0117");
    le16(item, zone);
    le32(item, 1323);
    le16(item, 1);
    le16(item, entry);
    le16(item, 1);
    write(item);
}
