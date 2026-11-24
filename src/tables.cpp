#include "tables.h"

#include <QStringDecoder>
#include <QStringEncoder>

namespace {

enum class Kind { Integer, Text, Frequency, Tone };

Kind kindOf(quint32 id)
{
    switch (id) {
    case 917: case 690: case 972: case 1325: case 1326: return Kind::Text;
    case 924: case 925: case 697: case 698: return Kind::Frequency;   // stored in Hz
    case 699: case 701: return Kind::Tone;                            // stored in tenths of a Hz
    default: return Kind::Integer;
    }
}

quint64 toInt(const QByteArray &raw)
{
    quint64 v = 0;
    for (int i = raw.size() - 1; i >= 0; --i)
        v = (v << 8) | quint8(raw[i]);
    return v;
}

// 430325000 with 6 decimals -> "430.325"
QString fixedPoint(quint64 v, int decimals)
{
    quint64 scale = 1;
    for (int i = 0; i < decimals; ++i)
        scale *= 10;
    QString frac = QString::number(v % scale).rightJustified(decimals, '0');
    while (frac.endsWith('0'))
        frac.chop(1);
    return QString::number(v / scale) + (frac.isEmpty() ? QString() : "." + frac);
}

bool parseFixed(QString text, int decimals, quint64 *out)
{
    text = text.trimmed().replace(',', '.');
    const QStringList parts = text.split('.');
    if (parts.size() > 2 || parts[0].isEmpty())
        return false;
    QString frac = parts.size() == 2 ? parts[1] : QString();
    if (frac.size() > decimals)
        return false;
    bool ok1 = false, ok2 = true;
    quint64 v = parts[0].toULongLong(&ok1);
    for (int i = 0; i < decimals; ++i)
        v *= 10;
    if (!frac.isEmpty())
        v += frac.leftJustified(decimals, '0').toULongLong(&ok2);
    *out = v;
    return ok1 && ok2;
}

QString show(quint32 id, const QByteArray &raw)
{
    switch (kindOf(id)) {
    case Kind::Text: {
        QStringDecoder utf16(QStringConverter::Utf16LE);
        QString text = utf16(raw);
        const int nul = text.indexOf(QChar(0));
        return nul < 0 ? text : text.left(nul);
    }
    case Kind::Frequency: return fixedPoint(toInt(raw), 6);
    case Kind::Tone: return fixedPoint(toInt(raw), 1);
    case Kind::Integer: break;
    }
    return QString::number(toInt(raw));
}

// `length` is the size the radio itself reported for this parameter.
bool parse(quint32 id, const QString &text, int length, QByteArray *out, QString *why)
{
    quint64 v = 0;
    switch (kindOf(id)) {
    case Kind::Text: {
        if (text.size() > 16) {
            *why = "longer than 16 characters";
            return false;
        }
        QStringEncoder utf16(QStringConverter::Utf16LE);
        *out = utf16(text);
        return true;
    }
    case Kind::Frequency:
        if (!parseFixed(text, 6, &v)) { *why = "not a frequency in MHz"; return false; }
        break;
    case Kind::Tone:
        if (!parseFixed(text, 1, &v)) { *why = "not a tone in Hz"; return false; }
        break;
    case Kind::Integer: {
        bool ok = false;
        v = text.trimmed().toULongLong(&ok);
        if (!ok) { *why = "not a whole number"; return false; }
        break;
    }
    }
    if (length < 8 && (v >> (8 * length))) {
        *why = "too large";
        return false;
    }
    out->clear();
    for (int i = 0; i < length; ++i)
        out->append(char(v >> (8 * i)));
    return true;
}

QList<quint32> ids(const Table &t)
{
    QList<quint32> out;
    for (const Column &c : t.columns)
        out.append(c.id);
    return out;
}

Progress progress;

void step(const QString &what, int done, int total)
{
    if (progress)
        progress(what, done, total);
}

QList<Fields> readRaw(Radio &radio, const Table &t)
{
    QList<Fields> rows;
    const int n = radio.rowCount(t.keyId);
    const int batch = 10;
    for (int first = 0; first < n; first += batch) {
        step(QString("Reading %1").arg(QString(t.title).toLower()), first, n);
        for (const Fields &got : radio.readRows(first, qMin(batch, n - first), ids(t))) {
            Fields ordered;                              // our column order, whatever the radio sends
            for (const Column &c : t.columns) {
                QByteArray raw;
                for (const auto &[id, value] : got)
                    if (id == c.id)
                        raw = value;
                ordered.append({c.id, raw});
            }
            rows.append(ordered);
        }
    }
    step(QString("Reading %1").arg(QString(t.title).toLower()), n, n);
    return rows;
}

} // namespace

void setProgress(Progress p) { progress = std::move(p); }

const QList<Table> &tables()
{
    // Column order for digital channels is the order the app uses when it adds one.
    static const QList<Table> all = {
        {"digital", "Digital channels", 917, {
            {917, "alias", "Name"}, {941, "slotOperation", "Slot (0/1)"}, {931, "colorCode", "Color code"},
            {924, "rxFreq", "RX MHz"}, {925, "txFreq", "TX MHz"}, {937, "txContactId", "Contact ID"},
            {938, "txContactType", "Contact type"}, {1513, "managerId", nullptr},
            {943, "ChannelModeStatus", nullptr}, {920, "TalkAround", nullptr},
            {964, "TalkAroundState", nullptr}, {932, "PrivateCallConfirmed", nullptr},
            {926, "TXAdmit", nullptr}, {927, "TXTimeoutTime", nullptr}, {928, "TOTPreAlertTime", nullptr},
            {951, "InCallTXAdmit", nullptr}, {952, "EmergencySystem", nullptr}, {960, "RRSChannel", nullptr},
            {961, "LocationInfoRevertChannel", nullptr}, {967, "RSSIThreshold", nullptr},
            {918, "ChannelType", nullptr}, {948, "RXOnly", nullptr}}},
        {"analog", "Analog channels", 690, {
            {690, "alias", "Name"}, {697, "rxFreq", "RX MHz"}, {698, "txFreq", "TX MHz"},
            {699, "RXCTCorCDCValue", "RX tone Hz"}, {700, "RXCTCorCDCType", "RX tone type"},
            {701, "TXCTCorCDCValue", "TX tone Hz"}, {702, "TXCTCorCDCType", "TX tone type"},
            {703, "RXSquelchMode", nullptr}, {691, "ChannelType", nullptr}, {693, "TalkAround", nullptr},
            {733, "TalkAroundState", nullptr}, {707, "TXAdmit", nullptr}, {708, "TXTimeOutTime", nullptr},
            {709, "TOTPreAlertTime", nullptr}, {713, "AutoResetTime", nullptr}, {719, "RXOnly", nullptr},
            {720, "CTCSSTailRevertOption", nullptr}, {722, "ChannelModeStatus", nullptr},
            {723, "EmergencySystem", nullptr}, {727, "PersonalityList", nullptr},
            {728, "TXAdmitNotAppliedinAutoResetMode", nullptr}, {731, "RSSIThreshold", nullptr}}},
        {"contacts", "Contacts", 972, {
            {972, "callAlias", "Name"}, {973, "callId", "DMR ID"},
            {974, "callType", "Type (1 group, 0 private)"}}},
        {"zones", "Zones", 1325, {
            {1325, "ZoneAlias", "Name"}, {1326, "ZoneIndexprefix", "Prefix"},
            {1327, "ZoneType", "Type (0 digital, 1 analog)"}},
         "channels", "Channels (double-click to edit)"},
    };
    return all;
}

Rows readTable(Radio &radio, const Table &table)
{
    Rows rows;
    for (const Fields &fields : readRaw(radio, table)) {
        QStringList row;
        for (const auto &[id, raw] : fields)
            row.append(show(id, raw));
        rows.append(row);
    }
    return rows;
}

namespace {

using Members = QList<QPair<int, int>>;   // (channel number from 1, 0 digital / 1 analog)

QString memberNames(const Members &members, const Codeplug &plug)
{
    QStringList names;
    for (const auto &[channel, type] : members) {
        const Rows &rows = plug[type ? "analog" : "digital"];
        names << (channel >= 1 && channel <= rows.size() ? rows[channel - 1].value(0)
                                                         : QString("%1%2?").arg(type ? "A" : "D").arg(channel));
    }
    return names.join(", ");
}

// Names are looked up in the tables being written, so zones follow channels that were moved.
Members resolveMembers(const QString &text, bool preferAnalog, const Codeplug &plug, int zone)
{
    Members out;
    for (QString name : text.split(',', Qt::SkipEmptyParts)) {
        name = name.trimmed();
        if (name.isEmpty())
            continue;
        int found = 0, foundType = 0;
        for (int pass = 0; pass < 2 && !found; ++pass) {
            const int type = (pass == 0) == preferAnalog ? 1 : 0;
            const Rows &rows = plug[type ? "analog" : "digital"];
            for (int r = 0; r < rows.size() && !found; ++r)
                if (rows[r].value(0).trimmed() == name)
                    found = r + 1, foundType = type;
        }
        if (!found)
            throw RadioError(QString("Zones, row %1: there is no channel named \"%2\"").arg(zone + 1).arg(name));
        out.append({found, foundType});
    }
    return out;
}

} // namespace

Codeplug readCodeplug(Radio &radio)
{
    Codeplug plug;
    for (const Table &t : tables())
        plug[t.key] = readTable(radio, t);
    step("Reading zone channels", 0, 0);
    const auto members = radio.readZoneChannels(plug["zones"].size());
    for (int z = 0; z < plug["zones"].size(); ++z)
        plug["zones"][z] << memberNames(members[z], plug);
    return plug;
}

int sync(Radio &radio, const Codeplug &wanted, bool write, QStringList *log)
{
    int pending = 0;
    Codeplug onRadio;                                   // channel names as the radio had them
    for (const Table &t : tables()) {
        if (!wanted.contains(t.key))
            continue;
        const Rows &rows = wanted[t.key];
        const QList<Fields> current = readRaw(radio, t);
        for (const Fields &fields : current)
            onRadio[t.key].append({show(t.columns[0].id, fields[0].second)});
        if (current.isEmpty() && !rows.isEmpty())
            throw RadioError(QString("%1: the radio's table is empty, so field sizes are unknown; "
                                     "add one row with the Hytera app first").arg(t.title));

        auto encode = [&](int row, int col) {
            QByteArray raw;
            QString why;
            const QString text = rows[row].value(col);
            if (!parse(t.columns[col].id, text, current[0][col].second.size(), &raw, &why))
                throw RadioError(QString("%1, row %2, %3: \"%4\" is %5")
                                     .arg(t.title).arg(row + 1).arg(t.columns[col].name, text, why));
            return raw;
        };

        for (int row = 0; row < rows.size(); ++row) {
            Fields values;
            if (row < current.size()) {
                for (int col = 0; col < t.columns.size(); ++col) {
                    const QString have = show(t.columns[col].id, current[row][col].second);
                    const QString want = rows[row].value(col).trimmed();
                    if (want == have)
                        continue;
                    values.append({t.columns[col].id, encode(row, col)});
                    log->append(QString("%1 %2: %3  %4 -> %5")
                                    .arg(t.title).arg(row + 1).arg(t.columns[col].name, have, want));
                }
                if (values.isEmpty())
                    continue;
                if (write) {
                    step("Writing changes", pending + 1, 0);
                    radio.writeRow(row, values);
                }
            } else {
                for (int col = 0; col < t.columns.size(); ++col)
                    values.append({t.columns[col].id, encode(row, col)});
                log->append(QString("%1 %2: add \"%3\"").arg(t.title).arg(row + 1).arg(rows[row].value(0)));
                if (write) {
                    step("Writing changes", pending + 1, 0);
                    radio.addRow(values);
                }
            }
            ++pending;
        }
        for (int row = current.size() - 1; row >= rows.size(); --row) {
            log->append(QString("%1 %2: delete \"%3\"")
                            .arg(t.title).arg(row + 1).arg(show(t.columns[0].id, current[row][0].second)));
            if (write) {
                step("Writing changes", pending + 1, 0);
                radio.deleteRow(t.keyId, row);
            }
            ++pending;
        }

        if (!t.extraName)
            continue;
        // Zone membership. Channels can only be appended or removed by position, so keep the
        // common leading part, drop the rest from the end, and append what is missing.
        const auto radioMembers = write ? QList<Members>() : radio.readZoneChannels(current.size());
        for (int z = 0; z < rows.size(); ++z) {
            if (rows[z].size() <= t.columns.size())
                continue;                               // no membership given for this zone
            const Members want = resolveMembers(rows[z][t.columns.size()], rows[z].value(2).trimmed() == "1",
                                                wanted, z);
            // When writing, read each zone fresh: rows may just have been added or removed.
            const Members have = write ? Members(radio.readZoneChannels1(z)) : radioMembers.value(z);
            if (have == want)
                continue;
            // After writing, positions on the radio are those of `wanted`; before, the radio's own.
            log->append(QString("%1 %2: channels  %3 -> %4").arg(t.title).arg(z + 1)
                            .arg(memberNames(have, write ? wanted : onRadio), memberNames(want, wanted)));
            ++pending;
            if (!write)
                continue;
            // Channels can only be removed by position or appended. Walk the radio's list keeping
            // every entry that is next in the wanted order, remove the others (from the end, so
            // positions stay valid), then append whatever is still missing.
            QList<int> remove;
            int next = 0;
            for (int entry = 0; entry < have.size(); ++entry) {
                if (next < want.size() && have[entry] == want[next])
                    ++next;
                else
                    remove.append(entry);
            }
            const int steps = remove.size() + want.size() - next;
            int done = 0;
            for (int i = remove.size() - 1; i >= 0; --i) {
                step(QString("Updating zone %1").arg(z + 1), ++done, steps);
                radio.deleteZoneChannel(z, remove[i]);
            }
            for (int i = next; i < want.size(); ++i) {
                step(QString("Updating zone %1").arg(z + 1), ++done, steps);
                radio.addZoneChannel(z, want[i].first, want[i].second);
            }
        }
    }
    return pending;
}
