// The Bluetooth LE connection to the radio: finds it, connects, and carries raw bytes each way.
// Two implementations: link_bluez.cpp (Linux, BlueZ over D-Bus) and link_qt.cpp (Qt Bluetooth,
// for macOS and Windows; also builds on Linux). CMake picks one with S1PRO_BLUETOOTH.
#pragma once
#include <QByteArray>
#include <QObject>
#include <QString>
#include <stdexcept>

struct RadioError : std::runtime_error {
    explicit RadioError(const QString &m) : std::runtime_error(m.toStdString()) {}
};

void pump(int ms);   // run the event loop for a while

class Link : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    static Link *create(QObject *parent);

    // Scan for an S1 Pro. Returns the identifier to pass to open() ("" if none) and sets *name.
    // The identifier is the Bluetooth address, except on macOS, which only gives a per-Mac UUID.
    static QString find(int timeoutMs, QString *name);

    virtual void open(const QString &id) = 0;          // throws RadioError
    virtual void close() = 0;
    virtual void send(const QByteArray &bytes) = 0;    // throws RadioError when the link is gone

signals:
    void received(const QByteArray &bytes);            // in arbitrary pieces
};
