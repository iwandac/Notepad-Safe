#include "CommandPaletteModel.h"

#include <QtTest/QTest>

class TestCommandPaletteModel final : public QObject {
    Q_OBJECT

private slots:
    void prefixBeatsSubstring();
    void groupsKeepOrder();
    void disabledEntryDoesNotActivate();
    void mnemonicIsIgnored();
    void emptyQueryCapsGroups();
};

namespace {
using Group = PaletteEntry::Group;

PaletteEntry entry(Group group, const QString &title, bool enabled = true,
                   std::function<void()> activate = {})
{
    return {group, title, {}, {}, enabled, std::move(activate)};
}

QStringList titles(const CommandPaletteModel &model)
{
    QStringList result;
    for (int row = 0; row < model.rowCount(); ++row)
        result << model.index(row).data(Qt::DisplayRole).toString();
    return result;
}
} // namespace

void TestCommandPaletteModel::prefixBeatsSubstring()
{
    CommandPaletteModel model;
    model.setEntries({entry(Group::Command, QStringLiteral("全部保存")),
                      entry(Group::Command, QStringLiteral("保存")),
                      entry(Group::Command, QStringLiteral("关闭"))});
    model.setQuery(QStringLiteral("保存"));
    QCOMPARE(titles(model), (QStringList{QStringLiteral("保存"), QStringLiteral("全部保存")}));
}

void TestCommandPaletteModel::groupsKeepOrder()
{
    CommandPaletteModel model;
    model.setEntries({entry(Group::Recent, QStringLiteral("notes.txt")),
                      entry(Group::Command, QStringLiteral("Notes 设置")),
                      entry(Group::OpenDocument, QStringLiteral("notes.md")),
                      entry(Group::OpenDocument, QStringLiteral("my notes"))});
    model.setQuery(QStringLiteral("NOTES"));
    // Prefix matches first, each rank in group order: documents, commands, recent.
    QCOMPARE(titles(model), (QStringList{QStringLiteral("notes.md"),
                                         QStringLiteral("Notes 设置"),
                                         QStringLiteral("notes.txt"),
                                         QStringLiteral("my notes")}));
}

void TestCommandPaletteModel::disabledEntryDoesNotActivate()
{
    int runs = 0;
    CommandPaletteModel model;
    model.setEntries({entry(Group::Command, QStringLiteral("替换"), false, [&runs] { ++runs; }),
                      entry(Group::Command, QStringLiteral("查找"), true, [&runs] { runs += 10; })});
    model.setQuery({});
    QCOMPARE(model.rowCount(), 2);
    QVERIFY(!model.index(0).data(CommandPaletteModel::EnabledRole).toBool());
    QVERIFY(!model.activate(0));
    QCOMPARE(runs, 0);
    QVERIFY(model.activate(1));
    QCOMPARE(runs, 10);
}

void TestCommandPaletteModel::mnemonicIsIgnored()
{
    CommandPaletteModel model;
    model.setEntries({entry(Group::Command, QStringLiteral("保存(&S)")),
                      entry(Group::Command, QStringLiteral("&Find..."))});
    model.setQuery(QStringLiteral("保存"));
    QCOMPARE(titles(model), QStringList{QStringLiteral("保存(S)")});
    model.setQuery(QStringLiteral("find"));
    QCOMPARE(titles(model), QStringList{QStringLiteral("Find...")});
}

void TestCommandPaletteModel::emptyQueryCapsGroups()
{
    QVector<PaletteEntry> entries;
    for (int i = 0; i < 8; ++i) {
        entries << entry(Group::OpenDocument, QStringLiteral("doc %1").arg(i))
                << entry(Group::Command, QStringLiteral("cmd %1").arg(i))
                << entry(Group::Recent, QStringLiteral("recent %1").arg(i));
    }
    CommandPaletteModel model;
    model.setEntries(entries);
    model.setQuery({});
    QCOMPARE(model.rowCount(), 15);
    QCOMPARE(model.index(0).data(Qt::DisplayRole).toString(), QStringLiteral("doc 0"));
    QCOMPARE(model.index(5).data(Qt::DisplayRole).toString(), QStringLiteral("cmd 0"));
    QCOMPARE(model.index(10).data(Qt::DisplayRole).toString(), QStringLiteral("recent 0"));
    model.setQuery(QStringLiteral("cmd"));
    QCOMPARE(model.rowCount(), 8);
}

QTEST_GUILESS_MAIN(TestCommandPaletteModel)
#include "test_command_palette_model.moc"
