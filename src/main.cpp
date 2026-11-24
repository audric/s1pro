// s1pro: CHIRP-style editor for the Hytera S1 Pro.
//
//   s1pro [file.s1pro]     open the editor
//   s1pro --read           open the editor and read the radio straight away
//   s1pro info             print radio identity and table sizes
//   s1pro dump             print every table, tab-separated
#include "tables.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QLockFile>
#include <QMainWindow>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QSpinBox>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextStream>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>
#include <functional>
#ifdef Q_OS_WIN
#include <windows.h>
#include <cstdio>
#endif

// Connect, run the job, disconnect. The Bluetooth link drops now and then, so the whole job is
// retried; every job is safe to repeat (a read, or a sync that re-reads the radio first).
template <typename Job>
auto withRadio(QString address, const std::function<void(const QString &)> &report, Job job)
{
    // One radio session at a time across all running copies: two at once share the same
    // Bluetooth connection and corrupt each other's exchange.
    QLockFile lock(QDir::tempPath() + "/s1pro-radio.lock");
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0))
        throw RadioError("Another s1pro window is using the radio. Wait for it to finish.");

    QSettings settings("s1pro", "s1pro");
    if (address.isEmpty())
        address = settings.value("address").toString();
    if (address.isEmpty()) {
        report("Scanning for the radio…");
        QString name;
        address = Radio::find(12000, &name);
        if (address.isEmpty())
            throw RadioError("No S1 Pro found: is it on, with Bluetooth enabled and no phone connected?");
        report("Found " + name + " at " + address);
    }
    for (int attempt = 1;; ++attempt) {
        try {
            Radio radio;
            report("Connecting to the radio…");
            radio.open(address);
            settings.setValue("address", address);
            return job(radio);
        } catch (const RadioError &e) {
            if (attempt == 3)
                throw;
            report(QString("%1; retrying (%2/2)").arg(e.what()).arg(attempt));
        }
    }
}

class Grid : public QTableWidget {
    Q_OBJECT
public:
    explicit Grid(const Table &table)
        : QTableWidget(0, table.columns.size() + (table.extraHeader ? 1 : 0)), table_(table)
    {
        QStringList headers;
        for (const Column &c : table.columns)
            headers << (c.header ? c.header : c.name);
        if (table.extraHeader)
            headers << table.extraHeader;
        setHorizontalHeaderLabels(headers);
        setAlternatingRowColors(true);
        connect(this, &QTableWidget::itemChanged, this, &Grid::edited);
        connect(this, &QTableWidget::cellDoubleClicked, this, [this](int row, int column) {
            if (column >= table_.columns.size())
                emit extraRequested(row);
        });
    }

    QString extra(int row) const { return item(row, table_.columns.size())->text(); }
    void setExtra(int row, const QString &text)
    {
        if (text != extra(row))
            item(row, table_.columns.size())->setText(text);     // emits edited()
    }
    QStringList firstColumn() const
    {
        QStringList out;
        for (int r = 0; r < rowCount(); ++r)
            out << item(r, 0)->text();
        return out;
    }
    QString cell(int row, const char *name) const
    {
        for (int c = 0; c < table_.columns.size(); ++c)
            if (QLatin1String(table_.columns[c].name) == QLatin1String(name))
                return item(row, c)->text();
        return {};
    }

    Rows rows() const
    {
        Rows out;
        for (int r = 0; r < rowCount(); ++r)
            out.append(rowAt(r));
        return out;
    }

    void load(const Rows &rows)
    {
        const QSignalBlocker block(this);
        setRowCount(0);
        for (const QStringList &row : rows)
            insert(rowCount(), row);
        resizeColumnsToContents();
    }

    // A new row starts as a copy of the current (or last) one: every field needs a valid value.
    void addRow()
    {
        if (!rowCount()) {
            QMessageBox::information(this, "Add row", "Read from the radio first: a new row is "
                                                      "copied from an existing one.");
            return;
        }
        const QList<int> sel = selectedRows();
        const int src = sel.isEmpty() ? rowCount() - 1 : sel.last();
        QStringList values = editable(rowAt(src));
        values[0].clear();
        insert(src + 1, values);
        setCurrentCell(src + 1, 0);
        editItem(item(src + 1, 0));
        emit edited();
    }

    void duplicateRows()
    {
        const QList<int> sel = selectedRows();
        for (int n = 0; n < sel.size(); ++n)
            insert(sel.last() + 1 + n, editable(rowAt(sel[n])));
        if (!sel.isEmpty())
            emit edited();
    }

    void deleteRows()
    {
        const QList<int> sel = selectedRows();
        for (int i = sel.size() - 1; i >= 0; --i)
            removeRow(sel[i]);
        if (!sel.isEmpty())
            emit edited();
    }

    void moveRows(int step)
    {
        const QList<int> sel = selectedRows();
        if (sel.isEmpty() || sel.first() + step < 0 || sel.last() + step >= rowCount())
            return;
        const int column = qMax(currentColumn(), 0);
        for (int i = 0; i < sel.size(); ++i) {
            const int r = step < 0 ? sel[i] : sel[sel.size() - 1 - i];
            const QStringList values = rowAt(r);
            removeRow(r);
            insert(r + step, values);
        }
        clearSelection();
        setCurrentCell(sel.first() + step, column, QItemSelectionModel::NoUpdate);
        setRangeSelected({sel.first() + step, 0, sel.last() + step, columnCount() - 1}, true);
        emit edited();
    }

    // Clipboard is tab-separated text, so it also pastes to and from a spreadsheet.
    void copy() const
    {
        const QModelIndexList cells = selectedIndexes();
        if (cells.isEmpty())
            return;
        int r0 = rowCount(), r1 = 0, c0 = columnCount(), c1 = 0;
        for (const QModelIndex &i : cells) {
            r0 = qMin(r0, i.row()), r1 = qMax(r1, i.row());
            c0 = qMin(c0, i.column()), c1 = qMax(c1, i.column());
        }
        QStringList lines;
        for (int r = r0; r <= r1; ++r)
            lines << rowAt(r).mid(c0, c1 - c0 + 1).join('\t');
        QApplication::clipboard()->setText(lines.join('\n'));
    }

    void cut()
    {
        copy();
        if (wholeRowsSelected()) {
            deleteRows();
            return;
        }
        for (QTableWidgetItem *cell : selectedItems())
            if (cell->column() < table_.columns.size())
                cell->setText({});
    }

    // Pastes at the current cell; rows past the end are created (as copies of the last row,
    // then overwritten), so pasting a block of whole rows appends channels.
    void paste()
    {
        QString text = QApplication::clipboard()->text();
        while (text.endsWith('\n') || text.endsWith('\r'))
            text.chop(1);
        if (text.isEmpty())
            return;
        const QStringList lines = text.split('\n');
        const int r0 = qMax(currentRow(), 0);
        int c0 = qMax(currentColumn(), 0);
        if (lines.first().split('\t').size() == columnCount())
            c0 = 0;
        {
            const QSignalBlocker block(this);
            for (int dr = 0; dr < lines.size(); ++dr) {
                const int r = r0 + dr;
                if (r >= rowCount())
                    insert(r, r ? editable(rowAt(r - 1)) : QStringList());
                QString line = lines[dr];
                if (line.endsWith('\r'))
                    line.chop(1);
                const QStringList cells = line.split('\t');
                for (int dc = 0; dc < cells.size() && c0 + dc < table_.columns.size(); ++dc)
                    item(r, c0 + dc)->setText(cells[dc]);
            }
        }
        emit edited();
    }

    // Appends simplex analog channels, skipping frequencies already in the table.
    // Each new row is a copy of the last row with name, frequencies and tones replaced and the
    // repeater-related fields set the way the radio's own simplex channels have them.
    int addSimplexChannels(const QList<QPair<QString, QString>> &channels)
    {
        if (!rowCount())
            return -1;
        auto col = [this](const char *name) {
            for (int c = 0; c < table_.columns.size(); ++c)
                if (QLatin1String(table_.columns[c].name) == QLatin1String(name))
                    return c;
            return -1;
        };
        const int rx = col("rxFreq"), tx = col("txFreq");
        const QList<QPair<const char *, const char *>> fixed = {
            {"RXCTCorCDCType", "0"}, {"TXCTCorCDCType", "0"}, {"RXCTCorCDCValue", "67"},
            {"TXCTCorCDCValue", "67"}, {"TalkAround", "0"}, {"TalkAroundState", "1"},
            {"ChannelModeStatus", "0"}};
        int added = 0;
        for (const auto &[name, mhz] : channels) {
            bool present = false;
            for (int r = 0; r < rowCount() && !present; ++r)
                present = item(r, rx)->text().toDouble() == mhz.toDouble()
                          && item(r, tx)->text().toDouble() == mhz.toDouble();
            if (present)
                continue;
            QStringList values = rowAt(rowCount() - 1);
            values[0] = name;
            values[rx] = values[tx] = mhz;
            for (const auto &[field, value] : fixed)
                if (col(field) >= 0)
                    values[col(field)] = value;
            insert(rowCount(), values);
            ++added;
        }
        if (added) {
            resizeColumnsToContents();
            scrollToBottom();
            emit edited();
        }
        return added;
    }

signals:
    void edited();
    void extraRequested(int row);

private:
    const Table &table_;

    QStringList rowAt(int r) const
    {
        QStringList out;
        for (int c = 0; c < columnCount(); ++c)
            out << (item(r, c) ? item(r, c)->text() : QString());
        return out;
    }

    void insert(int at, const QStringList &values)
    {
        const QSignalBlocker block(this);
        insertRow(at);
        for (int c = 0; c < columnCount(); ++c) {
            auto *cell = new QTableWidgetItem(values.value(c));
            if (c >= table_.columns.size())
                cell->setFlags(cell->flags() & ~Qt::ItemIsEditable);
            setItem(at, c, cell);
        }
    }

    // A row's values without the display-only column.
    QStringList editable(const QStringList &row) const { return row.mid(0, table_.columns.size()); }

    QList<int> selectedRows() const
    {
        QList<int> out;
        for (const QModelIndex &i : selectedIndexes())
            if (!out.contains(i.row()))
                out.append(i.row());
        std::sort(out.begin(), out.end());
        return out;
    }

    bool wholeRowsSelected() const
    {
        const int n = selectedRows().size();
        return n && selectedIndexes().size() == n * columnCount();
    }
};

// Licence-free band plans, as (first channel MHz in Hz, step in Hz, channel count).
struct BandPlan {
    const char *title;
    const char *prefix;
    QList<quint32> hz;
    const char *note;
};

static QList<BandPlan> bandPlans()
{
    auto series = [](quint32 first, quint32 step, int count) {
        QList<quint32> out;
        for (int i = 0; i < count; ++i)
            out.append(first + step * i);
        return out;
    };
    return {
        {"PMR446 (16 channels, 446 MHz)", "PMR", series(446006250, 12500, 16),
         "PMR446 allows 0.5 W ERP with a fixed antenna; channels 9-16 are not allowed everywhere."},
        {"LPD433 (69 channels, 433 MHz)", "LPD", series(433075000, 25000, 69),
         "LPD433 allows 10 mW ERP."},
    };
}

static QString mhzText(quint32 hz)
{
    QString frac = QString::number(hz % 1000000).rightJustified(6, '0');
    while (frac.endsWith('0'))
        frac.chop(1);
    return QString::number(hz / 1000000) + (frac.isEmpty() ? QString() : "." + frac);
}

class Window : public QMainWindow {
    Q_OBJECT
public:
    Window()
    {
        tabs_ = new QTabWidget;
        for (const Table &t : tables()) {
            Grid *g = new Grid(t);
            connect(g, &Grid::edited, this, [this] { setDirty(true); });
            if (t.extraName)
                connect(g, &Grid::extraRequested, this, [this, g](int row) { editZoneChannels(g, row); });
            grids_.append(g);
            tabs_->addTab(g, t.title);
        }
        setCentralWidget(tabs_);
        resize(1250, 620);

        QToolBar *bar = addToolBar("Main");
        bar->setMovable(false);
        auto action = [&](QMenu *menu, const QString &text, const QKeySequence &key, bool onBar,
                          std::function<void()> slot) {
            QAction *a = menu->addAction(text);
            a->setShortcut(key);
            connect(a, &QAction::triggered, this, slot);
            if (onBar)
                bar->addAction(a);
            return a;
        };
        QMenu *file = menuBar()->addMenu("&File");
        QMenu *edit = menuBar()->addMenu("&Edit");
        QMenu *radio = menuBar()->addMenu("&Radio");
        action(file, "&Open…", QKeySequence::Open, false, [this] { open({}); });
        action(file, "&Save", QKeySequence::Save, false, [this] { save(); });
        action(file, "Save &as…", QKeySequence("Ctrl+Shift+S"), false, [this] { saveAs(); });
        action(file, "&Quit", QKeySequence::Quit, false, [this] { close(); });
        action(radio, "&Read from radio", QKeySequence("Ctrl+R"), true, [this] { readRadio(); });
        action(radio, "&Write to radio…", QKeySequence("Ctrl+W"), true, [this] { writeRadio(); });
        action(radio, "&Forget radio address", {}, false, [this] {
            QSettings("s1pro", "s1pro").remove("address");
            statusBar()->showMessage("The next read or write will scan for the radio.");
        });
        bar->addSeparator();
        action(edit, "Cu&t", QKeySequence::Cut, false, [this] { grid()->cut(); });
        action(edit, "&Copy", QKeySequence::Copy, false, [this] { grid()->copy(); });
        action(edit, "&Paste", QKeySequence::Paste, false, [this] { grid()->paste(); });
        edit->addSeparator();
        action(edit, "&Add row", QKeySequence("Ctrl+N"), true, [this] { grid()->addRow(); });
        action(edit, "D&uplicate rows", QKeySequence("Ctrl+D"), true, [this] { grid()->duplicateRows(); });
        action(edit, "&Delete rows", QKeySequence("Ctrl+Del"), true, [this] { grid()->deleteRows(); });
        action(edit, "Move &up", QKeySequence("Alt+Up"), true, [this] { grid()->moveRows(-1); });
        action(edit, "Move dow&n", QKeySequence("Alt+Down"), true, [this] { grid()->moveRows(1); });
        edit->addSeparator();
        bar->addSeparator();
        action(edit, "&Import PMR / LPD…", QKeySequence("Ctrl+I"), true, [this] { importBand(); });

        setDirty(false);
        statusBar()->showMessage("Read from radio to start, or open a saved file.");
    }

    void open(QString path)
    {
        if (path.isEmpty())
            path = QFileDialog::getOpenFileName(this, "Open", {}, "S1 Pro files (*.s1pro);;All files (*)");
        if (path.isEmpty())
            return;
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            QMessageBox::critical(this, "Open", f.errorString());
            return;
        }
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        Codeplug plug;
        present_.clear();
        zoneChannelsKnown_ = true;
        for (const Table &t : tables()) {
            if (!root.contains(t.key))
                continue;                               // older file: leave that table alone on the radio
            present_.insert(t.key);
            Rows rows;
            for (const QJsonValue &rowValue : root[t.key].toArray()) {
                if (t.extraName && !rowValue.toObject().contains(t.extraName))
                    zoneChannelsKnown_ = false;
                const QJsonObject row = rowValue.toObject();
                QStringList cells;
                for (const Column &c : t.columns)
                    cells << row[c.name].toString();
                if (t.extraName)
                    cells << row[t.extraName].toString();
                rows.append(cells);
            }
            plug[t.key] = rows;
        }
        load(plug);
        path_ = path;
        setDirty(false);
    }

    void readRadio()
    {
        if (dirty_ && QMessageBox::question(this, "Read from radio",
                                            "Replace the tables with the radio's contents?") != QMessageBox::Yes)
            return;
        Codeplug plug;
        const bool ok = radioJob("Reading from radio…", [&](Radio &radio) {
            plug = readCodeplug(radio);
            return 0;
        });
        if (!ok)
            return;
        load(plug);
        present_.clear();
        for (const Table &t : tables())
            present_.insert(t.key);
        zoneChannelsKnown_ = true;
        setDirty(false);
        QStringList parts;
        for (const Table &t : tables())
            parts << QString("%1 %2").arg(plug[t.key].size()).arg(QString(t.title).toLower());
        statusBar()->showMessage("Read " + parts.join(", ") + ".");
    }

protected:
    void closeEvent(QCloseEvent *e) override
    {
        if (busy_ || (dirty_ && QMessageBox::question(this, "Quit", "Discard unsaved changes?")
                                    != QMessageBox::Yes))
            e->ignore();
        else
            e->accept();
    }

private:
    Grid *grid() { return static_cast<Grid *>(tabs_->currentWidget()); }

    void setDirty(bool dirty)
    {
        dirty_ = dirty;
        setWindowTitle(QString("%1%2 — S1 Pro editor").arg(dirty ? "*" : "", path_.isEmpty() ? "untitled" : path_));
    }

    Codeplug codeplug() const
    {
        // Only tables that were actually read or loaded are synced: an empty grid that was never
        // filled must not be taken as "delete everything".
        Codeplug plug;
        for (int i = 0; i < grids_.size(); ++i) {
            const Table &t = tables()[i];
            if (!present_.contains(t.key))
                continue;
            Rows rows = grids_[i]->rows();
            if (t.extraName && !zoneChannelsKnown_)
                for (QStringList &row : rows)
                    row = row.mid(0, t.columns.size());
            plug[t.key] = rows;
        }
        return plug;
    }

    void load(const Codeplug &plug)
    {
        for (int i = 0; i < grids_.size(); ++i)
            grids_[i]->load(plug.value(tables()[i].key));
    }

    void save()
    {
        if (path_.isEmpty())
            return saveAs();
        QJsonObject root;
        const Codeplug plug = codeplug();
        for (const Table &t : tables()) {
            QJsonArray rows;
            for (const QStringList &cells : plug[t.key]) {
                QJsonObject row;
                for (int c = 0; c < t.columns.size(); ++c)
                    row[t.columns[c].name] = cells.value(c);
                if (t.extraName)
                    row[t.extraName] = cells.value(t.columns.size());
                rows.append(row);
            }
            root[t.key] = rows;
        }
        QFile f(path_);
        if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(root).toJson()) < 0) {
            QMessageBox::critical(this, "Save", f.errorString());
            return;
        }
        setDirty(false);
    }

    void saveAs()
    {
        QString path = QFileDialog::getSaveFileName(this, "Save as", path_.isEmpty() ? "radio.s1pro" : path_,
                                                    "S1 Pro files (*.s1pro)");
        if (path.isEmpty())
            return;
        if (!path.endsWith(".s1pro"))
            path += ".s1pro";
        path_ = path;
        save();
    }

    Grid *gridFor(const char *key)
    {
        for (int i = 0; i < tables().size(); ++i)
            if (QLatin1String(tables()[i].key) == QLatin1String(key))
                return grids_[i];
        return nullptr;
    }

    // Pick and order the channels of one zone. The zone stores channel names; they are matched
    // to positions when writing, so moving channel rows around later does not break zones.
    void editZoneChannels(Grid *zones, int row)
    {
        if (!zoneChannelsKnown_) {
            QMessageBox::information(this, "Zone channels", "This file was saved without zone channels. "
                                                            "Read from the radio to edit them.");
            return;
        }
        const bool analog = zones->cell(row, "ZoneType").trimmed() == "1";
        const QStringList all = gridFor(analog ? "analog" : "digital")->firstColumn();
        QStringList members;
        for (const QString &name : zones->extra(row).split(',', Qt::SkipEmptyParts))
            members << name.trimmed();

        QDialog dialog(this);
        dialog.setWindowTitle(QString("Channels in zone \"%1\"").arg(zones->cell(row, "ZoneAlias")));
        auto *available = new QListWidget, *chosen = new QListWidget;
        available->setSelectionMode(QAbstractItemView::ExtendedSelection);
        chosen->setSelectionMode(QAbstractItemView::ExtendedSelection);
        for (const QString &name : all)
            if (!members.contains(name))
                available->addItem(name);
        chosen->addItems(members);

        auto *add = new QPushButton("Add →"), *remove = new QPushButton("← Remove");
        auto *up = new QPushButton("Up"), *down = new QPushButton("Down");
        connect(add, &QPushButton::clicked, &dialog, [&] {
            for (QListWidgetItem *i : available->selectedItems()) {
                chosen->addItem(i->text());
                delete i;
            }
        });
        connect(remove, &QPushButton::clicked, &dialog, [&] {
            for (QListWidgetItem *i : chosen->selectedItems())
                delete i;
            available->clear();
            QStringList now;
            for (int i = 0; i < chosen->count(); ++i)
                now << chosen->item(i)->text();
            for (const QString &name : all)
                if (!now.contains(name))
                    available->addItem(name);
        });
        auto move = [&](int step) {
            const int from = chosen->currentRow(), to = from + step;
            if (from < 0 || to < 0 || to >= chosen->count())
                return;
            chosen->insertItem(to, chosen->takeItem(from));
            chosen->setCurrentRow(to);
        };
        connect(up, &QPushButton::clicked, &dialog, [&] { move(-1); });
        connect(down, &QPushButton::clicked, &dialog, [&] { move(1); });

        auto *middle = new QVBoxLayout;
        middle->addStretch();
        for (QPushButton *b : {add, remove, up, down})
            middle->addWidget(b);
        middle->addStretch();
        auto *lists = new QHBoxLayout;
        auto column = [](const QString &title, QWidget *w) {
            auto *box = new QVBoxLayout;
            box->addWidget(new QLabel(title));
            box->addWidget(w);
            return box;
        };
        lists->addLayout(column(analog ? "Analog channels not in this zone" : "Digital channels not in this zone",
                                available));
        lists->addLayout(middle);
        lists->addLayout(column("In this zone, in order", chosen));
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        auto *layout = new QVBoxLayout(&dialog);
        layout->addLayout(lists);
        layout->addWidget(buttons);
        dialog.resize(640, 480);
        if (dialog.exec() != QDialog::Accepted)
            return;
        QStringList result;
        for (int i = 0; i < chosen->count(); ++i)
            result << chosen->item(i)->text();
        zones->setExtra(row, result.join(", "));
    }

    void importBand()
    {
        const QList<BandPlan> plans = bandPlans();
        QDialog dialog(this);
        dialog.setWindowTitle("Import licence-free channels");
        auto *form = new QFormLayout(&dialog);
        auto *band = new QComboBox;
        for (const BandPlan &p : plans)
            band->addItem(p.title);
        auto *from = new QSpinBox, *to = new QSpinBox;
        auto *note = new QLabel;
        note->setWordWrap(true);
        note->setMinimumWidth(420);
        auto update = [&] {
            const BandPlan &p = plans[band->currentIndex()];
            from->setRange(1, p.hz.size());
            to->setRange(1, p.hz.size());
            from->setValue(1);
            to->setValue(p.hz.size());
            note->setText(QString("%1 MHz to %2 MHz. %3\n\nChannels are added to the Analog tab as "
                                  "simplex, without tones; frequencies already present are skipped. "
                                  "Transmit power is a radio-wide setting and is not changed here.")
                              .arg(mhzText(p.hz.first()), mhzText(p.hz.last()), p.note));
        };
        connect(band, &QComboBox::currentIndexChanged, &dialog, update);
        update();
        form->addRow("Band", band);
        form->addRow("First channel", from);
        form->addRow("Last channel", to);
        form->addRow(note);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
        buttons->addButton("Import", QDialogButtonBox::AcceptRole);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        form->addRow(buttons);
        if (dialog.exec() != QDialog::Accepted)
            return;

        const BandPlan &p = plans[band->currentIndex()];
        QList<QPair<QString, QString>> channels;
        for (int n = from->value(); n <= to->value(); ++n)
            channels.append({QString("%1 %2").arg(p.prefix).arg(n), mhzText(p.hz[n - 1])});
        int analog = 0;
        for (int i = 0; i < tables().size(); ++i)
            if (QLatin1String(tables()[i].key) == QLatin1String("analog"))
                analog = i;
        tabs_->setCurrentIndex(analog);
        const int added = grids_[analog]->addSimplexChannels(channels);
        if (added < 0)
            QMessageBox::information(this, "Import", "Read from the radio first: imported channels "
                                                     "are modelled on an existing analog channel.");
        else
            statusBar()->showMessage(QString("Added %1 channel(s), skipped %2 already present. "
                                             "Nothing is on the radio until you write.")
                                         .arg(added).arg(channels.size() - added));
    }

    // Radio jobs run on the GUI thread; they pump the event loop while waiting, so the window
    // stays alive. The editor is locked meanwhile.
    // `total` is the number of write steps when known, so the bar can show real progress.
    template <typename Job>
    bool radioJob(const QString &message, Job job, int total = 0)
    {
        if (busy_)
            return false;
        busy_ = true;
        QProgressDialog box(message, QString(), 0, 0, this);
        box.setWindowTitle("Radio");
        box.setWindowModality(Qt::WindowModal);
        box.setMinimumDuration(0);
        box.setMinimumWidth(420);
        box.setAutoClose(false);
        box.setAutoReset(false);
        box.setWindowFlag(Qt::WindowCloseButtonHint, false);
        box.show();
        QApplication::setOverrideCursor(Qt::BusyCursor);
        auto say = [&](const QString &text) {
            box.setRange(0, 0);                         // moving bar: no count for this phase
            box.setLabelText(text);
            statusBar()->showMessage(text);
        };
        setProgress([&](const QString &what, int done, int stepTotal) {
            const int outOf = stepTotal ? stepTotal : total;
            box.setRange(0, outOf);
            box.setValue(qMin(done, outOf));
            const QString text = outOf ? QString("%1… %2 of %3").arg(what).arg(done).arg(outOf)
                                       : QString("%1… %2").arg(what).arg(done);
            box.setLabelText(text);
            statusBar()->showMessage(text);
        });
        menuBar()->setEnabled(false);
        centralWidget()->setEnabled(false);
        for (QToolBar *bar : findChildren<QToolBar *>())
            bar->setEnabled(false);
        statusBar()->showMessage(message);
        QString error;
        try {
            withRadio({}, say, job);
        } catch (const RadioError &e) {
            error = e.what();
        }
        setProgress(nullptr);
        QApplication::restoreOverrideCursor();
        box.close();
        busy_ = false;
        menuBar()->setEnabled(true);
        centralWidget()->setEnabled(true);
        for (QToolBar *bar : findChildren<QToolBar *>())
            bar->setEnabled(true);
        statusBar()->showMessage(error.isEmpty() ? "Done." : "Failed.");
        if (!error.isEmpty()) {
            QMessageBox::critical(this, "Radio", error);
        }
        return error.isEmpty();
    }

    void writeRadio()
    {
        const Codeplug wanted = codeplug();
        QStringList changes;
        int count = 0;
        if (!radioJob("Comparing with radio…", [&](Radio &radio) {
                changes.clear();
                return count = sync(radio, wanted, false, &changes);
            }))
            return;
        if (!count) {
            statusBar()->showMessage("The radio already matches; nothing to write.");
            return;
        }

        QDialog dialog(this);
        dialog.setWindowTitle("Write to radio");
        auto *layout = new QVBoxLayout(&dialog);
        layout->addWidget(new QLabel(QString("%1 row(s) will be changed on the radio:").arg(count)));
        auto *list = new QPlainTextEdit(changes.join('\n'));
        list->setReadOnly(true);
        list->setMinimumSize(640, 320);
        layout->addWidget(list);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
        buttons->addButton("Write", QDialogButtonBox::AcceptRole);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(buttons);
        if (dialog.exec() != QDialog::Accepted) {
            statusBar()->showMessage("Nothing written.");
            return;
        }

        int left = 0;
        QStringList remaining;
        if (!radioJob("Writing to radio…", [&](Radio &radio) {
                QStringList ignored;
                sync(radio, wanted, true, &ignored);
                remaining.clear();
                return left = sync(radio, wanted, false, &remaining);   // re-read and compare
            }, count))
            return;
        statusBar()->showMessage(left ? QString("Written, but %1 row(s) still differ.").arg(left)
                                      : QString("Written and verified."));
        if (left)
            QMessageBox::warning(this, "Write to radio",
                                 QString("%1 row(s) still differ after writing:\n\n%2").arg(left).arg(remaining.join('\n')));
    }

    QTabWidget *tabs_;
    QList<Grid *> grids_;
    QString path_;
    bool dirty_ = false, busy_ = false;
    QSet<QString> present_;             // tables holding real data (read from radio or loaded)
    bool zoneChannelsKnown_ = false;
};

static int commandLine(const QString &command)
{
    QTextStream out(stdout), err(stderr);
    try {
        withRadio({}, [&](const QString &m) { err << m << Qt::endl; }, [&](Radio &radio) {
            if (command == "info") {
                out << "firmware: " << radio.query(1) << "\nserial:   " << radio.query(11)
                    << "\ncodeplug: " << radio.query(2) << "\n";
                for (const Table &t : tables())
                    out << t.title << ": " << radio.rowCount(t.keyId) << " rows\n";
            } else if (command == "zone-add" || command == "zone-del") {
                const QStringList a = QCoreApplication::arguments().mid(2);
                if (command == "zone-add" && a.size() == 3)
                    radio.addZoneChannel(a[0].toInt() - 1, a[1].toInt(), a[2].toInt());
                else if (command == "zone-del" && a.size() == 2)
                    radio.deleteZoneChannel(a[0].toInt() - 1, a[1].toInt() - 1);
                else
                    throw RadioError("usage: zone-add ZONE CHANNEL TYPE | zone-del ZONE POSITION (all from 1)");
                out << "ok\n";
            } else if (command == "zones") {
                const int n = radio.rowCount(1325);
                for (int z = 0; z < n; ++z) {
                    out << "zone " << z + 1 << ":";
                    for (const auto &[channel, type] : radio.readZoneChannels1(z))
                        out << " " << (type ? "A" : "D") << channel;
                    out << "\n";
                }
            } else {
                for (const Table &t : tables()) {
                    QStringList names;
                    for (const Column &c : t.columns)
                        names << c.name;
                    out << "# " << t.title << "\n" << names.join('\t') << "\n";
                    for (QStringList row : readTable(radio, t)) {
                        if (t.extraName)
                            row << "";
                        out << row.join('\t') << "\n";
                    }
                }
            }
            return 0;
        });
    } catch (const RadioError &e) {
        err << "error: " << e.what() << Qt::endl;
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
#ifdef Q_OS_WIN
    // A windowed program has no console; borrow the terminal it was started from, if any,
    // so that "s1pro info" and friends can print.
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
#endif
    const QString first = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    if (QStringList{"info", "dump", "zones", "zone-add", "zone-del"}.contains(first)) {
        QCoreApplication app(argc, argv);
        return commandLine(first);
    }
    QApplication app(argc, argv);
    Window window;
    window.show();
    if (first == "--read")
        QTimer::singleShot(0, &window, [&window] { window.readRadio(); });
    else if (!first.isEmpty())
        window.open(first);
    return app.exec();
}

#include "main.moc"
