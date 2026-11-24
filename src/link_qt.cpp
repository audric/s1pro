// Bluetooth link through Qt Bluetooth: macOS (CoreBluetooth), Windows (WinRT), and Linux too.
#include "link.h"

#include <QBluetoothDeviceDiscoveryAgent>
#include <QBluetoothDeviceInfo>
#include <QBluetoothUuid>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHash>
#include <QLowEnergyController>
#include <QLowEnergyService>
#include <functional>
#include <optional>
#if QT_CONFIG(permissions)
#include <QPermissions>
#endif

namespace {

const QBluetoothUuid SERVICE_UUID(QStringLiteral("0783b03e-8535-b5a0-7140-a304d2495cb7"));
const QBluetoothUuid RX_UUID(QStringLiteral("0783b03e-8535-b5a0-7140-a304d2495cba"));   // computer -> radio
const QBluetoothUuid TX_UUID(QStringLiteral("0783b03e-8535-b5a0-7140-a304d2495cb8"));   // radio -> computer
const quint16 HYTERA_MANUFACTURER = 4098;                         // seen in the S1 Pro advertisement

// Devices seen by a scan in this run. A connection needs the scan's device record, not just an
// identifier (macOS has no other way to name a device).
QHash<QString, QBluetoothDeviceInfo> known;

QString idOf(const QBluetoothDeviceInfo &device)
{
#ifdef Q_OS_MACOS
    return device.deviceUuid().toString(QUuid::WithoutBraces);
#else
    return device.address().toString();
#endif
}

bool waitFor(const std::function<bool()> &done, int timeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    while (!done()) {
        if (clock.elapsed() > timeoutMs)
            return false;
        pump(20);
    }
    return true;
}

void ensurePermission()
{
#if QT_CONFIG(permissions)
    QBluetoothPermission permission;
    permission.setCommunicationModes(QBluetoothPermission::Access);
    if (qApp->checkPermission(permission) == Qt::PermissionStatus::Undetermined) {
        bool answered = false;
        qApp->requestPermission(permission, qApp, [&answered](const QPermission &) { answered = true; });
        waitFor([&] { return answered; }, 120000);
    }
    if (qApp->checkPermission(permission) != Qt::PermissionStatus::Granted)
        throw RadioError("This program is not allowed to use Bluetooth. "
                         "Allow it in the system's privacy settings and try again.");
#endif
}

std::optional<QBluetoothDeviceInfo> scan(const std::function<bool(const QBluetoothDeviceInfo &)> &match,
                                         int timeoutMs)
{
    ensurePermission();
    QBluetoothDeviceDiscoveryAgent agent;
    agent.setLowEnergyDiscoveryTimeout(timeoutMs);
    std::optional<QBluetoothDeviceInfo> found;
    bool finished = false;
    QString error;
    auto seen = [&](const QBluetoothDeviceInfo &device) {
        if (!found && match(device))
            found = device;
    };
    QObject::connect(&agent, &QBluetoothDeviceDiscoveryAgent::deviceDiscovered, &agent, seen);
    QObject::connect(&agent, &QBluetoothDeviceDiscoveryAgent::deviceUpdated, &agent,
                     [&](const QBluetoothDeviceInfo &device, QBluetoothDeviceInfo::Fields) { seen(device); });
    QObject::connect(&agent, &QBluetoothDeviceDiscoveryAgent::finished, &agent, [&] { finished = true; });
    QObject::connect(&agent, &QBluetoothDeviceDiscoveryAgent::canceled, &agent, [&] { finished = true; });
    QObject::connect(&agent, &QBluetoothDeviceDiscoveryAgent::errorOccurred, &agent, [&] {
        error = agent.errorString();
        finished = true;
    });
    agent.start(QBluetoothDeviceDiscoveryAgent::LowEnergyMethod);
    waitFor([&] { return found.has_value() || finished; }, timeoutMs + 2000);
    agent.stop();
    if (!found && !error.isEmpty())
        throw RadioError("Bluetooth scan failed: " + error);
    if (found)
        known.insert(idOf(*found).toLower(), *found);
    return found;
}

class QtLink : public Link {
public:
    using Link::Link;
    ~QtLink() override { close(); }

    void open(const QString &id) override
    {
        close();
        ensurePermission();
        if (!known.contains(id.toLower())
            && !scan([&](const QBluetoothDeviceInfo &d) { return idOf(d).compare(id, Qt::CaseInsensitive) == 0; },
                     12000))
            throw RadioError("radio " + id + " not found: is it on, with Bluetooth enabled "
                             "and no phone connected to it?");

        error_.clear();
        controller_ = QLowEnergyController::createCentral(known.value(id.toLower()), this);
        connect(controller_, &QLowEnergyController::errorOccurred, this,
                [this] { error_ = controller_->errorString(); });
        controller_->connectToDevice();
        if (!waitFor([&] { return controller_->state() == QLowEnergyController::ConnectedState || failed(); }, 30000)
            || failed())
            fail("cannot connect to the radio");

        bool discovered = false;
        connect(controller_, &QLowEnergyController::discoveryFinished, this, [&discovered] { discovered = true; });
        controller_->discoverServices();
        if (!waitFor([&] { return discovered || failed(); }, 15000) || failed())
            fail("cannot list the radio's services");
        service_ = controller_->createServiceObject(SERVICE_UUID, this);
        if (!service_)
            throw RadioError("the radio did not expose its programming service");
        connect(service_, &QLowEnergyService::errorOccurred, this,
                [this](QLowEnergyService::ServiceError e) { error_ = QString("service error %1").arg(int(e)); });
        service_->discoverDetails();
        if (!waitFor([&] { return service_->state() == QLowEnergyService::RemoteServiceDiscovered || failed(); }, 15000)
            || failed())
            fail("cannot read the radio's programming service");

        rx_ = service_->characteristic(RX_UUID);
        const QLowEnergyCharacteristic tx = service_->characteristic(TX_UUID);
        if (!rx_.isValid() || !tx.isValid())
            throw RadioError("the radio did not expose its programming service");
        connect(service_, &QLowEnergyService::characteristicChanged, this,
                [this](const QLowEnergyCharacteristic &c, const QByteArray &value) {
                    if (c.uuid() == TX_UUID)
                        emit received(value);
                });

        const QLowEnergyDescriptor cccd = tx.clientCharacteristicConfiguration();
        if (!cccd.isValid())
            throw RadioError("cannot subscribe to the radio");
        bool subscribed = false;
        connect(service_, &QLowEnergyService::descriptorWritten, this, [&subscribed] { subscribed = true; });
        service_->writeDescriptor(cccd, QLowEnergyCharacteristic::CCCDEnableNotification);
        if (!waitFor([&] { return subscribed || failed(); }, 8000) || failed())
            fail("cannot subscribe to the radio");

        const int mtu = controller_->mtu();
        chunk_ = qBound(20, (mtu > 0 ? mtu : 23) - 3, 509);
    }

    void close() override
    {
        if (controller_) {
            // Stop our lambdas (some capture locals of open()) before tearing down.
            controller_->disconnect(this);
            if (service_)
                service_->disconnect(this);
            if (controller_->state() != QLowEnergyController::UnconnectedState) {
                controller_->disconnectFromDevice();
                waitFor([&] { return controller_->state() == QLowEnergyController::UnconnectedState; }, 3000);
            }
        }
        delete service_;
        delete controller_;
        service_ = nullptr;
        controller_ = nullptr;
        rx_ = {};
    }

    void send(const QByteArray &bytes) override
    {
        if (!service_ || failed() || controller_->state() == QLowEnergyController::UnconnectedState
            || controller_->state() == QLowEnergyController::ClosingState)
            throw RadioError("Bluetooth link lost" + (error_.isEmpty() ? QString() : " (" + error_ + ")"));
        for (int i = 0; i < bytes.size(); i += chunk_)
            service_->writeCharacteristic(rx_, bytes.mid(i, chunk_), QLowEnergyService::WriteWithoutResponse);
    }

private:
    bool failed() const { return !error_.isEmpty(); }
    [[noreturn]] void fail(const QString &what)
    {
        throw RadioError(what + (error_.isEmpty() ? QString(": no answer") : ": " + error_));
    }

    QLowEnergyController *controller_ = nullptr;
    QLowEnergyService *service_ = nullptr;
    QLowEnergyCharacteristic rx_;
    QString error_;
    int chunk_ = 20;
};

} // namespace

Link *Link::create(QObject *parent) { return new QtLink(parent); }

QString Link::find(int timeoutMs, QString *name)
{
    const auto device = scan(
        [](const QBluetoothDeviceInfo &d) { return d.manufacturerIds().contains(HYTERA_MANUFACTURER); }, timeoutMs);
    if (!device)
        return {};
    if (name)
        *name = device->name();
    return idOf(*device);
}
