// Linux Bluetooth link: BlueZ over D-Bus.
#include "link.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QVariantMap>

using InterfaceList = QMap<QString, QVariantMap>;
using ManagedObjects = QMap<QDBusObjectPath, InterfaceList>;
Q_DECLARE_METATYPE(InterfaceList)
Q_DECLARE_METATYPE(ManagedObjects)

namespace {

const char *BLUEZ = "org.bluez";
const char *DEVICE = "org.bluez.Device1";
const char *GATT_CHAR = "org.bluez.GattCharacteristic1";
const char *PROPS = "org.freedesktop.DBus.Properties";
const QString RX_UUID = "0783b03e-8535-b5a0-7140-a304d2495cba";   // computer -> radio
const QString TX_UUID = "0783b03e-8535-b5a0-7140-a304d2495cb8";   // radio -> computer
const quint16 HYTERA_MANUFACTURER = 4098;                         // seen in the S1 Pro advertisement

QDBusConnection bus() { return QDBusConnection::systemBus(); }

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


class BluezLink : public Link {
    Q_OBJECT
public:
    using Link::Link;
    ~BluezLink() override { close(); }
    void open(const QString &address) override;
    void close() override;
    void send(const QByteArray &bytes) override;

private slots:
    void onProperties(const QString &iface, const QVariantMap &changed, const QStringList &)
    {
        if (iface == QLatin1String(GATT_CHAR) && changed.contains("Value"))
            emit received(changed.value("Value").toByteArray());
    }

private:
    QString devicePath_, rxPath_, txPath_;
    int chunk_ = 20;
    bool connected_ = false;
};

void BluezLink::open(const QString &address)
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
}

void BluezLink::close()
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
}

void BluezLink::send(const QByteArray &bytes)
{
    QDBusInterface rx(BLUEZ, rxPath_, GATT_CHAR, bus());
    rx.setTimeout(5000);
    const QVariantMap options{{"type", "command"}};          // write without response
    for (int i = 0; i < bytes.size(); i += chunk_) {
        QDBusReply<void> r = rx.call("WriteValue", bytes.mid(i, chunk_), options);
        if (!r.isValid())
            throw RadioError("Bluetooth link lost (" + r.error().message() + ")");
    }
}

} // namespace

Link *Link::create(QObject *parent) { return new BluezLink(parent); }

QString Link::find(int timeoutMs, QString *name)
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

#include "link_bluez.moc"
