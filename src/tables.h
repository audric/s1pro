// The radio's editable tables, as text grids: what the GUI shows and what gets synced.
#pragma once
#include "radio.h"

#include <QMap>
#include <QStringList>
#include <functional>

struct Column {
    quint32 id;
    const char *name;     // parameter name from the Hytera app's dictionary (used in saved files)
    const char *header;   // friendlier grid header, or nullptr to use the name
};

struct Table {
    const char *key;
    const char *title;
    quint32 keyId;        // parameter that identifies the table for row count / delete
    QList<Column> columns;
    // Zones carry one more column after `columns`: their channels, as names separated by ", ".
    const char *extraName = nullptr;
    const char *extraHeader = nullptr;
};

using Rows = QList<QStringList>;          // one QStringList per row, in column order
using Codeplug = QMap<QString, Rows>;     // table key -> rows

const QList<Table> &tables();

// Called as work proceeds: what is happening, how many steps are done, and the total
// (0 when unknown). Set to nullptr to stop reporting.
using Progress = std::function<void(const QString &what, int done, int total)>;
void setProgress(Progress progress);

Rows readTable(Radio &radio, const Table &table);

// Everything the editor shows, zone membership included.
Codeplug readCodeplug(Radio &radio);

// Make the radio match `wanted`. With write == false nothing is sent and the changes are only
// listed in *log. Returns the number of rows that differ. Throws RadioError on bad cell values.
int sync(Radio &radio, const Codeplug &wanted, bool write, QStringList *log);
